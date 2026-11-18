#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

// Accessed only on the Application task. No audio queue or NVS writes here.
class GateGreetingGuard {
public:
    enum class Result { Accepted, Duplicate, Busy, Cooldown, Expired, Invalid };

    static bool ValidId(const std::string& id) {
        if (id.empty() || id.size() > 128) {
            return false;
        }
        for (unsigned char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == ':' || c == '-')) {
                return false;
            }
        }
        return true;
    }

    static bool ValidWeatherUrl(std::string_view url) {
        constexpr std::string_view prefix = "http://10.1.32.156:8981/weather/";
        constexpr std::string_view expires = ".ogg?expires=";
        constexpr std::string_view signature = "&sig=";
        constexpr size_t expected_size = prefix.size() + 64 + expires.size() + 10 +
                                         signature.size() + 64;
        if (url.size() > 256 || url.size() != expected_size ||
            url.substr(0, prefix.size()) != prefix) {
            return false;
        }
        size_t offset = prefix.size();
        auto consume = [&](size_t count, bool hex) {
            for (size_t end = offset + count; offset < end; ++offset) {
                const char c = url[offset];
                if (!((c >= '0' && c <= '9') || (hex && c >= 'a' && c <= 'f'))) {
                    return false;
                }
            }
            return true;
        };
        if (!consume(64, true) || url.substr(offset, expires.size()) != expires) {
            return false;
        }
        offset += expires.size();
        if (!consume(10, false) || url.substr(offset, signature.size()) != signature) {
            return false;
        }
        offset += signature.size();
        return consume(64, true);
    }

    // Remote app OTA: the host only names a digest; the URL is fixed to the host's
    // temporary LAN firmware service and the hash-named file it publishes.
    static bool ValidSha256(std::string_view digest) {
        if (digest.size() != 64) {
            return false;
        }
        for (char c : digest) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    static std::string OtaUrl(std::string_view digest) {
        return "http://10.1.32.156:8980/gate-" + std::string(digest.substr(0, 12)) + ".bin";
    }

    Result Try(const std::string& id, int64_t now_ms, int64_t deadline_ms,
               const std::function<bool()>& start) {
        if (!ValidId(id)) {
            return Result::Invalid;
        }
        if (now_ms >= deadline_ms) {
            return Result::Expired;
        }
        while (!seen_.empty() && now_ms - seen_.front().at_ms > 120000) {
            seen_.pop_front();
        }
        for (const auto& entry : seen_) {
            if (entry.id == id) {
                return Result::Duplicate;
            }
        }
        if (last_started_ms_ >= 0 && now_ms - last_started_ms_ < 30000) {
            return Result::Cooldown;
        }
        if (!start()) {
            return Result::Busy;
        }
        last_started_ms_ = now_ms;
        if (seen_.size() == 32) {
            seen_.pop_front();
        }
        seen_.push_back({id, now_ms});
        return Result::Accepted;
    }

private:
    struct Entry {
        std::string id;
        int64_t at_ms;
    };
    std::deque<Entry> seen_;
    int64_t last_started_ms_ = -1;
};
