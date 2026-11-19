# 网络电台（Internet Radio）

> 目标板：**Waveshare ESP32-S3 e-Paper 3.97**（`CROSSPOINT_CAP_RADIO=1`）。
> 参考实现：[mac_radio](../../../mac_radio)（macOS 端，JSON 站点表 + AVFoundation）。
> 本文件是电台功能的系统记录：目录约定、支持的格式、播放链路、内存取舍与已知限制。

## 1. 范围

| 项目 | 结论 |
|---|---|
| 支持 | **MP3 over HTTP/HTTPS**（Icecast/SHOUTcast 直播流，含 ICY 元数据） |
| 不支持 | HLS（`.m3u8`）、AAC、OGG。HLS 需要播放列表解析 + TS 解复用，v1 不做；条目仍会导入并在播放时报错 |
| 站点来源 | SD 卡 `/radio` 目录，见 §2 |
| 音频输出 | 复用 `HalAudioOutput` / `AudioManager`（ES8311 + I2S），与本地音乐互斥 |
| 编译开关 | 仅 waveshare env 打开 `-DCROSSPOINT_CAP_RADIO=1`；代码用 `#if CROSSPOINT_CAP_RADIO` 包裹 |

## 2. 从 SD 卡导入

固定目录 `/radio`，不递归。同名/多格式可混放，按文件名排序合并：

| 文件 | 格式 | 分类字段 |
|---|---|---|
| `stations.json` | `[{"category","name","url"}, …]`，与 mac_radio 的 `MacRadioStations.json` 相同 | `category` |
| `*.m3u` | `#EXTINF:-1,名称` 后跟 URL；无 EXTINF 时用 URL 作名称 | 文件名（去扩展名） |
| `*.txt` | 每行 `名称,URL` | 文件名（去扩展名） |

- 上限 `kMaxStations = 1024`；超出部分忽略。
- 站点表（280 B/台，满额约 280 KB）与单文件解析缓冲（512 KB）都走 PSRAM
  （`HalMemory::allocatePsram`），不占内部 RAM——内部堆要留给解码器和 TLS 会话。
  没有 PSRAM 的板子会拒绝导入并记录日志。
- JSON 以可变缓冲传给 ArduinoJson，字符串零拷贝指向 PSRAM 缓冲，只有节点占用内部堆。
- 字段上限：名称 64 B、URL 192 B、分类 24 B；超长按 UTF-8 边界截断。
- 单文件上限 512 KB；解析失败只记录日志，不影响其它文件。
- 空目录、无 `/radio`、无 SD 分别给出不同空态文案。
- 编辑完列表后，在曲目页**长按返回**重新导入（无需重启）。

示例：

```json
[
  {"category":"音乐","name":"轻听 1085","url":"https://lhttp-hw.qingting.fm/live/1085/64k.mp3"}
]
```

## 3. 播放链路

```
RadioListActivity ──Confirm──> RadioPlayerActivity
        │                            │
        └── RadioSession.playIndex ──┘
                    │
      ensureWifi()  │  NetworkStartup::setMode(STA) + 保存的凭据
                    ▼
        RadioStream::open()  HTTP/1.0 GET + Icy-MetaData:1
                    │         ├─ 跟随最多 3 次 302
                    │         ├─ 解析 icy-metaint，剥离元数据块
                    │         └─ minimp3 解帧，先拿到采样率/声道
                    ▼
        HalAudioOutput::playMusicPcm(readThunk, …)  ← 音频任务拉 PCM
```

- 连接与握手发生在 **app 主循环**（`RadioSession::poll()`），不在音频任务；因此会阻塞输入几秒，页面先绘制「连接中…」。
- 音频任务通过 `RadioStream::readPcm()` 阻塞拉流；断流时返回 0，`poll()` 把状态转成 `Error`，再次按 Confirm 会重连（直播无断点续播）。
- 暂停 = 关闭 socket；继续 = 重新连接并回到直播边缘。
- 开始电台会先停掉本地音乐（同一 codec）；进入深睡、USB Drive 接管 SD 时都会停止。

## 4. 内存与任务栈

- `RadioStream`：8 KB 环形输入 + 2 KB 帧缓冲 + 1152×2 立体声 PCM（约 15 KB，静态）。
- minimp3 解码器约 7 KB，随 `open()` 分配、`close()` 释放。
- **音频任务栈**：TLS 握手后内部 RAM 会碎片化，实测最大连续块约 23.5 KB，小于任务需要的 24 KB。`AudioManager` 因此在 `xTaskCreatePinnedToCore` 失败时回退到 `.bss` 里的 24 KB 静态栈（`xTaskCreateStaticPinnedToCore`），否则联网播放会在启动音频时静默失败。代价是常驻 RAM 增加 24 KB（整机约 47%）。
- TLS 只在 HTTPS 源使用；`setInsecure()` 跳过证书校验（无系统时钟与根证书包，公开音频流可接受）。

## 5. 交互

| 页面 | 按键 | 行为 |
|---|---|---|
| 曲目/电台列表 | Up/Down | 移动；长按翻页 |
| | Confirm | 播放并进入播放页 |
| | Back | 退出应用（继续后台播放） |
| | Back 长按 | 重新导入 `/radio` |
| 播放页 | Confirm | 播放 / 暂停 |
| | 左/右短按 | 音量 ±5，底部浮层 3 秒 |
| | 左右长按（Up/Down） | 上一台 / 下一台（阻塞几秒重连） |
| | Back | 回列表，继续播放 |
| | Back 长按 | 停止并回列表 |

## 6. 已知限制

1. 连接是同步的：Wi-Fi 加入最多 15 秒、握手 5 秒级，期间按键无响应（页面已先绘制「连接中」）。
2. 只支持 MP3；AAC/HLS 源会显示「连接失败」，需要后续版本。
3. 直播无缓冲策略：网络抖动会直接卡顿，不做自适应缓冲。
4. 音量与本地音乐共用 `SETTINGS.musicVolume`（同一条输出通路）。

## 7. 验证

1. 在 SD 卡建 `/radio`，放入 §2 任一格式的列表；进入 Apps → 网络电台，列表应显示导入的站点与分类。
2. 确认 Wi-Fi 已保存凭据；选中一个 MP3 源，页面显示「连接中…」→「播放中」，串口出现 `[RAD] Playing <名称> (<rate> Hz <ch> ch)`。
3. 返回列表后该行显示「播放中」；再次进入不重启播放。
4. 暂停/继续、上一台/下一台、音量浮层逐一验证。
5. 断开网络：约 6 秒后状态变为「连接失败」，按 Confirm 可重试。
