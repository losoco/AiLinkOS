# 录音/音频与 Flash 扩容复盘（Waveshare ESP32-S3 ePaper 3.97）

2026-11-20。记录「录音速记」从采集打通到播放器复用、音量曲线、Flash 分区调整的完整经验。
面向后续做音频、存储或新板接入的同学，重点是**症状 → 根因 → 修复**，避免重复踩坑。

## 1. 结果速览

| 主题 | 结论 |
|---|---|
| 录音 | 16/32 kHz 单声道 WAV，写入 `/rec`，1 小时/不限 |
| 播放 | 录音列表 Confirm 复用音乐播放器（暂停、音量、切歌、进度） |
| 音量 | 0 静音；1–100 % 映射 −35…0 dB，再叠加 +3.6 dB 硬件补偿 |
| 备忘录响度 | 播放侧 +18 dB（仅外部列表），WAV 文件不改 |
| Flash | SPIFFS 3.375 MiB → 1 MiB；app 槽 6.25 MiB → 7.4375 MiB |
| 镜像 | 6,496,923 / 7,798,784 B（83.3 %），余量约 1.24 MiB |

## 2. 问题复盘

### 2.1 麦克风 ADC 恒为 0

- **症状**：`CMD:REC` 的 `acRms` 恒为 0，实际环境有声音。
- **根因**：ES8311 麦克风 PGA 增益寄存器 `0x16` 写成了 `0x08`，该字段是 0 基编码，`+42 dB` 应是 `0x07`；越界值静音了 PGA。另外 16 kHz 时 `0x04`（DAC OSR）必须是 `0x20`。
- **修复**：`codecStartAdc()` 逐寄存器对齐厂商 `esp_codec_dev` 的 `es8311_open() + es8311_start()` 序列；开始采集时拉高 PA（GPIO39，兼麦克风偏置），结束拉低。
- **经验**：寄存器序列不要凭数据手册“似乎合理”拼；以厂商 demo 的完整序列为准，并用 `CMD:REC` 的 RMS/峰值做闭环验证。

### 2.2 列表 Confirm 没反应

- **症状**：录音列表按 Confirm 无任何变化，日志里只有一条播放失败。
- **根因**：自研 `RecordingPlayer` 给 `HalAudioOutput::playMusicPcm()` 传了非空 seek 回调，但该回调永远返回 `false`；`AudioManager::playPcm()` 在启动前要求 `source.seek(0)` 成功，于是直接拒绝播放。
- **修复**：删除 `RecordingPlayer`，录音列表改用 `MUSIC_SESSION.playExternal(paths, index)`，把整份 `/rec` 列表交给音乐播放器；`seek` 传 `nullptr`。
- **经验**：不要为“只是播一个文件”再写一套 PCM 会话；复用现有 `MusicSession` 的解码、暂停、音量、切歌与 UI，成本更低、行为更一致。也不要传“存在但总失败”的回调。

### 2.3 录音时长比实际短约 25 %

- **症状**：墙钟 8.5 s，WAV 只有 00:06。
- **根因**：`pump()` 在 Activity 主循环里拉流，而主循环每轮末尾固定 `delay(10)`；每 32 ms 采一块、再睡 10 ms，I2S DMA 持续溢出。
- **修复**：`RecorderActivity::skipLoopDelay()` 在录制时返回 `true`，让主循环 `yield()` 而不是 `delay(10)`，同时保持 240 MHz。
- **验证**：8.5 s 墙钟 → 00:08（含 0.24 s 起始跳过）。
- **经验**：任何“在 Activity loop 里持续拉 I2S/串口”的功能，都要检查主循环的空闲延时；`skipLoopDelay()` 是现成开关。

### 2.4 开始录音卡 5.8 秒

- **症状**：进入录音页到真正开录间隔近 6 秒。
- **根因**：`RecordingSession::begin()` 里调用 `Storage.getSpace()`，冷缓存会触发整卡 `freeClusterCount()`。
- **修复**：去掉开录前的剩余空间探测；写满时由短写报 `WriteFailed` 并正常收尾。
- **经验**：启动路径上不要做全卡扫描。`getSpace()`/`freeClusterCount()` 只适合后台或用户明确等待的场景。

### 2.5 音量 50 % 以下几乎无声

- **旧实现**：`reg = percent × 0xCF / 100`，把百分比线性映射到 dB 寄存器；50 % ≈ −44 dB，100 % = +16 dB。
- **官方 demo**（`03_Music`）：`esp_codec_dev_set_out_vol(70)`，内部是 1) 百分比 → −50…0 dB 的感知曲线；2) dB → ES8311 `0x32`（0.5 dB/步，`0xBF` = 0 dB）；3) 再减 `hw_gain`，即加 +3.6 dB（`20·log10(3.3/5.0)`）。
- **最终实现**（`AudioManager::setVolume`）：
  - `0` → `0x00` 静音；
  - `1..100` → `0x32 = 0x80 + 0.7 × percent`，覆盖 −35…0 dB 再加 +3.6 dB 补偿；
  - 35 % = `0x98`（−19.5 dB），50 % = `0xA3`（−14 dB），100 % = `0xC6`（+3.5 dB，官方 100 % 的实际寄存器值）。
- **为什么不是严格照搬官方曲线**：官方 −50…0 dB 在 35 % 时约 −32 dB，这颗 1 W 喇叭上几乎听不见。保留寄存器刻度与官方上限，压缩动态范围、抬高下限。
- **设备验证**：`CMD:VOL 35/50/100/0` 回读 `0x98/0xA3/0xC6/0x00`。

### 2.6 备忘录仍然偏小

- **实测**：`CMD:REC 3` 得到 `acRms ≈ 507–583`、`peak ≈ 1941–2533`，约 **−36 dBFS RMS / −22 dBFS 峰值**；音乐文件通常 −14 dBFS 左右。
- **修复**：只在外部列表（`external_`）播放时对 PCM 乘 8（+18 dB），带饱和；WAV 不动，便于导出给 ASR。
- **经验**：先量电平再调音量。源信号比音乐低 20 dB 时，任何曲线都救不了低百分比；要么提高录音增益，要么在播放侧补偿。

### 2.7 Flash 99.1 %

- **构成**：`.flash.text` 3.16 MB + `.flash.rodata` 3.19 MB；字体约 1.3 MB（CJK 8/10/12 = 937 KB）、i18n 全语言约 544 KB、断词 trie 351 KB（德语 206 KB）。
- **结构性浪费**：3.375 MiB SPIFFS 分区完全没用到（Arduino SPIFFS 已关闭，源码无挂载）。
- **修复**：`partitions.csv` 改为 app0/app1 各 `0x770000`（7.4375 MiB）、spiffs `0xEF0000` + `0x100000`（1 MiB）；同步 `scripts/package_nightly_target.py`、分区测试和布局文档。
- **验证**：构建 83.3 %；从设备 `0x8000` 回读分区表逐项一致；`verify_partition_csv/binary` 通过。
- **OTA 注意**：普通 OTA 不更新分区表。旧 6.25 MB 表的设备仍受旧上限约束，镜像一旦超过 6.55 MB 就必须整表刷写。

### 2.8 构建报 `No space left on device`

- **根因**：不是固件 Flash，而是**宿主机磁盘**满（当时只剩 513 MB）。
- **处理**：清理可再生的包缓存（`~/.cache/uv`、`~/Library/Caches/pip`、`node-gyp`）后链接成功。
- **经验**：看到该错误先 `df -h`，区分“主机磁盘”与“ESP Flash 分区”。

### 2.9 i18n 构建失败：`STR_REC_EMPTY` 缺失

- **症状**：`gen_i18n.py` 报 “1 string(s) used in source but missing from english.yaml”。
- **原因**：多会话/多次改动后，源码用了 key，而 yaml 被另一轮修改覆盖。
- **处理**：补回中英文 key 后构建通过。
- **经验**：新增 `tr(STR_*)` 时同时改 `english.yaml` 与 `chinese.yaml`；构建前的 i18n 校验就是最后防线。

## 3. 这块板的交互约束（4 键）

- 物理键：Back、Confirm、Left、Right；没有独立 Up/Down。
- 方向键**按住 650 ms** 会被 SDK 合成为 Up/Down 按下，松开时发 Up/Down 释放。长按操作要监听 Up/Down，而不是 `wasLongPressed(Left/Right)`。
- Confirm 有 300 ms 双击窗口：短按的 pressed+released 会在窗口结束后一起上报；长按超过 300 ms 进入 ConfirmHold，释放时上报 release。
- 列表里用 `wasLongPressed(..., 600)` 消费长按并抑制释放，避免同一次手势再触发导航。

## 4. 音频链路与换算

```
MusicPlayerActivity / RecorderActivity        (crossmux)
        ↓
MusicSession / RecordingSession               (crossmux)
        ↓
HalAudioOutput                                (crossmux, lib/hal)
        ↓
AudioManager                                  (freeink-sdk)
        ↓
I2S TX/RX → ES8311 DAC/ADC → PA/麦克风          (hardware)
```

- ES8311 `0x32`（DAC）/ `0x17`（ADC）音量：0.5 dB/步，`0x00 = −95.5 dB`，`0xBF = 0 dB`，`0xFF = +32 dB`。
- 麦克风 PGA：`0x16 = 0x07` 为 +42 dB。
- 录音格式：44 字节 RIFF/WAVE，PCM，单声道 16-bit，16 kHz（标准）/32 kHz（高音质）。
- 采集块：512 帧立体声 → 取 L/R 均值；起始丢 120 ms、再 120 ms 淡入。
- 1 小时文件：16 kHz ≈ 115 MB；32 kHz ≈ 230 MB。

## 5. 调试工具

```bash
# 构建 / 烧录
pio run -e waveshare_epaper_397
pio run -e waveshare_epaper_397 -t upload --upload-port /dev/cu.usbmodem11201

# 串口探针（CROSSMUX_DEBUG_SERIAL_COMMANDS）
CMD:REC <sec>            # 录 N 秒到 /rec/probe.wav，逐秒打印 acRms/peak
CMD:RECPLAY [index]      # 用共享播放器播放 /rec 第 index 条并打开播放页
CMD:VOL <pct>            # 设置音量并回读 0x32
CMD:SCREENSHOT           # 800×480 1-bit 原始帧缓冲
CMD:VOICEMEMO            # 进入录音列表
CMD:VOICEREC             # 直接进入录音页并开录
```

截图解码（宿主 Python + Pillow）：

```python
from PIL import Image
raw = open('shot.raw', 'rb').read()
Image.frombytes('1', (800, 480), raw).rotate(-90, expand=True).save('shot.png')
```

## 6. 仓库边界与提交流程

- **crossmux**：Activity、UI、i18n、设置、`lib/hal`、`MusicSession`、录音文件格式与列表。
- **freeink-sdk**：`BoardConfig`、驱动、I2S、ES8311 寄存器与音量曲线。
- 修改 SDK 的顺序：
  1. `freeink-sdk` 提交；
  2. 父仓库更新子模块指针并提交；
  3. 先推子模块，再推父仓库（否则 gitlink 指向远端不存在的 commit）。
- `.gitmodules` 指向 `losoco/freeink-sdk`（`branch = develop`）；父仓库远端为 `losoco/AiLinkOS`。

## 7. 下次做音频功能的检查清单

- [ ] 先确认能力宏：`CROSSPOINT_CAP_MUSIC_PLAYER` / `CROSSPOINT_CAP_VOICE_MEMO`。
- [ ] 采集/播放是否在 Activity loop 中持续拉流？是则加 `skipLoopDelay()`。
- [ ] 启动路径是否触发全卡扫描（`getSpace`、目录遍历）？
- [ ] 新 `tr(STR_*)` 是否同时写入 `english.yaml` 和 `chinese.yaml`？
- [ ] 音量/增益是否用 dBFS 实测过，而不是只看百分比？
- [ ] 改动寄存器后，是否用 `CMD:VOL` / `CMD:REC` 回读验证？
- [ ] 二进制/布局改动是否同步脚本、测试与文档？
- [ ] 分区/镜像大小是否重新测量，并确认 OTA 兼容性？
- [ ] 子模块改动是否单独提交，父仓库指针是否已更新？

## 8. 遗留事项

- 录音电平本身偏低；若希望 WAV 直接更响，可在采集侧提高 ADC 数字音量（`0x17`），但会抬底噪，需实测。
- 德文断词 trie 占 206 KB，是全语言 i18n 之外最大的可裁剪项。
- 旧分区表设备的 OTA 迁移方案尚未设计。
- 播放/录音后的音乐与电台回归需要每次硬件验收时复测。
