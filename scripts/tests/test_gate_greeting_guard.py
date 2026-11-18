import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GateGreetingGuardTest(unittest.TestCase):
    def test_embedded_female_audio_decodes_with_device_demuxer(self):
        source = r'''
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include "ogg_demuxer.h"
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    assert(!bytes.empty());
    OggDemuxer demuxer;
    size_t packets = 0;
    unsigned duration_ms = 0;
    demuxer.OnPacket([&](const uint8_t*, int rate, int duration, size_t size) {
        assert(rate > 0 && duration > 0 && size > 0);
        ++packets;
        duration_ms += duration;
    });
    for (size_t offset = 0; offset < bytes.size(); offset += 1024) {
        demuxer.Process(bytes.data() + offset, std::min(size_t(1024), bytes.size()-offset));
    }
    assert(!demuxer.HasError() && demuxer.Finish());
    assert(packets > 0 && duration_ms >= 500 && duration_ms <= 3000);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / 'esp_log.h').write_text('#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n')
            src, exe = base/'audio_test.cc', base/'audio_test'
            src.write_text(source)
            demux = ROOT/'main/audio/demuxer'
            subprocess.run(shlex.split(os.environ.get('CXX','g++')) + [
                '-std=c++20','-I',str(base),'-I',str(demux),str(src),str(demux/'ogg_demuxer.cc'),'-o',str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe),str(ROOT/'main/assets/gate_hello.ogg')],check=True,timeout=5)

    def test_expiry_busy_cooldown_dedup_and_validation(self):
        source = r'''
#include <cassert>
#include "gate_greeting_guard.h"
int main() {
    using Result = GateGreetingGuard::Result;
    GateGreetingGuard guard;
    int starts = 0;
    auto play = [&]() { ++starts; return true; };
    assert(!GateGreetingGuard::ValidId(""));
    assert(!GateGreetingGuard::ValidId("bad/id"));
    assert(!GateGreetingGuard::ValidId(std::string(129, 'x')));
    assert(GateGreetingGuard::ValidId("1794221111.123-ab_cd:test"));
    assert(guard.Try("event-1", 100, 100, play) == Result::Expired);
    assert(guard.Try("bad/id", 100, 200, play) == Result::Invalid);
    assert(guard.Try("event-1", 100, 200, [] { return false; }) == Result::Busy);
    assert(starts == 0);
    assert(guard.Try("event-1", 101, 200, play) == Result::Accepted);
    assert(guard.Try("event-1", 102, 200, play) == Result::Duplicate);
    assert(guard.Try("event-2", 102, 200, play) == Result::Cooldown);
    assert(starts == 1);
    assert(guard.Try("event-2", 30102, 30200, play) == Result::Accepted);
    assert(starts == 2);
    // Expiry is always checked, even for a previously accepted ID.
    assert(guard.Try("event-2", 30103, 30103, play) == Result::Expired);
    // RAM dedup is intentionally bounded and expires after two minutes.
    assert(guard.Try("event-1", 151000, 152000, play) == Result::Accepted);
    assert(starts == 3);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            src = base / 'guard_test.cc'
            exe = base / 'guard_test'
            src.write_text(source)
            subprocess.run(shlex.split(os.environ.get('CXX', 'g++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'main/notify'), str(src), '-o', str(exe),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_weather_url_matches_host_signer_only(self):
        # Shape produced by /home/ryan/services/frigate/notifier/morning_weather.py.
        good = ('http://10.1.32.156:8981/weather/' + 'a' * 64 + '.ogg?expires=1794300000&sig=' + 'b' * 64)
        bad = [good.replace('10.1.32.156', '10.1.32.157'), good.replace(':8981', ':8982'),
               good.replace('http://', 'https://'), good + '&x=1', good[:-1], good.replace('a' * 64, 'A' * 64),
               good.replace('/weather/', '/weather/../'), good.replace('expires=1', 'expires=x'), '']
        source = r'''
#include <cstdio>
#include "gate_greeting_guard.h"
int main(int argc, char** argv) {
    if (!GateGreetingGuard::ValidWeatherUrl(argv[1])) return 1;
    for (int i = 2; i < argc; ++i) if (GateGreetingGuard::ValidWeatherUrl(argv[i])) return i;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            src, exe = base / 'url_test.cc', base / 'url_test'
            src.write_text(source)
            subprocess.run(shlex.split(os.environ.get('CXX', 'g++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'main/notify'), str(src), '-o', str(exe),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(exe), good, *bad], check=True, timeout=5)

    def test_ota_digest_and_fixed_url(self):
        source = r"""
#include <cassert>
#include "gate_greeting_guard.h"
int main() {
    const std::string good(64, 'a');
    assert(GateGreetingGuard::ValidSha256(good));
    assert(GateGreetingGuard::ValidSha256("966a3bca5c80cf4eee254a341a395bcf6e792e09bcb45753280938b81a5b6cac"));
    assert(!GateGreetingGuard::ValidSha256(std::string(63, 'a')));
    assert(!GateGreetingGuard::ValidSha256(std::string(65, 'a')));
    assert(!GateGreetingGuard::ValidSha256(std::string(64, 'A')));
    assert(!GateGreetingGuard::ValidSha256(std::string(63, 'a') + "/"));
    assert(GateGreetingGuard::OtaUrl("966a3bca5c80cf4eee254a341a395bcf6e792e09bcb45753280938b81a5b6cac") ==
           "http://10.1.32.156:8980/gate-966a3bca5c80.bin");
}
"""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            src, exe = base / 'ota_test.cc', base / 'ota_test'
            src.write_text(source)
            subprocess.run(shlex.split(os.environ.get('CXX', 'g++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'main/notify'), str(src), '-o', str(exe),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
