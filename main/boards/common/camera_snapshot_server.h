#pragma once

#include <esp_http_server.h>

#include <string>

class Esp32Camera;

// Serves GET /snapshot (one JPEG) and, with CONFIG_XIAOZHI_CAMERA_STREAM, GET /stream (MJPEG)
// from the already initialized camera. Enabled by CONFIG_XIAOZHI_CAMERA_SNAPSHOT_SERVER.
// Every request must carry the access token (?token= or Authorization: Bearer).
class CameraSnapshotServer {
public:
    static CameraSnapshotServer& GetInstance() {
        static CameraSnapshotServer instance;
        return instance;
    }

    // Start listening on CONFIG_XIAOZHI_CAMERA_SNAPSHOT_PORT. Safe to call more than once.
    bool Start(Esp32Camera* camera);

    CameraSnapshotServer(const CameraSnapshotServer&) = delete;
    CameraSnapshotServer& operator=(const CameraSnapshotServer&) = delete;

private:
    CameraSnapshotServer() = default;

    static esp_err_t HandleSnapshot(httpd_req_t* req);
#if CONFIG_XIAOZHI_CAMERA_STREAM
    static esp_err_t HandleStream(httpd_req_t* req);
    static void StreamTask(void* arg);
#endif
    void LoadToken();
    bool CheckToken(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    std::string token_;
};
