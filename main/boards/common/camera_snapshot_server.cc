#include "camera_snapshot_server.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

#include "esp32_camera.h"

#define TAG "CameraSnapshot"

bool CameraSnapshotServer::Start(Esp32Camera* camera) {
    if (server_ != nullptr) {
        return true;
    }
    if (camera == nullptr) {
        ESP_LOGW(TAG, "No camera, snapshot server not started");
        return false;
    }
    camera_ = camera;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_XIAOZHI_CAMERA_SNAPSHOT_PORT;
    // Keep clear of the Wi-Fi provisioning web server which uses the default control port
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    // JPEG encoding runs in the server task, keep it below the audio tasks (opus_codec is 2)
    config.task_priority = 1;
    config.stack_size = 6144;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 1;
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

    ESP_LOGI(TAG, "Snapshot server listening on port %d, GET /snapshot, quality=%d, token %s",
             CONFIG_XIAOZHI_CAMERA_SNAPSHOT_PORT, CONFIG_XIAOZHI_CAMERA_SNAPSHOT_JPEG_QUALITY,
             strlen(CONFIG_XIAOZHI_CAMERA_SNAPSHOT_TOKEN) > 0 ? "required" : "not set");
    return true;
}

bool CameraSnapshotServer::CheckToken(httpd_req_t* req) {
    const char* expected = CONFIG_XIAOZHI_CAMERA_SNAPSHOT_TOKEN;
    if (strlen(expected) == 0) {
        return true;
    }
    char query[128];
    char token[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return strcmp(token, expected) == 0;
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
                message = "camera busy\n";
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
