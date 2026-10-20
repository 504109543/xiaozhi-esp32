#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <img_converters.h>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "display.h"
#include "esp32_camera.h"
#include "esp_timer.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"

#define TAG "Esp32Camera"

#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
#if CONFIG_XIAOZHI_CAMERA_HMIRROR
static constexpr bool kConfiguredHMirror = true;
#else
static constexpr bool kConfiguredHMirror = false;
#endif
#if CONFIG_XIAOZHI_CAMERA_VFLIP
static constexpr bool kConfiguredVFlip = true;
#else
static constexpr bool kConfiguredVFlip = false;
#endif
#endif

// Bytes of pixel data in a frame buffer
static size_t FrameDataSize(const camera_fb_t* fb) {
    if (fb->format == PIXFORMAT_RGB565) {
        return (size_t)fb->width * fb->height * 2;
    }
    return fb->len;
}

// Copy pixel data out of the driver frame buffer with optional RGB565 byte swapping
static void CopyFrame(const camera_fb_t* fb, uint8_t* dst, size_t size, bool swap_bytes) {
    if (fb->format == PIXFORMAT_RGB565 && swap_bytes) {
        size_t pixel_count = size / 2;
        const uint16_t* src = (const uint16_t*)fb->buf;
        uint16_t* dst16 = (uint16_t*)dst;
        for (size_t i = 0; i < pixel_count; i++) {
            dst16[i] = __builtin_bswap16(src[i]);
        }
    } else {
        memcpy(dst, fb->buf, size);
    }
}

static bool ToV4l2Format(pixformat_t format, v4l2_pix_fmt_t* out) {
    switch (format) {
        case PIXFORMAT_RGB565:
            *out = V4L2_PIX_FMT_RGB565;
            return true;
        case PIXFORMAT_YUV422:
            *out = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
            return true;
        case PIXFORMAT_YUV420:
            *out = V4L2_PIX_FMT_YUV420;
            return true;
        case PIXFORMAT_GRAYSCALE:
            *out = V4L2_PIX_FMT_GREY;
            return true;
        case PIXFORMAT_JPEG:
            *out = V4L2_PIX_FMT_JPEG;
            return true;
        case PIXFORMAT_RGB888:
            *out = V4L2_PIX_FMT_RGB24;
            return true;
        default:
            return false;
    }
}

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);  // Control camera mirror: 1 for mirror, 0 for normal
        }
#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
        s->set_hmirror(s, kConfiguredHMirror ? 1 : 0);
        s->set_vflip(s, kConfiguredVFlip ? 1 : 0);
#endif
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    if (streaming_on_) {
        std::lock_guard<std::mutex> lock(fb_mutex_);
        esp_camera_deinit();
        streaming_on_ = false;
    }
    if (encode_buf_) {
        heap_caps_free(encode_buf_);
        encode_buf_ = nullptr;
        encode_buf_size_ = 0;
    }
    has_frame_ = false;
}

// Get the latest frame, discard old frames for real-time performance.
// Caller must hold fb_mutex_ and return the frame with esp_camera_fb_return().
camera_fb_t* Esp32Camera::GrabLatestFrame() {
    camera_fb_t* fb = nullptr;
    for (int i = 0; i < 2; i++) {
        if (fb) {
            esp_camera_fb_return(fb);
        }
        fb = esp_camera_fb_get();
        if (!fb) {
            return nullptr;
        }
    }
    return fb;
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    has_frame_ = false;

    if (!streaming_on_) {
        return false;
    }

    int64_t start_time = esp_timer_get_time();
    // AI capture has priority: announce it so snapshot requests back off, then wait for the lock
    ai_capture_waiters_.fetch_add(1);
    std::unique_lock<std::mutex> lock(fb_mutex_);
    ai_capture_waiters_.fetch_sub(1);

    camera_fb_t* fb = GrabLatestFrame();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        return false;
    }

    size_t data_size = FrameDataSize(fb);

    // Allocate or reallocate encode buffer if needed
    if (encode_buf_size_ < data_size) {
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
        }
        encode_buf_ = (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (encode_buf_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
            encode_buf_size_ = 0;
            esp_camera_fb_return(fb);
            return false;
        }
        encode_buf_size_ = data_size;
    }

    // Copy data to encode buffer with optional byte swapping, then give the frame buffer back
    // to the driver so it is not held while JPEG encoding and uploading
    CopyFrame(fb, encode_buf_, data_size, swap_bytes_enabled_);
    frame_len_ = data_size;
    frame_width_ = fb->width;
    frame_height_ = fb->height;
    frame_format_ = fb->format;
    esp_camera_fb_return(fb);
    lock.unlock();
    int64_t grab_time = esp_timer_get_time() - start_time;

    if (frame_format_ == PIXFORMAT_RGB565) {
        // Allocate separate buffer for preview display
        uint8_t* preview_data =
            (uint8_t*)heap_caps_malloc(frame_len_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, encode_buf_, frame_len_);
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, frame_len_, frame_width_, frame_height_, frame_width_ * 2,
                    LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        }
    } else if (frame_format_ == PIXFORMAT_JPEG) {
        // JPEG format preview usually requires decoding, skip preview display for now, just log
        ESP_LOGW(TAG, "JPEG capture success, len=%zu, but not supported for preview", frame_len_);
    }

    has_frame_ = true;
    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%zu, format=%d, grab time: %d ms", frame_width_,
             frame_height_, frame_len_, frame_format_, int(grab_time / 1000));

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

std::expected<std::string, std::string> Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }

    if (!has_frame_) {
        return std::unexpected("No camera frame captured");
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return std::unexpected("Failed to create JPEG queue");
    }

    // Start encoding thread
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = frame_width_;
        uint16_t h = frame_height_;
        v4l2_pix_fmt_t enc_fmt;
        if (!ToV4l2Format(frame_format_, &enc_fmt)) {
            ESP_LOGE(TAG, "Unsupported pixel format: %d", frame_format_);
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
            return;
        }

        // The frame buffer was already returned to the driver, encode from the copy
        uint8_t* jpeg_src_buf = encode_buf_;
        size_t jpeg_src_len = frame_len_;

        bool ok = image_to_jpeg_cb(
            jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 80,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                JpegChunk chunk = {.data = nullptr, .len = len};
                if (index == 0 && data != nullptr && len > 0) {
                    chunk.data = (uint8_t*)heap_caps_aligned_alloc(
                        16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (chunk.data == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
                        chunk.len = 0;
                    } else {
                        memcpy(chunk.data, data, len);
                    }
                } else {
                    chunk.len = 0;  // Sentinel or error
                }
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return len;
            },
            jpeg_queue);

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "JPEG encoding time: %ld ms", int((end_time - start_time) / 1000));
    });

    auto drain_jpeg_queue = [this, jpeg_queue]() {
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        encoder_thread_.join();
        vQueueDelete(jpeg_queue);
    };

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (auto opened = http->Open("POST", explain_url_); !opened) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", opened.error().ToString().c_str());
        drain_jpeg_queue();
        return std::unexpected("Failed to connect to explain URL");
    }

    auto write_or_fail = [&http](const char* data, size_t size) -> bool {
        if (auto written = http->Write(data, size); !written) {
            ESP_LOGE(TAG, "Failed to upload photo: %s", written.error().ToString().c_str());
            return true;
        }
        return false;
    };

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        if (write_or_fail(question_field.c_str(), question_field.size())) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        if (write_or_fail(file_header.c_str(), file_header.size())) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        const bool write_failed = write_or_fail((const char*)chunk.data, chunk.len);
        heap_caps_free(chunk.data);
        if (write_failed) {
            drain_jpeg_queue();
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
        total_sent += chunk.len;
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        return std::unexpected("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        if (write_or_fail(multipart_footer.c_str(), multipart_footer.size())) {
            http->Close();
            return std::unexpected("Failed to upload photo");
        }
    }
    if (write_or_fail("", 0)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
             "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             frame_width_, frame_height_, (int)total_sent, (int)remain_stack_size,
             question.c_str(), result.c_str());
    return result;
}

std::expected<Esp32Camera::JpegSnapshot, Esp32Camera::SnapshotError> Esp32Camera::CaptureSnapshotJpeg(
    uint8_t quality) {
    if (!streaming_on_) {
        return std::unexpected(SnapshotError::kNotReady);
    }
    // Never make an AI capture wait behind a snapshot that has not started yet
    if (ai_capture_waiters_.load() > 0) {
        return std::unexpected(SnapshotError::kBusy);
    }
    std::unique_lock<std::mutex> lock(fb_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(SnapshotError::kBusy);
    }

    int64_t start_time = esp_timer_get_time();
    camera_fb_t* fb = GrabLatestFrame();
    if (!fb) {
        ESP_LOGE(TAG, "Snapshot capture failed");
        return std::unexpected(SnapshotError::kCaptureFailed);
    }

    // Copy into a private buffer so the AI capture buffer (encode_buf_) is never touched
    JpegSnapshot snapshot;
    snapshot.width = fb->width;
    snapshot.height = fb->height;
    pixformat_t format = fb->format;
    size_t data_size = FrameDataSize(fb);
    std::unique_ptr<uint8_t, FreeDeleter> frame(
        (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame) {
        esp_camera_fb_return(fb);
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for snapshot frame", data_size);
        return std::unexpected(SnapshotError::kNoMemory);
    }
    CopyFrame(fb, frame.get(), data_size, swap_bytes_enabled_);
    esp_camera_fb_return(fb);
    lock.unlock();

    int64_t copied_time = esp_timer_get_time();
    snapshot.capture_us = copied_time - start_time;

    if (format == PIXFORMAT_JPEG) {
        // Sensor already produced JPEG, no re-encoding needed
        snapshot.data = std::move(frame);
        snapshot.len = data_size;
        return snapshot;
    }

    v4l2_pix_fmt_t enc_fmt;
    if (!ToV4l2Format(format, &enc_fmt)) {
        ESP_LOGE(TAG, "Unsupported pixel format: %d", format);
        return std::unexpected(SnapshotError::kEncodeFailed);
    }
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    bool ok = image_to_jpeg(frame.get(), data_size, snapshot.width, snapshot.height, enc_fmt,
                            quality, &jpeg, &jpeg_len);
    frame.reset();
    if (!ok || jpeg == nullptr || jpeg_len == 0) {
        heap_caps_free(jpeg);
        ESP_LOGE(TAG, "Snapshot JPEG encoding failed");
        return std::unexpected(SnapshotError::kEncodeFailed);
    }
    snapshot.data.reset(jpeg);
    snapshot.len = jpeg_len;
    snapshot.encode_us = esp_timer_get_time() - copied_time;
    return snapshot;
}
