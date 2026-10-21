#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_camera.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
public:
    struct FreeDeleter {
        void operator()(uint8_t* p) const { heap_caps_free(p); }
    };

    struct JpegSnapshot {
        std::unique_ptr<uint8_t, FreeDeleter> data;
        size_t len = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        int64_t capture_us = 0;  // Frame grab + copy, camera lock held
        int64_t encode_us = 0;   // JPEG encoding, camera lock released
    };

    enum class SnapshotError {
        kNotReady,       // Camera driver failed to initialize
        kBusy,           // AI capture pending or camera in use
        kCaptureFailed,  // esp_camera_fb_get() failed
        kNoMemory,       // Frame copy allocation failed
        kEncodeFailed,   // JPEG encoding failed
    };

private:
    camera_config_t config_;
    bool available_ = false;     // Sensor was detected at boot
    bool streaming_on_ = false;  // Driver is running and writing frames into PSRAM
    bool swap_bytes_enabled_ = true;  // Swap pixel byte order for RGB565, enabled by default
    int8_t hmirror_ = -1;             // Runtime mirror/flip overrides, -1 = not set
    int8_t vflip_ = -1;
    esp_timer_handle_t idle_timer_ = nullptr;
    int64_t last_use_us_ = 0;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    // The driver has a single frame buffer, so frames are copied out and returned right away.
    // fb_mutex_ serializes all esp_camera_fb_get()/fb_return() users.
    std::mutex fb_mutex_;
    std::atomic<int> ai_capture_waiters_{0};  // AI capture has priority over snapshots
    // Copy of the last frame taken by Capture(), consumed by Explain()
    uint8_t* encode_buf_ = nullptr;  // Buffer for JPEG encoding (with optional byte swap)
    size_t encode_buf_size_ = 0;     // Allocated capacity of encode_buf_
    size_t frame_len_ = 0;           // Valid bytes in encode_buf_
    uint16_t frame_width_ = 0;
    uint16_t frame_height_ = 0;
    pixformat_t frame_format_ = PIXFORMAT_RGB565;
    bool has_frame_ = false;

    camera_fb_t* GrabLatestFrame();
    // Caller must hold fb_mutex_
    bool StartLocked();
    void StopLocked();
    void ApplySensorSettings();
    void ArmIdleStop();
    void StopIfIdle();

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token) override;
    virtual bool Capture() override;
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual bool SetSwapBytes(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;

    // Grab a fresh frame and encode it to JPEG without touching the AI capture buffers.
    // Never blocks waiting for the camera: returns kBusy while an AI capture is pending.
    std::expected<JpegSnapshot, SnapshotError> CaptureSnapshotJpeg(uint8_t quality);
};
