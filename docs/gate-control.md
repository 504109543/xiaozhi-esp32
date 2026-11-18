# 卷闸门常驻本地问候控制

目标是无需唤醒语音会话即可播报固定女声“你好”，同时保留正常唤醒、对话和打断。复用现有的摄像头 HTTP 服务和 NotifyPlayer，不保持录音 WebSocket，不新增后台麦克风上传，也不改板级引脚。

## 固件变体

- 实际硬件：bread-compact-wifi / ESP32-S3 / OLED 128×32。
- 新变体名：`bread-compact-wifi-gate`，配置 `main/boards/bread-compact-wifi/config.gate.json`。BOARD_TYPE 保持 bread-compact-wifi；BOARD_NAME 为唯一的 gate 变体名，以免与旧配置重名。
- 启用摄像头 HTTP、MJPEG、按需相机、Wi-Fi 性能模式和 `CONFIG_XIAOZHI_GATE_GREETING=y`。
- `main/assets/gate_hello.ogg` 来自 Ryan 已选择的腾讯智瑜女声（101001），内容固定“你好”。音频在 flash 中，播放无需服务器语音连接或在线合成。

规范构建命令（ESP-IDF 6.1 环境）：

```bash
python scripts/build.py bread-compact-wifi --config config.gate.json --name bread-compact-wifi-gate
```

使用保留 NVS 的 app-only OTA 更新，不把 merged-binary.bin 写到 0 地址。旧稳定 app 回退文件在 VM 的 `/home/ryan/work/esp32/artifacts/ryan-cam-stream-http-bread-compact-wifi-2433dc0/app-ota_0-0x20000.bin`。更新需要确认设备硬件、当前分区和 OTA app 大小后执行；本文件不是更新成功的证据。

## 本地接口

复用摄像头 8080 端口及 NVS `camera/http_token`。两条接口都要求 `Authorization: Bearer <token>`，token 不编译到固件、不记录到新的请求日志。只在受信局域网使用，与既有摄像头 HTTP 一致。

- `GET /status`：返回 `ok`、`local_greeting`、`device_id`、固件 version、state、idle 和 uptime_ms。视频在线不再被当作语音会话在线；local_greeting=true 表示存在独立本地问候能力。
- `POST /greet`：JSON 仅需 `event_id`（1–128 个字母/数字/点/下划线/冒号/连字符）和 `ttl_ms`（1–15000）。不接受任意文本、音频 URL 或文件路径。
- 返回 ok=true/status=accepted 只证明本机开始播放任务，不是硬件发声确认。重复 event_id 返回 duplicate。忙碌/冷却/过期返回 409。
- HTTP 任务把请求投递给 Application 主任务进行最终状态检查；主任务没有在约 1 秒内接单则作废，不会在长时间堵塞恢复后突然播报。
- 设备 idle 时才开始；使用原通知状态机，禁用远端语音处理，播放结束回到 idle。唤醒打断沿用原通知流程。
- 同一启动周期中事件 ID 去重 120 秒，最多 32 条；成功启动后的设备冷却为 30 秒。设备 RAM 去重不会跨断电保存，主机 SQLite 仍提供跨重启事件去重。断电/网络故障期间不承诺播放。

## 主机接入与启用

`/home/ryan/services/frigate/notifier/notifier.py` 支持 `GREETING_TRANSPORT=http`，向设备 `/greet` 发送请求；Bearer 来自既有 `secrets/frigate.env` 的 FRIGATE_ESP32_TOKEN，不复制到新凭据文件。

2026-11-09 23:38–23:39 CST 已完成 app-only OTA。设备重新联网后 `/status` 返回设备 ID 匹配、local_greeting=true、idle=true，主机已把 GREETING_TRANSPORT 改为 http 并重建 notifier。语音会话为空时，本地问候测试请求被设备接受，随后恢复 idle；实际女声发声仍待用户确认。请求 TTL 按原始检测帧时间计算，超过 15 秒不补播；钉钉独立投递。临时 OTA 下载服务已停止。

运维检查：

```bash
cd /home/ryan/services/frigate
docker compose exec -T notifier python /app/notifier.py --device-status
```

## 必需硬件验收

1. 不唤醒设备，确认 /status 可用；不允许因此开启持续录音。
2. 真实过门，验证收到钉钉且播放女声“你好”；出门方向按对应区域规则处理。
3. 语音会话空闲超过旧的两分钟超时后，仍能触发本地问候。
4. 播报中唤醒/说话，确认通知被打断，随后正常对话；播放完成后不自动进入持续收音。
5. 在可控范围内测试设备断网再联网、断电再启动和服务端重启，确认恢复后可以播报，过期问候不补播。不要为测试重启整个家庭路由器或影响其他设备。
6. 确认摄像头 MJPEG 持续拉流时音频、控制响应及温度均可接受。

## 测试

新增主机 C++ guard 测试覆盖有效期、参数验证、忙碌、冷却和去重；通知端增加 HTTP 固定操作、Bearer、目标限制和过期不发请求测试。编译成功不等于上述硬件场景已验证。

## 固定问候响度（2026-11-10）

用户确认原本地播放能听到一次，但声音很小。固定女声素材增加 12 dB，测得峰值约 -5.0 dBFS、平均 -19.9 dBFS，仅改变本地问候，不改设备全局音量或日常对话。通过如下命令从原始 female WAV 重建内嵌 Ogg，然后重新构建/OTA；不要对已放大的 Ogg 再叠加增益：

```bash
ffmpeg -i gate-hello.wav -af volume=12dB -c:a libopus -ar 16000 -ac 1 \
  -b:a 24000 -frame_duration 60 -application voip gate_hello.ogg
```

设备 OggDemuxer 已验证实际增强 Ogg 结构及持续时间。增强素材在后续 OTA 完成后才生效，当前部署状态以 app manifest 及现场试听为准。

## 早上天气播报与 100% 音量（2026-11-10）

Ryan 要求 07:00–09:00（北京时间，不含 09:00）问候后加播天气，地点佛山市禅城区南庄镇（播报“南庄”）；并要求设备音量直接 100%（场地大）。

- 主机：`weather-refresh.timer`（systemd user，每天 06:45、07:45，Persistent）运行 `/home/ryan/services/frigate/notifier/weather_refresh.py`：Open-Meteo 当日预报 → 固定模板句（无 LLM，降雨码或降水概率≥50% 加“出门记得带伞”）→ xiaozhi 容器内腾讯智瑜 101001 合成（`deployment/tools/prepare-gate-weather.py`）→ 容器 ffmpeg 管道转 Opus Ogg（+10 dB、限幅，实测平均约 -20.6 dBFS、峰值 -4.8 dBFS，与 +12 dB 的“你好”一致）→ `weather/cache/<sha256>.ogg` + `current.json`，失败保留旧缓存。
- notifier：同进程在 `10.1.32.156:8981` 提供只读 `/weather/<sha>.ogg`，HMAC 签名 60 秒有效，密钥只在内存。只有 `weather/config.json` enabled=true、处于 7–9 点、缓存为当天且哈希一致时，`/greet` 才带 `weather_url`；否则只说“你好”。
- 固件：`/greet` 可选 `weather_url`，只接受上述固定前缀与格式；先播内嵌“你好”，再拉取天气，天气失败不影响“你好”。`/status` 增加 `morning_weather`、`output_volume`。首次启动一次性把输出音量设为 100% 并写 NVS 标记 `gate/volume_full_v1`，之后用户再调低不会被重启覆盖（该板 NoAudioCodec 音量按平方曲线，默认 70% 约为 100% 的 -6 dB）。
- 候选 app：`/home/ryan/work/esp32/artifacts/gate-control-20261109/manifest-weather.json`（gate-966a3bca5c80.bin，含 +12 dB 问候，取代 manifest-louder 候选），installed=false。
- 启用步骤：OTA 安装该候选并确认 `--device-status` 显示 morning_weather=true、output_volume=100 后，再把 `weather/config.json` 的 enabled 改为 true（notifier 每次读取，无需重启）。旧固件收到 weather_url 的行为未验证，所以启用前不能打开。
- 测试：`tests/test_weather.py`（7 项，离线）与 22 项 notifier 测试在 notifier 镜像内通过；固件 guard 测试 3 项（含 URL 格式）通过，真实天气 Ogg 经设备 demuxer 解析约 3.5 秒。

## 2026-11-10 18:22：天气 + 100% 音量固件已安装

Ryan 唤醒后经 gate_upgrade 一次性授权安装 gate-966a3bca5c80（manifest-weather.json installed=true）；授权已被服务端解除，临时 8980 下载服务已停止。重启后 /status：device_id 匹配、local_greeting=true、morning_weather=true、output_volume=100、idle=true。notifier 已重建（device-status 显示 output_volume）。`weather/config.json` 已设 enabled=true。18:27 发出一次测试问候被设备接受（窗口外，只说“你好”），响度待用户确认；7–9 点天气实际播放待次日真机验证。

## 晚间明日天气（2026-11-10 20:40）

Ryan 要求晚上 8 点后播明天天气。仅改主机端，固件不变（固件只校验 URL 格式）。
- `weather/config.json` 新增 evening_start_hour=20、evening_end_hour=24：20:00–23:59 问候后播“南庄明天……”，缓存 `weather/cache/tomorrow.json`；07–09 仍播今天（current.json）。00:00–07:00、09:00–20:00 只说“你好”。
- `weather-refresh.timer` 增加 19:45、21:45；上午运行渲染 today，下午/晚上渲染 tomorrow（也可 `weather_refresh.py today|tomorrow` 手动指定）。旧音频清理会保留两个槽当前引用的文件。
- 签名密钥改为 notifier 状态卷 `/data/weather.key`（0600，首次自动生成），使 `docker exec` 手动测试与主进程签名一致；此前进程内随机密钥导致手动测试 403、只说“你好”。
- 测试：notifier 22 项、天气 8 项离线通过。20:4x 真机测试：设备忙约 4.7 秒（你好≈1 s + 明日天气≈3.5 s），推断天气已播放，待用户确认实际听到。
- 2026-11-10 晚：Ryan 确认手动测试听到“你好”+明日天气，并且出去再进门的真实触发也听到了（100% 音量）。

## 远程升级（无需唤醒）固件候选（2026-11-10 21:10）

Ryan 要求以后升级不再唤醒。上一版 gate-966a3bca5c80 并不含此功能（之前只是提议）。
- 固件：8080 新增 Bearer 保护的 `POST /ota`，body 仅 `{"sha256","size"}`，不接受 URL；设备固定拉取 `http://10.1.32.156:8980/gate-<sha前12>.bin`，仅 idle 时接受。`Ota::Upgrade` 新增可选 SHA-256(PSA)/大小校验，任一不符即 esp_ota_abort、不切换启动分区、继续运行旧 app。bootloader 回滚已启用（CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE），新 app 在开机连上 8202 OTA 检查后才 MarkCurrentVersionValid；未确认就重启会回滚。`/status` 增加 remote_ota、elf_sha256、app_partition、pending_verify。httpd max_uri_handlers 4→5。升级时设备仍会播放原有“升级中”提示音并重启。
- 主机：`/home/ryan/services/frigate/notifier/gate_ota_push.py <app.bin>`：校验镜像头与大小、哈希命名发布到 artifacts、临时 systemd-run gate-firmware-http(8980, ≤15 分钟)、经 notifier 容器（持有 token）`--request-ota`、轮询 /status 直到 elf_sha256 变为新版并 pending_verify=false，最后总是停止下载服务并写 manifest-<sha12>.json。
- 候选 `manifest-remote-ota.json`（gate-34442d80cdfa.bin），installed=false。因现装固件没有 /ota，需最后一次唤醒走旧 gate_upgrade 安装；之后用 gate_ota_push.py。
- 测试：固件 guard 4 项（含 OTA 摘要/URL）、notifier 22 项、天气 8 项通过；ESP-IDF 6.1 构建通过，35% 余量。真机远程 OTA 尚未执行。Docker Hub 当时 DNS/401 失败，notifier 镜像以现有本地镜像为基础叠加 COPY 重建。

## 招呼语、追问与免唤醒续聊（2026-11-12，已安装）

Ryan 要求把固定“你好”换成更拟人的问候，在固定时段天气后说一句“今天有什么好玩的分享一下吗？”，随后直接进入对话，不再说唤醒词。

- 固定女声（智瑜 101001）改为“嗨，你好呀～”，新增“今天有什么好玩的分享一下吗？”。两条素材均为 +12 dB、16 kHz 单声道 Opus Ogg，实测平均/峰值约 -21.9/-5.7 dBFS 与 -19.3/-5.3 dBFS，与旧“你好”响度一致。嵌入 `main/assets/gate_hello.ogg` 与新增 `main/assets/gate_prompt.ogg`；旧素材备份在 `/home/ryan/work/esp32/artifacts/gate-greeting-20261112/gate_hello_before_20261112.ogg`。
- 播放顺序：本地问候 → 可选天气 URL（07–09 播今天、20–24 播明天，仍由主机 `weather_url()` 决定）→ 本地追问。天气拉取失败仍播追问；整段播放完成后才继续。
- 追问播完后由 `HandleNotificationFinished` 调用 `BeginConversationWithoutWakeWord()`：设备 idle → connecting，打开正常 WebSocket 语音会话并发送 `listen start`；不发送 `wake_word_detected`，服务端按普通一轮拾音处理，用户直接回答即可。没有额外提示音。
- 唤醒、按键、`abort`、升级、重启等任何 `StopNotification()` 都会清除待续聊标记，不会在无关通知后误开麦。30 秒问候冷却与 event_id 去重不变。
- 构建：`python scripts/build.py bread-compact-wifi --config config.gate.json --name bread-compact-wifi-gate`，ESP-IDF 6.1，app 分区余量 35%。固件宿主测试 86/87 通过；唯一失败是既有 `config.snapshot-test.json` 与 `config.json` 的 BOARD_NAME 重复，与本次改动无关。
- 候选：`/home/ryan/work/esp32/artifacts/gate-control-20261109/manifest-b2df7fad1138.json`（gate-b2df7fad1138.bin）。2026-11-12 22:13 经远程 `/ota` 安装成功，这是新 HTTP OTA 路径的首次真实升级；重启后 `/status` 的 `elf_sha256` 为 `366ebd15…`，`app_partition=ota_0`、`pending_verify=false`、idle=true、音量 100%，临时 8980 下载服务已停止。实际发声、追问和免唤醒对话仍待现场确认。
- 未改服务端 socket `say_hello` 路径（当前 `GREETING_TRANSPORT=http`，该路径未启用）；若将来切回 socket，它仍是旧“你好”。

## POST /say（2026-11-17，镜像 gate-dee153938243）

- 认证同 `/greet`；body `{"text":"..."}`，≤180 字节，拒绝引号、反斜杠、控制字符（`SendWakeWordDetected` 拼接 JSON）。
- 仅空闲时接受（忙 → 409 busy，不打断），打开语音会话后以 `listen/detect` 发送文字，服务端按用户输入交给大模型；不发送唤醒词音频。
- 主机工具：`xiaozhi-esp32-server/deployment/tools/device-say.sh "放一个小时音乐"`，可配合 `systemd-run --user --on-active=...` 定时。
