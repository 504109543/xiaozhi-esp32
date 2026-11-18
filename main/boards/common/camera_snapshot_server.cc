#include "camera_snapshot_server.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_random.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp32_camera.h"
#include "settings.h"
#if CONFIG_XIAOZHI_GATE_GREETING
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <cJSON.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "system_info.h"
#endif

#define TAG "CameraSnapshot"

#if CONFIG_XIAOZHI_CAMERA_STREAM
#define ENDPOINTS "/snapshot /stream"
#else
#define ENDPOINTS "/snapshot"
#endif

bool CameraSnapshotServer::Start(Esp32Camera* camera) {
    if (server_ != nullptr) {
        return true;
    }
    if (camera == nullptr) {
        ESP_LOGW(TAG, "No camera, snapshot server not started");
        return false;
    }
    camera_ = camera;
    LoadToken();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_XIAOZHI_CAMERA_SNAPSHOT_PORT;
    // Keep clear of the Wi-Fi provisioning web server which uses the default control port
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    // JPEG encoding runs in the server task, keep it below the audio tasks (opus_codec is 2)
    config.task_priority = 1;
    config.stack_size = 6144;
    // A running stream keeps one socket; leave room to answer /snapshot and a second /stream
    config.max_open_sockets = 3;
    config.max_uri_handlers = 6;  // snapshot, stream, greet, say, status, ota
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    httpd_uri_t snapshot_uri = {
        .uri = "/snapshot",
        .method = HTTP_GET,
        .handler = HandleSnapshot,
        .user_ctx = this,
    };
    err = httpd_register_uri_handler(server_, &snapshot_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /snapshot: %s", esp_err_to_name(err));
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }

#if CONFIG_XIAOZHI_CAMERA_STREAM
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = HandleStream,
        .user_ctx = this,
    };
    err = httpd_register_uri_handler(server_, &stream_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /stream: %s", esp_err_to_name(err));
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }
#endif

#if CONFIG_XIAOZHI_GATE_GREETING
    httpd_uri_t greet_uri = {
        .uri = "/greet",
        .method = HTTP_POST,
        .handler = HandleGreeting,
        .user_ctx = this,
    };
    httpd_uri_t say_uri = {
        .uri = "/say",
        .method = HTTP_POST,
        .handler = HandleSay,
        .user_ctx = this,
    };
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = HandleStatus,
        .user_ctx = this,
    };
    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = HandleOta,
        .user_ctx = this,
    };
    if (httpd_register_uri_handler(server_, &greet_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &say_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &status_uri) != ESP_OK ||
        httpd_register_uri_handler(server_, &ota_uri) != ESP_OK) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGE(TAG, "Failed to register gate control endpoints");
        return false;
    }
#endif

    ESP_LOGI(TAG, "Camera HTTP server listening on port %d: %s, snapshot quality=%d",
             CONFIG_XIAOZHI_CAMERA_SNAPSHOT_PORT, ENDPOINTS,
             CONFIG_XIAOZHI_CAMERA_SNAPSHOT_JPEG_QUALITY);
    return true;
}

// The token comes from Kconfig when set, otherwise from NVS. A random token is generated and
// stored on first use, so no secret has to be committed or compiled into CI artifacts.
void CameraSnapshotServer::LoadToken() {
    token_ = CONFIG_XIAOZHI_CAMERA_SNAPSHOT_TOKEN;
    if (!token_.empty()) {
        ESP_LOGI(TAG, "Access token: configured in Kconfig");
        return;
    }
    Settings settings("camera", true);
    token_ = settings.GetString("http_token");
    if (token_.empty()) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        char hex[sizeof(random) * 2 + 1];
        for (size_t i = 0; i < sizeof(random); i++) {
            snprintf(hex + i * 2, 3, "%02x", random[i]);
        }
        token_ = hex;
        settings.SetString("http_token", token_);
        ESP_LOGI(TAG, "Generated a new access token");
    }
    // Printed on every boot so it can be read back over the serial console
    ESP_LOGI(TAG, "Access token: %s (NVS camera/http_token)", token_.c_str());
}

bool CameraSnapshotServer::CheckToken(httpd_req_t* req) {
    char token[72] = {};
    char header[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Bearer ", 7) == 0) {
        strncpy(token, header + 7, sizeof(token) - 1);
    } else {
        char query[160];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
            return false;
        }
    }
    // Compare without an early exit so the response time does not depend on the token prefix
    size_t len = strlen(token);
    if (len != token_.size()) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(token[i] ^ token_[i]);
    }
    return diff == 0;
}

esp_err_t CameraSnapshotServer::HandleSnapshot(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);

    if (!self->CheckToken(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "invalid token\n");
    }

    auto result = self->camera_->CaptureSnapshotJpeg(CONFIG_XIAOZHI_CAMERA_SNAPSHOT_JPEG_QUALITY);
    if (!result) {
        const char* status = "500 Internal Server Error";
        const char* message = "capture failed\n";
        switch (result.error()) {
            case Esp32Camera::SnapshotError::kBusy:
                status = "503 Service Unavailable";
                message = "camera busy (AI capture or stream running)\n";
                break;
            case Esp32Camera::SnapshotError::kNotReady:
                status = "503 Service Unavailable";
                message = "camera not ready\n";
                break;
            case Esp32Camera::SnapshotError::kNoMemory:
                message = "out of memory\n";
                break;
            case Esp32Camera::SnapshotError::kEncodeFailed:
                message = "jpeg encode failed\n";
                break;
            case Esp32Camera::SnapshotError::kCaptureFailed:
                break;
        }
        ESP_LOGW(TAG, "Snapshot rejected: %s", message);
        httpd_resp_set_status(req, status);
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, message);
    }

    // Memory is sampled after encoding, while the JPEG is still allocated
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    int capture_ms = int(result->capture_us / 1000);
    int encode_ms = int(result->encode_us / 1000);

    // Header values must stay valid until the response is sent
    char capture_hdr[16], encode_hdr[16], internal_hdr[16], psram_hdr[16];
    snprintf(capture_hdr, sizeof(capture_hdr), "%d", capture_ms);
    snprintf(encode_hdr, sizeof(encode_hdr), "%d", encode_ms);
    snprintf(internal_hdr, sizeof(internal_hdr), "%u", (unsigned)free_internal);
    snprintf(psram_hdr, sizeof(psram_hdr), "%u", (unsigned)free_psram);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Capture-Ms", capture_hdr);
    httpd_resp_set_hdr(req, "X-Encode-Ms", encode_hdr);
    httpd_resp_set_hdr(req, "X-Free-Internal", internal_hdr);
    httpd_resp_set_hdr(req, "X-Free-Psram", psram_hdr);

    // The camera lock is already released, sending does not block AI capture
    int64_t send_start = esp_timer_get_time();
    esp_err_t err = httpd_resp_send(req, (const char*)result->data.get(), result->len);
    int send_ms = int((esp_timer_get_time() - send_start) / 1000);

    ESP_LOGI(TAG,
             "Snapshot %ux%u jpeg=%u bytes q=%d capture=%d ms encode=%d ms send=%d ms%s, "
             "internal free=%u min=%u largest=%u, psram free=%u min=%u, stack hwm=%u",
             result->width, result->height, (unsigned)result->len,
             CONFIG_XIAOZHI_CAMERA_SNAPSHOT_JPEG_QUALITY, capture_ms, encode_ms, send_ms,
             err == ESP_OK ? "" : " (send failed)", (unsigned)free_internal,
             (unsigned)min_free_internal, (unsigned)largest_internal, (unsigned)free_psram,
             (unsigned)min_free_psram, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return err;
}

#if CONFIG_XIAOZHI_CAMERA_STREAM

#define STREAM_BOUNDARY "xiaozhi-frame"

struct StreamJob {
    Esp32Camera* camera;
    httpd_req_t* req;
    Esp32Camera::StreamParams params;
    int fps;
};

static bool ParseFrameSize(const char* value, framesize_t* out) {
    if (strcmp(value, "qvga") == 0) {
        *out = FRAMESIZE_QVGA;  // 320x240
    } else if (strcmp(value, "hvga") == 0) {
        *out = FRAMESIZE_HVGA;  // 480x320
    } else if (strcmp(value, "vga") == 0) {
        *out = FRAMESIZE_VGA;  // 640x480
    } else if (strcmp(value, "svga") == 0) {
        *out = FRAMESIZE_SVGA;  // 800x600
    } else {
        return false;
    }
    return true;
}

// GET /stream?token=...&size=qvga|hvga|vga|svga&fps=1..N&q=4..63
esp_err_t CameraSnapshotServer::HandleStream(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);

    if (!self->CheckToken(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "invalid token\n");
    }

    Esp32Camera::StreamParams params;
    params.jpeg_quality = CONFIG_XIAOZHI_CAMERA_STREAM_JPEG_QUALITY;
    int fps = CONFIG_XIAOZHI_CAMERA_STREAM_DEFAULT_FPS;
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "size", value, sizeof(value)) == ESP_OK &&
            !ParseFrameSize(value, &params.frame_size)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "size must be qvga, hvga, vga or svga\n");
        }
        if (httpd_query_key_value(query, "fps", value, sizeof(value)) == ESP_OK) {
            fps = atoi(value);
        }
        if (httpd_query_key_value(query, "q", value, sizeof(value)) == ESP_OK) {
            params.jpeg_quality = atoi(value);
        }
    }
    if (fps < 1 || fps > CONFIG_XIAOZHI_CAMERA_STREAM_MAX_FPS) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "fps out of range\n");
    }
    if (params.jpeg_quality < 4 || params.jpeg_quality > 63) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "q must be 4-63 (lower is better)\n");
    }

    if (!self->camera_->BeginStream()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_hdr(req, "Retry-After", "5");
        return httpd_resp_sendstr(req, "stream already running or camera not ready\n");
    }

    // Hand the request to a separate task so the server keeps answering other requests
    httpd_req_t* async_req = nullptr;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        self->camera_->EndStream();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "failed to start stream\n");
    }
    auto* job = new StreamJob{self->camera_, async_req, params, fps};
    // Below the audio tasks; frames are only copied and sent, no encoding happens here
    if (xTaskCreate(StreamTask, "camera_stream", 4096, job, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        httpd_resp_set_status(async_req, "500 Internal Server Error");
        httpd_resp_sendstr(async_req, "failed to start stream\n");
        httpd_req_async_handler_complete(async_req);
        self->camera_->EndStream();
        delete job;
        return ESP_OK;
    }
    return ESP_OK;
}

void CameraSnapshotServer::StreamTask(void* arg) {
    auto* job = static_cast<StreamJob*>(arg);
    httpd_req_t* req = job->req;
    const int64_t interval_us = 1000000LL / job->fps;
    const int resolution_index = job->params.frame_size;
    ESP_LOGI(TAG, "Stream started: %dx%d, fps=%d, q=%d", resolution[resolution_index].width,
             resolution[resolution_index].height, job->fps, job->params.jpeg_quality);

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int64_t started = esp_timer_get_time();
    int64_t window_start = started;
    int frames = 0, window_frames = 0, busy = 0, failures = 0;
    size_t bytes = 0, window_bytes = 0;
    int64_t window_capture_us = 0;
    const char* end_reason = "client disconnected";

    while (true) {
        int64_t frame_start = esp_timer_get_time();
        auto result = job->camera->CaptureStreamJpeg(job->params);
        if (!result) {
            if (result.error() == Esp32Camera::SnapshotError::kBusy) {
                // AI capture in progress, the picture freezes until it is done
                busy++;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (++failures >= 5) {
                end_reason = "camera failed";
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        failures = 0;

        char part[128];
        int part_len = snprintf(part, sizeof(part),
                                "--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                                "Content-Length: %u\r\n\r\n",
                                (unsigned)result->len);
        if (httpd_resp_send_chunk(req, part, part_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char*)result->data.get(), result->len) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            break;
        }
        frames++;
        window_frames++;
        bytes += result->len;
        window_bytes += result->len;
        window_capture_us += result->capture_us;
        result->data.reset();

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 10000000LL) {
            int64_t window_ms = (now - window_start) / 1000;
            ESP_LOGI(TAG,
                     "Stream: %d.%d fps, avg frame=%u bytes, %d KB/s, avg capture=%d ms, "
                     "busy=%d, internal free=%u min=%u, psram free=%u, stack hwm=%u",
                     int(window_frames * 1000 / window_ms), int(window_frames * 10000 / window_ms % 10),
                     (unsigned)(window_bytes / window_frames),
                     int(window_bytes * 1000 / 1024 / window_ms),
                     int(window_capture_us / window_frames / 1000),
                     busy, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            window_start = now;
            window_frames = 0;
            window_bytes = 0;
            window_capture_us = 0;
            busy = 0;
        }

        int64_t elapsed = esp_timer_get_time() - frame_start;
        if (elapsed < interval_us) {
            vTaskDelay(pdMS_TO_TICKS((interval_us - elapsed) / 1000));
        }
    }

    httpd_resp_send_chunk(req, nullptr, 0);
    httpd_req_async_handler_complete(req);
    job->camera->EndStream();
    ESP_LOGI(TAG, "Stream ended (%s): %d frames, %u bytes in %d s", end_reason, frames,
             (unsigned)bytes, int((esp_timer_get_time() - started) / 1000000));
    delete job;
    vTaskDelete(nullptr);
}

#endif  // CONFIG_XIAOZHI_CAMERA_STREAM

#if CONFIG_XIAOZHI_GATE_GREETING
esp_err_t CameraSnapshotServer::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);
    if (!self->CheckToken(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    const auto* description = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "local_greeting", true);
    cJSON_AddBoolToObject(root, "morning_weather", true);
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "version", description->version);
    cJSON_AddNumberToObject(root, "state", Application::GetInstance().GetDeviceState());
    cJSON_AddBoolToObject(root, "idle",
                          Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "output_volume",
                            Board::GetInstance().GetAudioCodec()->output_volume());
    cJSON_AddBoolToObject(root, "remote_ota", true);
    char elf_sha[65];
    for (size_t i = 0; i < 32; ++i) {
        snprintf(elf_sha + i * 2, 3, "%02x", description->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(root, "elf_sha256", elf_sha);
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    cJSON_AddStringToObject(root, "app_partition", running ? running->label : "");
    cJSON_AddBoolToObject(root, "pending_verify",
                          running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
                              ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    char* output = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = output ? httpd_resp_sendstr(req, output) : ESP_FAIL;
    cJSON_free(output);
    cJSON_Delete(root);
    return result;
}

// Body: {"sha256":"<64 lowercase hex>","size":<bytes>}. No URL is accepted; the
// image must hash-match before the boot partition changes, and the bootloader's
// rollback reverts an app that never reaches MarkCurrentVersionValid().
esp_err_t CameraSnapshotServer::HandleOta(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);
    if (!self->CheckToken(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (req->content_len <= 0 || req->content_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request");
        }
        received += count;
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    const cJSON* sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!cJSON_IsObject(root) || !cJSON_IsString(sha) ||
        !GateGreetingGuard::ValidSha256(sha->valuestring) || !cJSON_IsNumber(size) ||
        !std::isfinite(size->valuedouble) || size->valuedouble < 1024 || target == nullptr ||
        size->valuedouble > target->size || size->valuedouble != std::floor(size->valuedouble)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sha256 or size");
    }
    const std::string digest(sha->valuestring);
    const size_t image_size = static_cast<size_t>(size->valuedouble);
    cJSON_Delete(root);

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    Application::GetInstance().Schedule([promise, digest, image_size]() {
        auto& app = Application::GetInstance();
        const bool idle = app.GetDeviceState() == kDeviceStateIdle;
        promise->set_value(idle);
        if (idle) {
            // Separate main-task job, so this HTTP request is answered first.
            app.Schedule([digest, image_size]() {
                if (!Application::GetInstance().UpgradeFirmware(GateGreetingGuard::OtaUrl(digest),
                                                                "(LAN gate OTA)", digest,
                                                                image_size)) {
                    ESP_LOGE(TAG, "Remote gate OTA failed; still running the current app");
                }
            });
        }
    });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (future.wait_for(std::chrono::milliseconds(1100)) != std::future_status::ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"dispatch_timeout\"}");
    }
    if (!future.get()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"accepted\"}");
}

// POST /say {"text":"..."}: open a voice session and send the text to the server
// as if the user had said it. Idle only; never interrupts playback or a conversation.
esp_err_t CameraSnapshotServer::HandleSay(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);
    if (!self->CheckToken(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request");
        }
        received += count;
    }
    if (body.find('\0') != std::string::npos || body.find("\\u0000") != std::string::npos) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON string");
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    const cJSON* text_json = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsObject(root) || !cJSON_IsString(text_json)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid text");
    }
    std::string text(text_json->valuestring);
    cJSON_Delete(root);
    // The protocol builds the listen/detect JSON by concatenation, so reject
    // anything that would need escaping.
    bool valid = !text.empty() && text.size() <= 180;
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            valid = false;
            break;
        }
    }
    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid text");
    }
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    // A stalled main task must not start a session after this request timed out.
    const int64_t deadline = esp_timer_get_time() / 1000 + 1000;
    Application::GetInstance().Schedule([promise, text, deadline]() {
        promise->set_value(Application::GetInstance().RequestRemoteText(text, deadline));
    });
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (future.wait_for(std::chrono::milliseconds(1100)) != std::future_status::ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"dispatch_timeout\"}");
    }
    if (future.get()) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"accepted\"}");
    }
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
}

esp_err_t CameraSnapshotServer::HandleGreeting(httpd_req_t* req) {
    auto* self = static_cast<CameraSnapshotServer*>(req->user_ctx);
    if (!self->CheckToken(req)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request");
        }
        received += count;
    }
    // cJSON strings are NUL-terminated; reject embedded NULs rather than validating
    // only a URL prefix before a decoded NUL.
    if (body.find('\0') != std::string::npos || body.find("\\u0000") != std::string::npos) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON string");
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "event_id");
    const cJSON* ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_ms");
    if (!cJSON_IsObject(root) || !cJSON_IsString(id) ||
        !GateGreetingGuard::ValidId(id->valuestring) || !cJSON_IsNumber(ttl) ||
        !std::isfinite(ttl->valuedouble) || ttl->valuedouble < 1 || ttl->valuedouble > 15000) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid event_id or ttl_ms");
    }
    const cJSON* weather = cJSON_GetObjectItemCaseSensitive(root, "weather_url");
    if (weather && (!cJSON_IsString(weather) ||
                    !GateGreetingGuard::ValidWeatherUrl(weather->valuestring))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid weather_url");
    }
    const std::string weather_url = weather ? weather->valuestring : "";
    const std::string event_id(id->valuestring);
    const int64_t deadline = esp_timer_get_time() / 1000 + static_cast<int64_t>(ttl->valuedouble);
    cJSON_Delete(root);
    using Result = GateGreetingGuard::Result;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    // Main-task state check and reservation: never play audio on the HTTP task.
    // A stalled main task must not produce a greeting after this request timed out.
    const int64_t dispatch_deadline = std::min(deadline, esp_timer_get_time() / 1000 + 1000);
    Application::GetInstance().Schedule([promise, event_id, dispatch_deadline, weather_url]() {
        promise->set_value(
            Application::GetInstance().RequestGateGreeting(event_id, dispatch_deadline, weather_url));
    });
    if (future.wait_for(std::chrono::milliseconds(1100)) != std::future_status::ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"dispatch_timeout\"}");
    }
    const auto result = future.get();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (result == Result::Accepted || result == Result::Duplicate) {
        return httpd_resp_sendstr(req, result == Result::Accepted
                                           ? "{\"ok\":true,\"status\":\"accepted\"}"
                                           : "{\"ok\":true,\"status\":\"duplicate\"}");
    }
    httpd_resp_set_status(req, "409 Conflict");
    if (result == Result::Cooldown) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"cooldown\"}");
    }
    return httpd_resp_sendstr(req, result == Result::Expired
                                       ? "{\"ok\":false,\"error\":\"expired\"}"
                                       : "{\"ok\":false,\"error\":\"busy\"}");
}
#endif
