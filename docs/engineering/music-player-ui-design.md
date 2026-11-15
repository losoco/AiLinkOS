# 音乐播放器 — 界面与交互设计稿

> 状态：**v1 与后台播放（v2）已实现，改动留在工作区待评审**（未提交）。本文同时作为该功能的系统记录：
> 评审稿的界面与交互约定未变，实现期确定的技术选择记录在 §1.1 与 §14。
>
> 范围：MP3（minimp3）与 16-bit PCM WAV（自实现的 RIFF 解析，无第三方库）。
> 界面、文件列表、播放控制按 CrossMux 自身的 Activity/UITheme/tr()/逻辑按键体系实现。
> 不移植源工程的 `sdcard_bsp`、`i2c_bsp`、`es8311_bsp` 与图标/界面素材。

## 1. 硬件与编译边界

| 项目 | 结论 |
|---|---|
| 目标板 | 首发只支持 **Waveshare ESP32-S3 e-Paper 3.97**（`waveshare_epaper_397` env）。该板带 ES8311 + 功放，与源工程硬件一致，见 [waveshare-epaper-397.md](waveshare-epaper-397.md) |
| 不支持 | Xteink X3/X4（ESP32-C3）在 `BoardConfig` 中是 `NO_AUDIO`，物理上没有 codec，音乐入口不显示、代码不编译 |
| 编译开关 | 仅 waveshare env 增加 `-DCROSSPOINT_CAP_MUSIC_PLAYER=1`；所有音乐相关入口与实现用 `#if CROSSPOINT_CAP_MUSIC_PLAYER` 包裹 |
| 防御 | 若某板型误开该宏但 `BoardConfig::ACTIVE.audio.output == None`，用编译期 `static_assert` 直接报错，不做运行期降级 |
| 音频通道 | 复用现有 `HalAudioOutput`（板级音频上电/功放/音量）与 `freeink::AudioManager`（I2S + ES8311），不新增音频驱动层 |
| 解码器 | MP3：minimp3（CC0）；WAV：自实现 RIFF 解析，只接受 16-bit PCM |
| 采样率 | **方案 B**：AudioManager 按文件实际采样率重配 I2S 时钟与 ES8311 寄存器（MCLK = 256×fs），44.1 kHz 原速播放；不在解码侧重采样 |

### 1.1 决策记录（v1 实现期）

| 决策 | 候选 | 选择与理由 |
|---|---|---|
| 44.1 kHz 播放 | ① 软件重采样到 16 kHz ② SDK 动态时钟 ③ 只播 16 kHz 文件 | **②**。重采样要额外的插值器与缓冲区、且有音质损失；只播 16 kHz 会把大量现成文件排除。方案 B 的代价是改 SDK 与 codec 寄存器，收益是原速原音质 |
| SDK 改动位置 | 改 `HalAudioOutput` / 新增平行驱动 / 改 `AudioManager` | **`AudioManager`**。I2S 通道、`currentRate_`、codec 初始化都在这里；另起一套会与提示音争用 I2S0 与 I2C。只新增 `codecConfigureRate()` 与去掉 `ensureI2s()` 的 16 kHz 假设 |
| 16 kHz 提示音 | 保留原路径 / 与新路径合并 | **保留**。`ES8311_MCLK_16K_INIT` 与 `codecConfigureRate(16000)` 写出的字节完全一致；`codecRate_ == 16 kHz` 时直接跳过重写 |
| 解码任务 | 新增专用任务 / 复用 AudioManager 的 `audio_play` 任务 | **复用**。少一个常驻任务与一份栈；代价是该任务栈从 8 KB 提到 24 KB（minimp3 单帧约 13 KB 栈），只在播放期间存在 |
| WAV 支持范围 | 全部位深 / 仅 16-bit PCM | **仅 16-bit PCM（含 EXTENSIBLE+PCM 子格式）**。8/24/32-bit 与 ADPCM/float 需要额外的样本转换路径，v1 收益低；其余变体走 `ErrorDecode`，不崩溃、不静默 |
| 音量落盘位置 | lib 读设置 / Activity 读设置 | **Activity**。`lib/MusicPlayer` 不依赖 `src/`（设备构建没有 `-Isrc`），设置读写留在 UI 层，关闭浮层时落盘 |
| 提示音与音乐并发 | 允许共用 / 音乐期间静音提示音 | **音乐独占输出**。`HalAudioOutput` 用 `musicOwnsOutput` 丢弃提示音，避免按键把 codec 重启、把歌切断 |
| 播放页波轮手势 | ① 软件计时长按（`wasLongPressed(Left/Right, 600)`）② 直接用 SDK 合成的 Up/Down | **②**。本板按住 GPIO4/6 达 650 ms 后，`Left/Right` 不再保持按下，软件计时永远等不到阈值；SDK 已在 650 ms 把该手势转成 `Up/Down` press，释放也只产生 Up/Down release，天然满足“阈值触发、不连发、短长互斥” |
| 音量反馈 | ① 每次调整弹浮层 ② 常驻音量条 ③ 不给反馈 | **① 复用现有浮层**。常驻会永久占用播放页空间；无反馈又无法确认当前音量。非模态 + 3 秒超时最多一次整帧刷新，符合 e-ink 刷新预算 |
| 音量刷新方式（v2） | ① 继续 `HALF` ② 整帧 FAST ③ 窗口 FAST | **③**。本板 HALF 是全屏 OTP 波形（闪黑），整帧 FAST 不闪但整屏驱动、残影范围大；驱动已有 `displayWindow`，接线后只驱动浮层矩形，且进度/时间可复用同一能力。详见 §8 |
| 切歌刷新方式（v2.1） | ① 保留整屏 `HALF` 清洗 ② 整帧 FAST ③ 脏区并集窗口 FAST | **③**。用户明确不接受切歌闪黑；切歌改动的曲名块/状态行/进度条/时间恰好连续，取并集后一次窗口更新即可覆盖，且驱动临时缓冲约 23 KB、返回即释放。残影改由唯一的周期清洗兜底，见 §8.3 |
| 列表页自动续播刷新（v2.1） | ① 保留 `HALF` ② 跟随列表默认 FAST | **②**。自动续播也是切歌，浏览列表时整屏闪一下同样不可接受；列表徽标只是文本变化，FAST 足够 |
| 后台会话归属（v2） | ① 由 Activity 持有 ② 单例常驻 | **②**。音频由 SDK 的 `audio_play` 任务持续拉流，`MusicSession` 已是唯一状态源；Activity 退出只销毁画面，不碰解码器与音频轨 |
| 全局 tick 位置（v2） | ① `ActivityManager::loop()` 顶部 ② `main.cpp::loop()` | **②**。`main.cpp::loop()` 是唯一的应用级周期点，且在独占存储、手势等早退分支之前；不在 `ActivityManager` 里引入音乐库依赖 |
| 全局状态可见性（v2） | ① 状态栏音符 ② Home 新增菜单项 ③ Apps 行徽标 ④ 不显示 | **①**。INX 状态条左端在电池对侧本身就是空白，音符只画在既有重绘帧上，不新增刷新。Home 菜单是静态条目表 + `static_assert` 索引，插一项会破坏索引；Apps 徽标在图标网格布局下无处可画 |
| 深睡停播位置（v2） | ① 各 Activity `prepareForSleep()` ② `enterDeepSleep()` 全局钩子 | **② 为主 + ① 兜底**。后台播放时当前 Activity 可能是阅读页，只有全深睡入口能保证停播；功放与音频轨也在这里统一断电 |
| 低电量停播（v2） | ① 不处理 ② 电池 ≤3% 且未充电时停播 | **②**。解码 + 功放是此时唯一可削减的大负载；停播路径不写 flash，避免掉电写坏设置 |
| 后台错误释放输出（v2） | ① 保持 `musicOwnsOutput` ② DMA 排空后释放 | **②**。否则一次解码失败会让提示音永久静音、音频轨常开；`poll()` 在 `isPlaying()` 转 false 后调用 `HalAudioOutput::shutdown()` |


## 2. 入口

- Apps 菜单新增一项：图标 `UIIcon::Music`，标题 `tr(STR_MUSIC_TITLE)`。
- 稳定 ID：`AppId::Music = 18`（当前 `Count = 18`，追加后 `Count = 19`），见 [AppVisibility.h](../../src/AppVisibility.h)。
- 参与现有“隐藏应用”位图，默认可见；不做单独的开关设置。
- 不新增顶部 Tab，不占用首页。

## 3. 信息架构与页面状态机

两个 Activity 共用同一个播放会话单例（`MusicSession`），列表页与播放页可来回切换而不打断播放：

```
[Apps 菜单]
    │ Confirm
    ▼
 List ──Confirm/点击行──> Player
  ▲  │                      │
  │  └────── Back ──────────┘   回到列表，继续播放
  │
  └── Back（根页面）──> 退出应用，会话常驻并继续后台播放

  （会话直到深睡 / 播放页长按 Back / 解码失败 / USB Drive 接管 SD 才释放音频轨）
```

内部状态：

| 状态 | 含义 | 屏上表现 |
|---|---|---|
| `ListEmptyNoSd` | 未检测到 SD | 居中提示 + 返回键提示 |
| `ListEmptyNoFolder` | 无 `/music` 目录 | 居中提示，给出期望路径 |
| `ListEmptyNoFiles` | 目录内无 MP3/WAV | 居中提示，提示放入文件后可长按返回重扫 |
| `ListReady` | 有可播放文件 | 文件列表 |
| `Playing` | 正在解码输出 | 播放页，进度按秒推进 |
| `Paused` | 已暂停，保留位置 | 播放页，进度静止 |
| `Stopped` | 已停止，位置归零 | 播放页或列表 |
| `ErrorDecode` | 文件损坏/读取失败 | 播放页错误行，停止输出 |
| `ErrorRate` | 采样率不受当前实现支持 | 播放页错误行，停止输出（不自动跳歌） |

状态迁移：

| 起点 | 事件 | 终点 | 副作用 |
|---|---|---|---|
| List | Confirm | Player / Playing | 建会话、上电音频、开始解码 |
| Player | Confirm | Playing ↔ Paused | 暂停保留位置；继续从原位置 |
| Player | Left / Right | Playing（新曲目） | 归零进度、切文件、重配采样率 |
| Player | 曲目自然结束 | Playing（下一首） | 列表循环；单曲列表即单曲循环 |
| Player | 解码失败 | ErrorDecode | 停止输出；不自动跳下一首 |
| Player | 采样率不支持 | ErrorRate | 停止输出；不自动跳下一首 |
| Player | Back | List | 继续播放，列表行标“播放中” |
| Player | Back 长按 | List | 先停止播放 |
| List | Back | 退出应用 | 会话常驻，继续后台播放（v2） |
| 任意 | 自动睡眠 | 睡眠 | 播放中抑制自动睡眠；未播放时正常深睡 |
| 任意（含后台） | 电源键睡眠 / 关机 | 睡眠 | `enterDeepSleep()` 先 `shutdown()`：停解码、静音功放、断音频轨 |

会话生命周期（v2）：

| 行为 | 会话 | 音频轨 |
|---|---|---|
| 列表 Back 退出 Apps | 保留曲库/曲目/位置/音量 | 播放中保持上电 |
| 阅读、书架、Home、设置等任意界面 | 保留 | 播放中保持上电 |
| 播放页长按 Back | 保留曲目与音量，状态转 `Stopped` | 断开（`HalAudioOutput::shutdown()`） |
| 解码失败 / 采样率不支持 | 保留曲目，状态转错误 | DMA 排空后断开；提示音恢复 |
| 深睡（自动或手动）、USB Drive 接管 SD | `shutdown()` 清空会话 | 先断开 |

边界约定：

- 上一首/下一首**不提供 seek**，总是整曲切换；进度条只读（v1 不做拖动定位）。
- 列表循环固定开启，v1 不做随机/单曲循环开关。
- 播放中删除文件或拔卡：读失败即 `ErrorDecode`，返回列表时强制重扫。
- 空列表时 Confirm 不产生任何副作用，不做“自动创建 /music 目录”的写操作。

## 4. 文件列表页

### 4.1 扫描规则

- 固定根目录 `/music`，**不递归**：扫描时间与内存可预期，也与源工程 `sdcard/music` 的约定一致。
- 只收 `.mp3` 与 `.wav`，扩展名比较大小写不敏感。
- WAV 在打开时才校验 RIFF 结构：非 PCM、非 16-bit、声道数异常走 `ErrorDecode`，不影响列表显示。
- 跳过隐藏项与系统目录（`._*`、`System Volume Information`）。
- 排序复用 `FsHelpers` 的自然排序（大小写不敏感），保证每次进入顺序稳定。
- 条目上限 1000 个；行数据按需生成（pull 式 row provider），不预建每个文件名的字符串数组。
- 目录不存在与目录为空是两种不同空态，文案不同。

### 4.2 列表行

沿用文件浏览器的行模型（[FileBrowserActivity.cpp](../../src/activities/home/FileBrowserActivity.cpp)）：

```
[icon] 夜空中最亮的星                        MP3
        ↑ 两行折行，第二行超出部分省略
```

| 元素 | 规则 |
|---|---|
| 图标 | `UIIcon::Music` 经 `listIconFor()` 渲染，24 px |
| 主标签 | 去掉 `.mp3` / `.wav` 后缀的文件名；`label.maxLines = 2`，超出以省略号截断；不做跑马灯 |
| 右侧值 | 普通行为该行的容器格式 `MP3` 或 `WAV`；当前播放行为 `tr(STR_MUSIC_PLAYING)`，当前暂停行为 `tr(STR_MUSIC_PAUSED)`，错误/停止后回落为格式值（v2） |
| 选中行 | 由主题的选中态负责，不额外画箭头 |
| 长文件名 | 依赖 SDK label 的 2 行折行 + 末尾省略；不缩放字体、不横向滚动 |
| 行属性 | 与文件浏览器一致：`rowProvider` 按需生成、`valueInset = 8`、`balanceWrappedLabelWithValue = false`，见 [FileBrowserActivity.cpp](../../src/activities/home/FileBrowserActivity.cpp#L612-L627) |

### 4.3 空态

居中两行文本（`drawCenteredWrappedText`，最多 2 行），下方沿用标准按键提示：

- 无 SD：`tr(STR_MUSIC_NO_SD)`。
- 无目录：`tr(STR_MUSIC_NO_FOLDER)`，第二行显示 `/music`。
- 无文件：`tr(STR_MUSIC_EMPTY)`，第二行提示可长按返回键重扫。

### 4.4 翻页与滚动

完全复用 `UiListActivity` 既有行为，不新写导航：

- Up/Down 单击：逐行移动选择。
- Up/Down 长按：整页翻动。
- 选择移动时自动滚动视口，保留部分可见的尾行。
- 右侧滚动条使用 `GUI.drawSideScrollBar`，条目不足以滚动时不画。

## 5. 播放页

### 5.1 800×480 布局

```
┌────────────────────────────────────────────────┐
│ 音乐                                [电量]     │  drawHeader
├────────────────────────────────────────────────┤
│                                                │
│              夜空中最亮的星                     │  标题：最多 2 行，居中
│                                                │
│           MP3 · 44.1 kHz · 3 / 12              │  副标题：小字号，1 行
│                                                │
│                  播放中                        │  状态标记：文字
│  ███████████████████░░░░░░░░░░░░░░░░░░░░░░░░   │  进度条
│  02:13                                   04:31 │  已播 / 总时长
│                                                │
├────────────────────────────────────────────────┤
│   返回        播放|暂停    音量-     音量+      │  GUI.drawButtonHints
└────────────────────────────────────────────────┘
```

| 元素 | 内容与规则 |
|---|---|
| 标题 | 文件名去后缀；最多 2 行；居中；超长省略 |
| 副标题 | 实际格式 + 实际采样率 + `第 N / M 首`。MP3 显示 `MP3 44.1 kHz`；WAV 额外显示位深与声道，如 `WAV 44.1 kHz 16-bit 2ch`。数值全部来自解码器上报，不读扩展名、不写死 MP3 |
| 状态标记 | 纯文字：播放中 / 已暂停 / 已停止 / 无法播放 / 不支持的采样率；不使用彩色底纹 |
| 进度条 | 轨道 = 1 px 描边 + 浅灰抖动底；已播 = 实心黑；高度 `clamp(rowHeight / 5, 6, 12)` px |
| 时间 | SMALL 字号；左为已播 `mm:ss`，右为总时长；总时长未知显示 `--:--`，进度条只显示已播比例，不画假进度 |
| 封面 | 见 §15（v2.1 已实现）：竖屏为"封面在上 + 文本在下"，宽屏为"左侧正方形图 + 右侧文本列"；无封面/内容区过小回退到当前居中布局 |
| 歌词视图 | 见 §16：同目录有可用 `.lrc` 时默认显示歌词窗口，长按 Confirm 可切回本表的信息布局；进度条与时间行在两种视图下位置不变 |
| 底部提示 | `mappedInput.mapLabels(Back, 播放|暂停, 音量-, 音量+)` 四键标签，不新造提示栏；长按切歌、长按切歌词视图属于二级手势，不占提示位 |

### 5.2 自适应规则

全部坐标来自 `renderer.getScreenWidth()/getScreenHeight()`、`pageContentRect()`、`renderer.getLineHeight(fontId)` 与主题 metrics，禁止硬编码 800/480：

- 标题块垂直中心位于内容区 26% 处；进度条位于内容区 62% 处；时间行紧随进度条下方。
- 内容区高度 ≥ 8 行正文时显示副标题；不足时隐藏副标题并把标题块上移。
- 内容区高度不足 5 行正文时进入紧凑模式：状态标记并入时间行左侧，标题最多 1 行。
- 宽度不足 320 px 时，副标题改用 SMALL 字号并省略“采样率”段，只保留 `MP3 · 3 / 12`。
- 横竖屏都按百分比 + 行高推导，不使用固定像素锚点；高 DPI 主题下字号由主题字体表决定，布局公式不变。

## 6. 按键映射

逻辑按键一律走 `MappedInputManager::Button::*`，不读 `HalGPIO::BTN_*`。
Waveshare 3.97 的物理键：Back=GPIO0、左=GPIO4、Confirm=GPIO5、右=GPIO6；左右长按 650 ms 会转为 Up/Down 事件（见 [waveshare-epaper-397.md](waveshare-epaper-397.md)）。

### 列表页

| 按键 | 单击 | 长按 |
|---|---|---|
| Up / Down | 上/下一项 | 整页翻动（UiListActivity 默认） |
| Confirm | 播放选中项 | 不绑定 |
| Back | 退出应用（继续后台播放） | 重新扫描目录（播放中会先停播） |
| Left / Right | 等价于 Up / Down（由方向映射决定） | 同上 |
| Power | 系统睡眠 | 系统睡眠 |

### 播放页

本板物理上只有 4 个键：Back=GPIO0、上/音量−=GPIO4、Confirm=GPIO5、下/音量+=GPIO6。
GPIO4/6 的**短按**在释放时才产生 `Left`/`Right`；**按住 ≥650 ms** 会被 SDK 的
`FunctionButtonGesture::DIRECTION_HOLD_MS` 转换为 `Up`/`Down` 的 press/hold，释放时只产生
`Up`/`Down` release，不再产生 `Left`/`Right` release。所以“短按 vs 长按”在输入层就是互斥的，
长按每次只产生一个 press 边沿，不会连发，也不需要再叠加一层软件阈值。

| 手势 | 逻辑事件 | 行为 |
|---|---|---|
| 波轮上短按（<650 ms，GPIO4） | `Left` release | 音量 −5（0–100 夹紧），显示音量浮层并重置 3 秒计时 |
| 波轮下短按（<650 ms，GPIO6） | `Right` release | 音量 +5，同上 |
| 波轮上长按（≥650 ms） | `Up` press | 上一首（一次长按触发一次） |
| 波轮下长按（≥650 ms） | `Down` press | 下一首（一次长按触发一次） |
| Confirm 短按 | `Confirm` release | 播放 / 暂停 |
| Confirm 长按（≥600 ms） | `Confirm` long press | 有歌词时在歌词 / 信息视图间切换；无可用 `.lrc` 时无动作 |
| Back 短按 | `Back` release | 返回列表，继续播放 |
| Back 长按（≥600 ms） | `Back` long press | 停止播放并返回列表 |
| Power | — | 系统睡眠（`enterDeepSleep()` 先 `shutdown()`；播放页 `prepareForSleep()` 兜底） |

方向对齐以列表页为准：GPIO4 在列表页里是“上”（`NavPrevious`），因此**音量 − 放在“上”、音量 + 放在“下”**。
播放页底部提示同步为 Back / 播放|暂停 / 音量− / 音量+；长按切歌属于二级手势，只写在本表里，不占提示位。

## 7. 音量浮层

```
        ┌──────────────────────────┐
        │ 音量                  60% │
        │ ────────────●─────────── │
        └──────────────────────────┘
```

- 触发：播放页波轮短按（音量 −/＋）；每次调整都会重置 3 秒计时。
- 关闭：3 秒无操作自动消失；离开播放页或系统睡眠时立即落盘并关闭。
- 非模态：显示期间仍可播放/暂停、长按切歌、按 Back 退出；浮层不再抢占按键。
- 位置：内容区底部、按键提示上方，水平居中；宽度 `min(content.width, 55% 屏宽)`；高度 = 2 行正文 + 2 个主题间距。
- 内容：左 `tr(STR_MUSIC_VOLUME)`，右百分比；下方滑条复用 FreeInkUI `slider`（`trackHeight=6`、`knobWidth=14`、`knobHeight=22`），不新增控件类型。
- 调整：每次短按步进 5，0–100 夹紧；只在浮层关闭/离开页面/睡眠时写入设置，不在每一步都写 SD。
- 音量落盘为 `CrossPointSettings::musicVolume`（`uint8_t`，默认 60），随现有 JSON 设置读写。

## 8. e-ink 刷新策略

### 8.1 能力核实（v2）

之前“本仓库没有窗口级局部刷新”的结论只对 **CrossMux 的封装层**成立，驱动层其实具备该能力：

| 证据 | 结论 |
|---|---|
| `FreeInkDisplay::displayWindow(x, y, w, h)`（`freeink-sdk/.../FreeInkDisplay.h`，SSD1677 实现见 `Ssd1677Driver.cpp`） | 驱动支持窗口级差分刷新；要求 x 与 width 为 8 的倍数，内部固定走 FAST/DU 波形 |
| `HalDisplay` 没有窗口方法；`GfxRenderer::displayWindow` 被注释掉，只留下 `screenRectToAlignedMemRect()` 与 `read/writeFramebufferRegion()` 做准备 | 封装层从未接线，故旧结论“没有窗口刷新”在应用层是对的 |
| 本板 `ssd1677StickyConfig()`：`fastSeqOverride = 0xFF`、`halfSeqOverride = 0` | FAST = 分区/DU，不闪；**HALF 回落到 0xF7 全屏 OTP 波形，会闪黑** |
| 本构建定义 `EINK_DISPLAY_SINGLE_BUFFER_MODE`；`Ssd1677Driver` 在窗口刷新后用新内容回写窗口内的 BW/RED | 后续整帧 FAST 的差分基线仍然正确，窗口与整帧可以混用 |
| `screenRectToAlignedMemRect()` 已处理四方向旋转与 8 像素外扩 | 屏幕坐标 → 面板坐标只需一次转换，不必在业务层关心旋转 |

因此 v2 新增 `HalDisplay::displayWindow()` 与 `GfxRenderer::displayWindow()`，播放页按“本帧改了什么”选择窗口。音量每步闪黑的根因就是旧代码每次 `promoteNextRefresh(HALF)`，而本板 HALF 实际是全屏清洗波形。

| 场景 | 刷新方式 | 频率上限 |
|---|---|---|
| 进入列表页 | 框架首帧（现有 Activity 路径） | 1 次 |
| 列表选择移动 | 跟随 `UiListActivity` 现有路径 | 事件驱动 |
| 进入播放页 | 全帧 `HALF_REFRESH` 一次，清掉上一页残影 | 1 次 |
| 播放进度 / 时间 | **窗口 FAST**：只推进度条与时间带 | ≤ 1 Hz |
| 播放 / 暂停 | 全帧 FAST（状态行与底部提示同时变化） | 事件驱动 |
| 切歌 / 上一首 / 下一首 / 自动续播 | **全帧 `HALF_REFRESH`**：一次性整屏清洗，封面与文字在同一次绘制里画好再推，不额外推白帧 | 事件驱动 |
| 音量浮层出现 / 调整 / 消失 | **窗口 FAST**：只推浮层面板矩形 | 调整期间 ≤ 5 Hz |
| 同一帧内多个区域都变化（如音量 + 进度） | **窗口 FAST**：取所有脏矩形的并集，一次窗口更新 | 事件驱动 |
| 每 180 次窗口 FAST 更新 | 全帧 `HALF_REFRESH` 清理一次；只在没有切歌、音量步进、浮层可见的帧上执行；切歌的 HALF 会把计数清零 | 约 3 分钟 |
| 从播放页返回列表 | 框架返回路径 | 1 次 |
| 暂停超过 30 秒 | 停止一切周期刷新 | 0 |
| 后台播放（任意非音乐界面） | 不刷新；只在下次自然重绘时画状态栏音符 | 0 |

### 8.2 为什么不会闪

- 窗口刷新固定使用 FAST/DU（`0xFF`），没有全屏反相阶段，所以不会出现“闪黑”。
- 绘制仍然整帧进行，只是 `displayWindow` 让面板只驱动窗口区域；窗口外 BW/RED 相同，不会被驱动。
- 音量浮层与进度带分别使用固定来源的矩形，不会因为文字长度变化而改变刷新区域；两者同帧变化时取并集，仍是一次窗口更新。
- 切歌（含自动续播）**故意**请求 HALF：槽位残影只有全屏 OTP 波形能真正擦掉，而切歌这一帧本来就要重画封面与整块文字，清洗不额外增加一次刷新。代价是这一次整屏波形会闪一下，只在用户主动换曲/一曲结束时发生。
- 1 Hz 进度刷新与音量浮层永远不请求 HALF：它们只推自己的窗口，窗口外像素不被驱动。
- 周期性 HALF 仍然保留（每 180 次窗口 FAST，且必须落在没有切歌、没有音量步进、浮层不可见的帧上），用于压掉进度带与偶发窗口残留；它只统计窗口帧，切歌的 HALF 已经清过屏，会把计数清零，两者不重复劳动。

### 8.3 残影处理与物理限制

- 窗口外像素在两次全帧之间保持不变，DU 差分不驱动它们，因此在窗口之间不会互相污染。
- 残影只在窗口内累积：进度带每秒变化、浮层只在调整时出现。
- 切歌顺带做了整屏 HALF 清洗之后，累积方式变成：
  - 进度带是固定高度的一条，1 Hz 更新，旧数字的浅灰轮廓会稳定叠在带内，形成一条很淡的“重影带”；这是两次切歌之间唯一会累积的区域。
  - 曲名/状态行/封面在每次切歌时被整屏波形清掉重画，不再叠加“多重曝光”。
  - 退化只发生在窗口内，且不累积到窗口外；页面主体仍然清晰可读。
- 清理节奏：面板更新计数到 180（约 3 分钟连续播放）且当帧无用户操作时做一次整屏 HALF；切歌的 HALF 也会把计数清零。若音乐长时间暂停（没有周期刷新），计数不再增长，进度带的灰斑会保持到下一次播放/操作后的第 180 次更新。
- 面板只提供整屏波形，无法“只对窗口施加全波形”；所以清理必须整屏进行，这是本硬件的能力边界，不是实现遗漏。
- 进度条已播区用实心黑、底色抖动浅灰，减少中间灰阶造成的大面积拖影；暂停 30 秒后停止一切周期刷新，避免静态画面持续累积。
- 首次上电/唤醒后的第一帧必须整帧刷新（驱动需要建立 BW/RED 基线）；`displayWindow` 只用于已经显示过完整页面的场景。
- 驱动内部按窗口大小临时分配一块缓冲（音量浮层约 8.4 KB、进度带约 8.9 KB），函数返回即释放；同类窗口尺寸固定，堆块可复用，不会随次数增长。切歌走整帧 HALF，不再有约 23 KB 的切歌窗口缓冲。

### 8.4 封面与周期刷新的隔离、残影处理（2026-11-15）

实机反馈"封面像呼吸一样反复变化"和"封面区域持续留残影"。定位结果与做法：

| 问题 | 证据 | 处理 |
|---|---|---|
| 1 Hz 刷新是否驱动了封面 | 新增 `Flush mode=… reason=… rect=… coverHit=…` 日志：进度刷新是 `rect=20,602,440x41`、`coverHit=0`；封面矩形 (96,86,288×288) 不在窗口内 | 脏区本来就不含封面；本轮再补一条硬保证：封面的**像素生成**也移出周期路径（见下） |
| 每帧重画封面 | 之前每帧 `Cover draw`（重新打开缓存 BMP、逐行贴图） | 首次画好后把槽位像素快照到 `coverTile_`（`getRegionByteSize`/`copyRegionToBuffer`），后续帧 `copyBufferToRegion` 回放，日志变为 `Cover restore`。1 Hz 路径不再碰 SD、不再解码，像素逐位不变 |
| 封面槽位残影 | 切歌后旧抖动图案透过新图（"双重曝光"）；先填白再推图（原方案 A）在实机上仍有残留 | 改为**切歌整屏 HALF**：这一帧本来就是重画，直接让全屏 OTP 波形把旧图案擦掉，封面与文字在同一个 framebuffer 里画好后只推一次（2026-11-15 第二轮，见下） |
| 方案演进 | A：槽位先刷白推一次窗口 FAST，再推新图 → 两次窗口 + 白闪，实机仍残留（差分波形对旧灰阶不彻底）；B：把封面并入每 180 次整屏 HALF → 不增刷新但要等最多 3 分钟；C：只靠差分波形 → 已否证；D：切歌直接整屏 HALF，一次成型 | 选 **D**。它不增加帧数（切歌那帧本来就要重画），不由用户额外等待，且是全屏波形唯一能真正做到"擦干净"的时机；代价仅是切歌时闪一次，与用户对"换曲"的心理预期一致 |

第二轮实测（自动化闭环，见 §15.12）：

- 切歌帧：`CMD MUSICNEXT` → `Cover draw 288x288 bpp=1 rect=96,86,288x288 src=…` → `Flush mode=HALF reason=TRACK rect=0,0,0x0 cover=1 coverHit=0 coverPainted=1`，`Wait complete: refresh (1717 ms)`（HALF 全屏波形）。
- 其余帧：`Cover restore` + `Flush mode=WINDOW reason=WINDOW rect=20,602,440x41 coverHit=0`，`Wait complete: refresh (515 ms)`（窗口 FAST）。
- 全程没有 `Cover wipe`（该步骤已删除）。
- 截屏逐像素校验（`renderer` 帧缓冲、面板 800×480 / 100 B 每行、竖屏映射）：三张封面矩形黑占比 0.496/0.646/0.950、哈希互异，跨帧整帧字节差异约 1 万字节（即封面与文字确实换了），封面顶边 y=86，y=78..85 全白（10 px 间隙保留）。

自动续播走同一条检测：`MusicSession::poll()` 在解码排空后调用 `next()`，播放页 `loop()` 用 `index != lastIndex_` 判断切歌，与手动切歌共用 `trackDirty_` → HALF 分支，因此一曲结束自动换曲同样会清洗，不需要额外开关。

## 9. 退出、后台与省电语义

从 v2 起 `MusicSession` 是常驻单例：Activity 退出（Back、Home 手势、进入阅读）**不等于停播**。
真正释放音频轨的路径都走同一个 `MusicSession::shutdown()` / `stop()`：

1. 停止解码任务并等待退出（不 `vTaskDelete` 正在持有 SD 锁的任务）。
2. 关闭 `HalFile`。
3. `AudioManager::stop()`，I2S 停止拉流。
4. 功放静音。
5. `HalAudioOutput::shutdown()`：关闭 AXP2101 的 ALDO1/ALDO2，音频轨断电。
6. 音量落盘（仅由播放页 `onExit()` / `prepareForSleep()` 负责）。

释放与保留路径：

| 路径 | 会话 | 音频轨 | 说明 |
|---|---|---|---|
| 列表根页面 Back、Home 手势、任意非音乐界面 | 保留 | 保留 | 后台继续播放；全局 `MUSIC_SESSION.poll()` 负责自动切歌 |
| 播放页短按 Back | 保留 | 保留 | 回列表，继续播放 |
| 播放页长按 Back | 保留曲目与音量 | 断开 | `stop()`，状态转 `Stopped` |
| 解码连续失败 / 采样率不支持 | 保留曲目，状态转错误 | DMA 排空后断开 | `poll()` 调 `stopOutput()`，释放 `musicOwnsOutput`，按键提示音恢复 |
| 自动睡眠 | — | — | 播放中抑制超时；Paused / Stopped / 错误态允许睡眠 |
| 电源键睡眠、低电量关机 | 清空 | 先断开 | `main.cpp::enterDeepSleep()` 在 WiFi、蓝牙、面板之前先 `shutdown()`；当前 Activity 是阅读页也覆盖 |
| USB Drive 接管 SD | 清空 | 先断开 | `ActivityManager::goToUsbDrive()` 在把原始卡交给主机前 `shutdown()`，避免固件继续从已脱离的文件系统解码 |

后台播放约束：

- 后台不产生任何 UI 刷新：全局 tick 只有 `MUSIC_SESSION.poll()`，曲目自然结束时静默切下一首，不调用 `requestUpdate()`。
- 状态栏播放音符只画在既有重绘帧上，不因播放状态变化主动刷新（见 §14.4）。
- 后台播放不改变阅读路径的刷新策略、前光或刷新率。
- 播放中（含后台）不会自动深睡；手动睡眠必须整机停播，不存在“屏幕睡、喇叭响”。

省电：

- 仅在 `Playing` 时抑制自动睡眠；`Paused` / `Stopped` / 错误态返回 false。
- 列表页本身不上电音频；只有真正开始播放时才调用 `HalAudioOutput` 上电。
- 电池 ≤3% 且未接 USB 时停止播放；该路径不写 flash。`Paused` 已断电，不重复处理。
- 播放中不改变前光/刷新率设置，避免和阅读路径互相影响。

## 10. 文案（i18n）

全部通过 `tr()`，只新增英文与中文；其余语言按现有机制回落英文。

| Key | en | zh |
|---|---|---|
| `STR_MUSIC_TITLE` | Music | 音乐 |
| `STR_MUSIC_NO_SD` | No SD card | 未检测到 SD 卡 |
| `STR_MUSIC_NO_FOLDER` | No /music folder on the SD card | SD 卡上没有 /music 目录 |
| `STR_MUSIC_EMPTY` | No MP3 or WAV files in /music | /music 目录中没有 MP3 或 WAV 文件 |
| `STR_MUSIC_PLAYING` | Playing | 播放中 |
| `STR_MUSIC_PAUSED` | Paused | 已暂停 |
| `STR_MUSIC_STOPPED` | Stopped | 已停止 |
| `STR_MUSIC_PREV` | Prev | 上一首 |
| `STR_MUSIC_NEXT` | Next | 下一首 |
| `STR_MUSIC_PLAY` | Play | 播放 |
| `STR_MUSIC_PAUSE` | Pause | 暂停 |
| `STR_MUSIC_VOLUME` | Volume | 音量 |
| `STR_MUSIC_VOL_DOWN` | Vol- | 音量- |
| `STR_MUSIC_VOL_UP` | Vol+ | 音量+ |
| `STR_MUSIC_VOLUME_DONE` | Done | 完成 |
| `STR_MUSIC_ERR_DECODE` | Cannot decode this file | 无法解码此文件 |
| `STR_MUSIC_ERR_RATE` | Unsupported sample rate | 不支持的采样率 |

评审稿里的 `STR_MUSIC_RESCAN` 未采用：长按 Back 重扫是隐藏手势，空态文案直接给出 `/music` 路径即可，少一个键。

## 11. 实现落点

| 内容 | 位置 |
|---|---|
| 会话/播放状态机 | `lib/MusicPlayer/MusicSession.{h,cpp}` |
| 解码接口 | `lib/MusicPlayer/AudioSource.h`（`AudioSource` + 扩展名判定 + 采样率范围） |
| MP3 解码 | `lib/MusicPlayer/Mp3Source.{h,cpp}` + `minimp3.{h,c}` |
| WAV 解码 | `lib/MusicPlayer/WavSource.{h,cpp}`（RIFF 解析 + 16-bit PCM 直读，无第三方库） |
| 歌词解析 | `lib/MusicPlayer/LrcLyrics.{h,cpp}`（标准 LRC，固定上限表 + 文本 blob） |
| 音频输出适配 | `lib/hal/HalAudioOutput.{h,cpp}`（`playMusicPcm` 独占 + `isPlaying`） |
| SDK 动态时钟 | `freeink-sdk/libs/hardware/AudioManager/src/AudioManager.cpp`：`codecConfigureRate()`、`ensureI2s()` 去掉 16 kHz 假设、`AUDIO_TASK_STACK = 24576` |
| 列表页 | `src/activities/apps/music/MusicListActivity.{h,cpp}`（继承 `UiListActivity`） |
| 播放页 | `src/activities/apps/music/MusicPlayerActivity.{h,cpp}`（自定义 Activity，直接 `GfxRenderer` 绘制） |
| 入口接线 | `AppVisibility.h`、`AppsMenuActivity.cpp`、`ActivityManager::goToMusic` |
| 图标 | `src/components/icons/inx_apps.h` 新增 `Music` 位图；`UIIcon::Music` 走 `listIconFor()` |
| 设置 | `CrossPointSettings::musicVolume`（JSON 读写，默认 60） |
| 全局播放 tick（v2） | `src/main.cpp`：`loop()` 顶部 `MUSIC_SESSION.poll()`；低电量停播；`enterDeepSleep()` 先 `shutdown()` |
| 状态栏播放指示（v2） | `src/components/themes/inx/InxTheme.cpp`：`drawMainTabStatusBar()` 左侧音符（原语绘制，无新位图/新文案） |
| SD 独占前停播（v2） | `src/activities/ActivityManager.cpp`：`goToUsbDrive()` 先 `shutdown()` |
| 窗口刷新（v2） | `lib/hal/HalDisplay.h/.cpp`：`displayWindow()` 转发到 `FreeInkDisplay::displayWindow`；`lib/GfxRenderer/GfxRenderer.h/.cpp`：屏幕坐标 → 8 像素对齐面板矩形，SIMULATOR 退回整帧 FAST |
| 播放页刷新分派（v2） | `src/activities/apps/music/MusicPlayerActivity.cpp`：`fullDirty_/progressDirty_/volumeDirty_` 选择整帧 / 进度带 / 浮层窗口；`volumePanelRect()` 为绘制与刷新的唯一几何来源 |
| 编译开关 | `platformio.ini` 的 waveshare env 增加 `CROSSPOINT_CAP_MUSIC_PLAYER=1` |

### 11.1 SDK 动态时钟细节（方案 B）

`AudioManager::codecConfigureRate(sampleRate)` 仅在 `I2sEs8311Mclk16k` 板型生效，写：

| 寄存器 | 值 | 含义 |
|---|---|---|
| `0x02` | `0x00` | pre-divider 1、multiplier 1（MCLK = 256×fs） |
| `0x03` | `0x10` | ADC OSR |
| `0x04` | `0x10`（>16 kHz）/ `0x20`（≤16 kHz） | DAC OSR，与 `esp_codec_dev` 的 ES8311 表一致 |
| `0x05` | `0x00` | ADC/DAC 分频 1 |
| `0x06` | `0x03` | BCLK = MCLK/4 = 64×fs |
| `0x07`,`0x08` | `0x00`,`0xFF` | LRCK 分频 256 |

I2S 侧由既有 `ensureI2s(sampleRate)` 负责：速率变化时禁用通道 → `i2s_channel_reconfig_std_clock()`（`mclk_multiple = 256`）→ 重新使能。16 kHz 时 `codecConfigureRate()` 因 `codecRate_` 已等于 16000 直接返回，寄存器字节与 `ES8311_MCLK_16K_INIT` 完全一致，提示音路径不变。

### 11.2 内存预算（Waveshare，内部堆空闲约 168 KB）

- minimp3 解码器状态约 17 KB，帧缓冲约 3.5 KB，PCM 中转 4.6 KB；WAV 只有 1 KB PCM 缓冲。
- 解码任务复用 `audio_play`，栈 24 KB（minimp3 单帧约 13 KB），仅在播放期间存在。
- 全部放内部 RAM，不放 PSRAM：解码/I2S 供给对延迟敏感，PSRAM 抖动会造成欠载爆音。

### 11.3 实机验证清单（烧录后逐项确认）

1. 44.1 kHz MP3：音高与时长正确（可用同一首歌的 44.1 kHz 与 16 kHz 版本对比），无变调、无“快放”。
2. 16 kHz 提示音：退出音乐后按键提示音仍正常，音高与改动前一致。
3. 切歌/暂停恢复：MP3↔WAV、44.1↔16 kHz 互切时无明显爆音、无半秒静默；若有个别 click，记录发生在“切歌”还是“恢复”。
4. 长时间播放：连续播放 ≥ 30 分钟，观察是否出现欠载爆音、卡顿、堆水位持续下降（串口日志/`free heap`）。
5. WAV 兼容性：16-bit PCM 单/双声道、16 kHz/44.1 kHz 各一；24-bit 与非 PCM 文件应进入 `ErrorDecode` 而不是播放噪声。
6. 自动睡眠：播放中不睡眠；暂停后能按时深睡；唤醒后再次进入音乐可正常播放。
7. 亮度/温度：功放长时间工作后板子无明显异常发热，音频轨断电后深睡电流正常。

## 12. 至今不做

- 拖动/点击进度条定位（seek）。
- FLAC、AAC、网络电台、蓝牙音频；WAV 只支持 16-bit PCM。
- 播放列表编辑、收藏、随机播放、单曲循环开关、断点续播。
- 歌词：v1 不做；v3 已实现，见 §16。
- 频谱。
- 专辑封面：v1 不做；v2.1 已实现，见 §15。
- 锁屏/睡眠界面上的播放控制。
- 触摸手势（本板无触摸；若后续板型有触摸，行与进度条保留命中区域即可增量接入）。

## 13. 评审验收点

1. 无音频板（X3/X4）编译产物中不存在音乐入口与解码代码。
2. 800×480 下列表行、播放页、音量浮层无重叠；1280×720 与竖屏下只靠公式重排，不出现裁切。
3. 列表长文件名最多 2 行，省略号落在第二行末尾。
4. 播放页静止时无周期刷新；播放时刷新不超过 1 Hz。
5. 音量调节与浮层消失不出现全屏闪黑；进度/时间更新不驱动整屏。切歌（播放页与列表页）不出现全屏波形，只有窗口内替换；窗口内不残留上一级界面内容，窗口外像素保持不变。
6. 只有周期清洗（约 3 分钟、且当帧无用户操作）会触发一次整屏 HALF；忽略它之后页面仍可读，残影按 §8.3 只出现在窗口内。
7. 任意真正退出的路径（长按 Back 停播、自动睡眠、电源键、USB Drive）后，功放静音与音频轨断电都被执行；仅离开音乐 App 时音频轨按 §9 保留。
8. 暂停后空闲超时能正常自动睡眠；播放中不会被自动睡眠打断。
9. 后台播放时前台阅读/列表的刷新次数与无音乐时一致（不因播放新增重绘）。

## 14. 后台播放（v2）

### 14.1 架构

播放会话与 UI 生命周期彻底解耦，沿用仓库既有的单例组织方式：

```
main.cpp loop() ──> MUSIC_SESSION.poll()        全局：自动切歌、错误清理、低电量停播
                        │
                        ├── SDK audio_play 任务 ──> readPcm() ──> 解码器 / SD
                        │        （持续拉流，不依赖任何 Activity）
                        └── HalAudioOutput ──> I2S + ES8311 + 功放

MusicListActivity / MusicPlayerActivity ──> 只读状态、发控制命令
```

- `MusicSession` 是唯一状态源：曲库、当前索引、状态、位置、音量、格式参数。Activity 退出只销毁画面。
- 音频输出由 SDK 任务持续拉流，因此退出 Activity 不会中断声音；`poll()` 只负责“曲目排空后切下一首”这类应用级决策。
- 全局 tick 放在 `main.cpp::loop()`，位于 `ActivityManager` 与所有手势/独占存储分支之前：任何前台界面都能覆盖，也不需要让 `ActivityManager` 依赖音乐库。
- 每帧成本：一次原子读 + 一次状态比较；没有 UI 刷新、没有 SD 访问（除非正好在切歌）。

### 14.2 状态机（含后台）

| 起点 | 事件（任意界面） | 终点 | 副作用 |
|---|---|---|---|
| `Playing` | 解码器排空 | `Playing`（下一首） | `poll()` 调 `next()`；后台不重绘 |
| `Playing` | 任意外部界面获得焦点 | `Playing` | 无；音频轨保持 |
| `Playing` | 播放页 Confirm / 列表重新进入 | `Playing` | 只接管界面，不重播 |
| `Playing` | 解码失败 / 拔卡 / 文件被删 | `ErrorDecode` | `readPcm` 置错；`poll()` 等 DMA 排空后关 codec 与音频轨 |
| `Playing` | 电池 ≤3% 且未充电 | `Stopped` | 全局 tick 停播；不写 flash |
| `Playing` | 深睡 / USB Drive | `Empty` | `shutdown()`：清曲库、停解码、断轨 |
| `Paused` | 深睡 | `Empty` | 同上；暂停态本就不阻止自动睡眠 |
| `Stopped` / 错误 | 深睡 | `Empty` | 无额外代价 |

### 14.3 后台 UI 与刷新

- 后台不调用 `requestUpdate()`、不改前光、不切刷新波形。
- 唯一可见痕迹：INX 状态条左侧的八分音符（见 §14.4）。它只随既有重绘出现，因此离开播放页后的第一帧就会带上；两首歌之间自然切歌不会产生额外的面板刷新。
- 列表页重新进入时选中行对齐 `currentIndex()`，因此后台自动切歌后打开列表，高亮行与正在播放的曲目一致。

### 14.4 全局可见性（改动最小的选择）

`InxTheme::drawMainTabStatusBar()` 在状态条左端（电池对侧）画一个 7×9 px 的八分音符，条件仅为 `MUSIC_SESSION.isPlaying()`：

- 用 `fillRect` 原语拼出头、符干、符尾，不新增位图、字体字形或 i18n 键。
- 仅在带音频能力的板型上编译（`CROSSPOINT_CAP_MUSIC_PLAYER` 守卫），X3/X4 不进入产物。
- 不改变状态条布局：左端本来就是空白，不需要预留宽度，也不会与电量/日期重叠。
- `Paused` 不显示音符；暂停时用户已经能通过“无声”与播放页确认状态。

未采用：Home 新增“正在播放”菜单项（静态表 + `static_assert(indexToMenuItem)` 索引，插入会破坏索引）、Apps 行徽标（图标网格布局没有值列）、阅读页状态栏（阅读页自绘，不经过主题状态条，改动面大）。

### 14.5 音频仲裁

沿用 v1 约定，后台播放不改变规则：

- 音乐播放/暂停期间 `HalAudioOutput` 置 `musicOwnsOutput`，按键提示音被直接丢弃，不会重启 codec、不会切断音乐。
- 音乐真正结束（长按 Back、曲库空、深睡、USB Drive）或因错误停止并完成 `stopOutput()` 后，`musicOwnsOutput` 释放，提示音恢复。
- 暂停时音频轨断电，提示音可正常播放；继续播放时重新上电。
- 后台自动切歌不经过 `AudioManager` 的提示音路径，不产生额外的 codec 配置竞争。

### 14.6 省电与睡眠

- `Playing` 时抑制自动睡眠（`main.cpp` 的超时判断 + `MusicPlayerActivity::preventAutoSleep()` 双层）。
- `Paused` / `Stopped` / 错误态不抑制：暂停后仍会按设置自动深睡。
- 手动电源键、低电量关机、USB Drive 三条路径都会在把控制权交出去之前停播并断轨；不存在“深睡后音频链路常开”。
- 电池 ≤3% 且未接 USB 时停播：解码 + 功放是此时唯一可削减的大负载；停播不写 flash。

### 14.7 异常边界

| 场景 | 行为 |
|---|---|
| 播放中拔 SD / 文件被删 | `readPcm` 读失败 → `ErrorDecode`；DMA 排空后关 codec 与音频轨，提示音恢复；界面显示错误行，不崩溃 |
| 后台播放中解码错误 | 同上；后台不弹窗、不重绘，回到播放页即可看到错误 |
| 采样率不支持 | `ErrorRate`；不自动跳下一首，避免连续错误刷屏 |
| 后台自动切歌时下一首损坏 | `playIndex()` 返回错误态，`poll()` 不会自旋 |
| 电池极低 | ≤3% 且未接 USB 停播；不写设置，避免掉电写坏 flash |
| USB Drive 接管 SD | 进入前 `shutdown()`，清空会话，避免从已脱离的文件系统读 |
| 重复 `poll()` | 幂等；`advancing_` 防止重入，错误态只在 DMA 排空后处理一次 |
| 返回播放页状态 | 曲目、位置、音量、播放/暂停全部来自 `MusicSession`，不重新打开文件、不从头开始 |

### 14.8 资源影响

- 新增代码：`main.cpp` 约 15 行、`InxTheme.cpp` 约 12 行、`ActivityManager.cpp` 3 行、`MusicSession::poll()` 4 行，均为条件编译。
- 无新增任务、无新增缓冲区、无新增常驻堆分配；Flash 增量在 1 KB 量级（远小于剩余约 246 KB）。
- 后台播放期间的额外 RAM 占用与 v1 播放相同（解码器状态 + audio_play 任务栈），不随界面切换增长。

## 15. 专辑封面（v2.1，已实现）

> 状态：**已实现**。§15.11 的四个确认点按原样落地；实现中调整的细节记在 §15.2、§15.10。

### 15.1 封面来源与优先级

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | MP3 内嵌 ID3v2 APIC，图片类型 `3`（Front Cover） | 标准做法；有多个 type=3 时取数据量最大的一个 |
| 2 | MP3 内嵌 APIC，图片类型 `0`（Other） | 部分抓轨工具写 0 而不是 3 |
| 3 | 同目录**同名**图片：`<曲名>.jpg/.jpeg/.png`（大小写不敏感） | "回退"路径；用户可以不改文件、只丢一张同名图 |
| 4 | 同目录公共图片：`cover.jpg` / `folder.jpg` / `front.jpg`（同名顺序） | 整张专辑共用一张；成本几乎为 0，一并支持 |
| 5 | 无 → 不画封面，回到全宽布局 | 不新增文字、不占位；见 15.6 退化规则 |

- 永不使用 APIC 的 `1`（32×32 图标）与 `2`（文件图标），也拒绝 MIME 为 `-->` 的链接帧。
- WAV：v1 只查 3–4 两种同目录图片；不解析 RIFF 内的 `ID3 ` chunk（少见，且 WAV 的 ID3 块多为文本）。
- 同名图片的匹配以"去掉扩展名的完整文件名"为键，避免 `01.mp3` 与 `01.wav` 互相串封面属于可接受行为（同一目录下同名即同一专辑曲目）。
- `MusicSession` 目前只暴露曲目信息、不暴露路径，需要补一个只读 `currentPath()`（不新增状态、不新增分配，返回内部字符串的 const 引用）。

### 15.2 ID3v2 APIC 解析范围

仓库现状：[Mp3Source.cpp](../../lib/MusicPlayer/Mp3Source.cpp) 只按头部 syncsafe 长度**跳过**整个 ID3v2 标签（含 v2.4 footer），不遍历帧、不读任何文本或图片。封面解析是新增代码，不引入新依赖。

| 项目 | v2.3 | v2.4 | v2.2（可选） |
|---|---|---|---|
| 帧头 | 4 字符 ID + 4 字节大端长度 + 2 标志 | 4 字符 ID + 4 字节 **syncsafe** 长度 + 2 标志 | 3 字符 ID + 3 字节长度 |
| 图片帧 | `APIC` | `APIC` | `PIC`，格式字段为 3 字符 `JPG`/`PNG` |
| 帧内布局 | 编码(1) + MIME(0 结尾) + 类型(1) + 描述(编码相关，0 结尾) + 数据 | 同 v2.3 | 编码(1) + 格式(3) + 类型(1) + 描述 + 数据 |
| 去同步 | 标签头 flag `0x80`，作用于整个标签 | 标签头 flag `0x80`（全局）或帧 flag `0x0008`（逐帧） | 标签头 flag `0x80`，全局 |
| 压缩 | 格式 flag `0x0080` | 格式 flag `0x0020`（伴随数据长度指示 `0x0004`） | 无（标签级 `0x40` 压缩直接放弃） |
| 加密 | 格式 flag `0x0040` | 格式 flag `0x0010` | 无 |
| 分组 | 格式 flag `0x0020`，数据前 1 字节组 ID | 格式 flag `0x0040`，数据前 1 字节组 ID | 无 |
| 扩展头 | flag `0x40`，4 字节长度不含自身 | flag `0x40`，syncsafe 长度含自身 | 无 |

推荐实现边界：

- **只流式定位，不把图片读进 RAM**：顺序读取标签流，跳过非图片帧，只解析 APIC 的文本字段；图片数据按 256 B 分块复制，不在堆上保留整图。
- **转换器只接受从 0 开始的文件**（JPEGDEC 的 seek 回调传绝对偏移），所以 APIC 数据先流式复制到临时文件 `/.crosspoint/music/covers/.picture`，再交给既有 JPEG/PNG 转换器；复制过程顺带完成去同步，图片依旧不整体入内存。
- **去同步**：v2.3/v2.2 的标签级去同步（flag `0x80`）用一层 256 B 顺序过滤器实现，`0xFF 0x00` 还原为 `0xFF`；帧长度按去同步后的字节数解释，边界与流读取一致。**v2.4 逐帧去同步（帧 flag `0x0008`）未实现**：需要第二层过滤视图且现实标签几乎不出现，按"不支持的帧"跳过并回退，见 §15.10。
- **压缩帧直接放弃并回退到 3–4 号来源**：解压需要接 `miniz`/`uzlib` 的流式 inflate 并保证长度守恒，代码量与出错面远大于收益（压缩 APIC 极少见）；不做负缓存。
- 加密帧、链接帧（MIME `-->`）、带数据长度指示的帧同样跳过。
- 安全上限：标签 ≤ 16 MB、单帧 ≤ 8 MB、帧长度不越过标签体；越界一律视为无封面，绝不按损坏长度分配或读取。
- 编码字节只用于跳过描述字段（UTF-16 描述按 2 字节对齐找终止符），图片数据本身与编码无关。
- MIME 只用于选择首选转换器；声明错格式时再试另一个转换器，由真实解码结果兜底。
- 不处理 EXIF 旋转（JPEGDEC 不解 EXIF）；手机拍的竖封面可能横向显示，属于已知限制，可在文档中提示用户改用同目录图片。

### 15.3 解码与缩放

复用现有能力，不新增库：

- JPEG：[JpegToBmpConverter.cpp](../../lib/JpegToBmpConverter/JpegToBmpConverter.cpp) 已有 `jpegFileTo1BitBmpStreamWithSize(file, print, maxW, maxH)`：DCT 1/2、1/4、1/8 预降采样 → 行级细缩放 → Atkinson 抖动 → 直接写 1-bit BMP。解码器 17.9 KB scratch 通过 `buildscratch::claim()` **借用 48 KB 帧缓冲**，用完即还（图书封面走的就是这条路径）。
- PNG：[PngToBmpConverter.h](../../lib/PngToBmpConverter/PngToBmpConverter.h) 的 `pngFileTo1BitBmpStreamWithSize()`，白底合成透明像素。
- 输出尺寸按版面槽位（见 15.6）一次到位；`crop=true` 时转换器输出"覆盖"尺寸（较长边可能大于槽位，例如 1000×500 的源出 400×200），绘制用 [GfxRenderer::drawBitmapCropToFill()](../../lib/GfxRenderer/GfxRenderer.cpp) 居中裁剪到槽位。该路径在缓存图不短于槽位时是 1:1 取中心区域，不做最近邻重采样，抖动点不会被二次打散。
- 耗时：与图书封面同一路径，预算 ≤ 250 ms（含 SD 读）。超出预算或实测有音频断音，就把转换拆成"每帧处理若干 MCU 行"的分片版本，仍复用同一解码器回调；本稿先按一次性转换设计。
- 不引入新依赖的取舍：可选方案是接一个专门的封面/EXIF 库（如 JPEGDEC 之外再加解码器），收益只有 EXIF 旋转与渐进式以外的边角格式，代价是 Flash 与维护面，否决。
- **4:4:0 类 JPEG（2026-11-15 修复）**：ffmpeg 写入 MP3 的封面是 `Y(1,2)+Cb(1,2)+Cr(1,2)`（每 MCU 6 块），而 vendored JPEGDEC 的灰度路径在读 SOF 时丢弃了色度采样因子、且解码循环对色度硬编码 1 块/MCU，熵流每 MCU 少消费 2 块，若干 MCU 后 Huffman 解码失败（`JPEG decode failed (rc=0, err=2)`），`JpegToBmpConverter` 判失败后被 `MusicCoverLoader` 静默回退 PNG（对 JPEG 数据必然 "Invalid PNG signature"）。修复见 `scripts/jpegdec_patches/0004-consume-sampled-chroma-blocks.patch`：解析 `h_samp_factor/v_samp_factor`，按采样因子循环消费色度块（仅灰度路径，RGB 路径仍只支持 4:4:4/4:2:0）。验证：本地 JPEGDEC harness 与 `ffmpeg -pix_fmt gray` 参考图 mean abs diff 0.037；实机 600×600 / 800×800 / 640×640 三张 JPEG 封面全部 `Convert JPEG -> 288x288 ok` 并绘制。

### 15.4 缓存

- 缓存文件：`/.crosspoint/music/covers/<fnv1a64(path|size|mtime|slot)>.bmp`，1-bit BMP，尺寸按槽位（200×200 ≈ 5.1 KB，含 BMP 头）。键含槽位，屏幕/布局变化时不会误用旧尺寸的图。
- 键含文件大小与 mtime：换封面/换文件自然换键，不需要失效逻辑；读失败、尺寸不符或数据被截断则删除重生成（沿用 `BookCoverLoader` 的"发现坏缓存即删除重建"做法）。已知边界：只替换同目录图片、不动音频时键不变，v1 需要手动删缓存或改动音频文件才会更新。
- 目标目录 `/.crosspoint/music/covers` 逐级创建（SdFat 的 `mkdir` 不建父目录）。
- 命中时只做一次 BMP 打开 + 逐行贴图，无解码、无 ID3 扫描。
- 清理：在**曲库重扫**（首次加载/长按 Back 重扫）后执行；目录内 `.bmp` 数 > 200 时，按 mtime 升序删到 150。上限约 1 MB SD 空间。不放在列表页 `onEnter()`，避免每次返回列表都付一次目录枚举。
- 临时文件 `/.crosspoint/music/covers/.picture` 在转换后立即删除；重扫清理时也会顺手删除上次异常断电留下的残片。
- 不做负缓存（无封面不落盘）：一次 ID3 头部扫描只有几 KB 读，比维护 `.none` 标记便宜。
- 不做 PSRAM 常驻缩略图：单张 5 KB，重进播放页从 SD 读一次即可（毫秒级），省下的是常驻内存与失效逻辑。

### 15.5 面板能力与抖动

| 项目 | 事实（代码依据） |
|---|---|
| 灰度级 | `BoardConfig` 默认 `grayscaleLevels = 4`；Waveshare 397 板型未覆盖该字段，即 2-bit / 4 级灰 |
| 本板灰度路径 | [CrossMuxSsd1677Driver.cpp](../../freeink-sdk/libs/display/FreeInkDisplay/src/driver/CrossMuxSsd1677Driver.cpp) 的本板配置 `absoluteGrayscale = false`，`displayGrayscaleBase()` 只能落回 `HALF_REFRESH` |
| 结论 | 4 级灰封面 = **整屏 HALF 波形（闪）+ 双平面灰度时序（约 226 ms）+ 灰度残影**，与"切歌不许闪"直接冲突，v1 否决 |
| v1 选择 | 二值 + 抖动，直接画进 BW 帧缓冲，因而可以走窗口 FAST，和 §8 的脏区策略完全一致 |
| 抖动算法 | 复用转换器内置的 Atkinson：照片在 200 px 下噪点细、可辨认，且不需要新代码；Bayer 有序抖动对照片更"网格化"，Floyd–Steinberg 需要额外误差行缓冲，均不选 |

观感预期（需实机确认）：封面是 1-bit 抖动图，远看能辨认构图与主色调，近看有噪点；对比度正常的专辑封面效果好于大面积渐变的照片。这是二值屏的物理上限，不是解码质量。

残影代价：200×200 = 40,000 像素，约占面板 10.4%；整块替换时旧封面的轮廓会在窗口内留下浅灰"双重曝光"，是比文字更明显的退化（见 15.7）。

### 15.6 布局

两种排布，按内容区纵横比选择（本板默认竖屏，插槽 `S` 为 8 的整数倍）：

| 排布 | 触发条件 | 槽位 `S` | 位置 | 文本区 |
|---|---|---|---|---|
| 竖排（封面在上） | `content.height > content.width` | `snap8(min(content.width, content.height × 45%))` | 水平居中于 `content.y` | 封面下方留 24 px，宽 `content.width` |
| 横排（封面在左） | 其余（内容区宽 ≥ 560、高 ≥ 240） | `snap8(min(content.height × 62%, content.width × 30%))` | `x = content.x`，`y = content.y + (content.height − S) / 2` | `x = content.x + S + 24` |

以本板竖屏内容区 440×644 为例：`S = snap8(min(440, 644×45%)) = 288`，封面上方留 10 px（`kCoverTopGap`，见 §15.7）后居中占上部，文本区 440×322；横屏 760×319 时 `S = snap8(min(319×62%, 760×30%)) = 192`，文本列 520 px。全部由 `getScreenWidth()/getScreenHeight()`、主题度量与 `measureCoverLayout()` 推导，禁止硬编码。

| 元素 | 有封面 | 无封面 |
|---|---|---|
| 封面 | 正方形 1:1 贴图 + 1 px 描边，居中裁剪 | 不占位 |
| 标题 | 文本区内居中，最多 2 行 | 整幅内容区，位置不变 |
| 副标题 / 状态 | 文本区内，紧随标题 | 不变 |
| 进度条 / 时间 | 文本区宽度，进度条在文本区 64% | 不变 |

退化规则：

- 竖排：`S < 120` 或封面下方不足 220 px → 隐藏封面，回到全宽居中布局。
- 横排：`content.width < 560`、`content.height < 240`，或 `S < 120`，或文本列不足 320 px → 隐藏封面，不挤压文字。
- 早期实现用"`content.width < 480` 就隐藏"的横排门槛，在 440 px 宽的竖屏内容区上恒为 0，实机因此从未显示封面（2026-11-15 修复，见 §15.10）。
- 无封面时**不保留**空白槽位，直接回到全宽布局；这比"永远给封面留位"更省空间，也避免空框长期占着残影区域。
- 列表页：**v1 不加小封面**。理由：行高只有一行文字，40×40 缩略图会强制每行多一次 BMP 打开与 1-bit 贴图，滚动时刷新时间与残影都会变差；曲目多时缓存条目数翻倍。若后续确有需求，用独立的 40×40 缓存与 PSRAM 列表快照，属于另一项改动。

### 15.7 刷新策略（与 §8 协同）

- 封面与文本列同属"切歌变化区"，切歌时改为**整帧 `HALF_REFRESH`**：封面、曲名、状态行、进度条、时间在同一个 framebuffer 里画好，只推一次面板。手工切歌与自动续播共用这一条路径（`index != lastIndex_` → `trackDirty_`）。
- 封面单独出现/消失会同时改变文本列宽度（全宽 ↔ 两列），因此和切歌一样按**整帧**处理：`fullDirty_` 会退化成一次整帧 FAST（不闪），下一次渲染即为无封面布局。
- 周期清洗沿用 §8.3：每 180 次**窗口 FAST** 更新一次、且当帧无用户操作；切歌的 HALF 会把计数清零，两者不重复。
- 残影累积与视觉退化：
  - 每次切歌整屏清洗，封面与文字都不再叠加“多重曝光”；两次切歌之间唯一累积的是进度带里的浅灰轮廓。
  - 残影只在窗口内，不会扩散到文本列或页头；页面仍可读。
  - 另有两个用户可感知的“顺带清理”：从播放页返回列表页（现有 `promoteNextRefresh(HALF)`）、长按 Back 重扫曲库，都会做一次整屏清理。
- **切歌整帧 HALF（已实现，2026-11-15 第二轮，对应 §8.4 方案 D）**：删除原先“槽位先刷白再推一次窗口”的方案 A（实机仍残留），改为切歌这一帧直接走 HALF 整屏波形，一次成型。日志 `Flush mode=HALF reason=TRACK`、`Wait complete: refresh (1717 ms)`；其余帧仍是 `Flush mode=WINDOW rect=20,602,440x41 coverHit=0`、`Wait complete: refresh (515 ms)`。
- **封面不参与周期刷新（已实现，2026-11-15）**：首次画好封面时把槽位像素从面板内存快照到 `coverTile_`（288² ≈ 10.4 KB，随 Activity 释放），之后每帧用 `copyBufferToRegion()` 回放，不再读 SD、不再解码、不改变像素。1 Hz 进度刷新只会推 `20,602,440x41` 的进度带；日志 `Flush ... coverHit=0` 证明窗口与封面矩形 (96,86,288×288) 不相交。
- 封面起始位置下移 10 px（`kCoverTopGap`，跟随内容区推导，不写死 800/480）：竖屏封面 `y = content.y + 10`，文本区高度同步减去 10，进度条与时间行仍按文本区相对位置计算，实测无重叠、无溢出；横屏在 `content.height − 10` 内居中。分界线仍在 `content.y`，封面顶边距其 10 px。
- 首次进入播放页（`fullDirty_`）本来就是整帧 FAST，封面随第一帧一起出现，不额外增加一次刷新。

### 15.8 资源

| 项目 | 预算 |
|---|---|
| Flash | 实现后整机余量 239,237 B（6,553,600 − 6,314,363）；按改动前记录的约 246 KB 折算，本次约 6–7 KB，落在预估区间上沿 |
| 新依赖 | 无（JPEGDEC / PNGdec / Bitmap / buildscratch 已在产物中） |
| 常驻 RAM | 播放页打开期间 10.4 KB（封面像素快照 `coverTile_`，288²/8，随 Activity 释放，见 §8.4）；其余 0；转换用的 17.9 KB 解码器 scratch 借用帧缓冲，转换结束即释放 |
| 转换期峰值 | 解码器 scratch 17.9 KB（借帧缓冲）+ MCU 行缓冲（≈ 源宽/8 × 8 行，几十 KB 上限，转换器自带 OOM 检查）+ BMP 行缓冲（约 100 B） |
| 绘制期 | `drawBitmapCropToFill` 的两条 2-bit 行工作缓冲 ≈ 2 × `S/4` ≈ 100 B |
| SD | 每张缓存 1-bit BMP ≈ `S²/8`：竖屏 288² 约 10.4 KB、横屏 192² 约 4.6 KB；上限 200 张 ≈ 1–2 MB |
| 音频影响 | 转换会占 SD 与 CPU；策略见 15.9，必须实测有无断音 |

### 15.9 异常与降级

| 场景 | 行为 |
|---|---|
| 无 APIC、无同目录图片 | 不画封面，回到全宽居中布局；每次切歌只扫一次标签头，不反复重扫 |
| APIC 为压缩/加密帧 | 跳过该帧，回退 3–4 号来源，`LOG_INF` 一次 |
| APIC 带 v2.4 逐帧去同步 | 同上（未实现该模式），回退 3–4 号来源 |
| MIME 不支持（GIF/WebP/BMP 内嵌） | 同上；同目录 GIF/BMP 也不支持，避免引入第三种转换器 |
| 图片损坏 / 截断 | 转换器返回 false → 删除半成品缓存 → 回全宽布局；不崩溃、不重试循环 |
| 超大文件（> 8 MB 帧 / > 8192 px） | 直接判失败，不分配缓冲；回全宽布局 |
| 标签结构损坏（长度越界、扩展到文件外） | 视为无标签，回退同目录图片 |
| 转换期间 OOM | 转换器已有堆检查并返回 false；保留原封面还是清空由首次绘制状态决定，不崩溃 |
| 播放中文件被删 / 拔卡 | 沿用 §14.7 的错误停止；封面绘制只在有会话时发生，缺路径直接回全宽布局 |
| 转换与刷新并发 | 封面解析/转换放在 `render()` 最开头、`clearScreen()` 之前，并用 `GfxRenderer::FrameBufferLoan` 包住；loan 结束后立即重建整帧。`loop()` 内不做转换，避免与正在排队的绘制交错 |
| 转换导致音频断音 | 首选"先转换封面再启动新曲"（切歌延迟增加，但不会断音）；若延迟不可接受，改为分片转换或仅缓存命中时立即切换 |

### 15.10 决策记录

| 决策 | 候选 | 选择与理由 |
|---|---|---|
| 内嵌与同名图片的优先级 | ① 同目录优先 ② 内嵌优先 | **②**，与主流播放器一致；同目录图片作为回退，避免"用户换图但文件里还有旧图"时看到旧图 |
| APIC 压缩帧 | ① 接 inflate 解压 ② 跳过并回退 | **②**。压缩 APIC 极少；流式 inflate 要处理长度守恒与错误传播，代码量与风险不成比例 |
| 图片是否落盘缓存 | ① 每次实时解码 ② 缓存目标尺寸 1-bit BMP | **②**。切歌是高频操作，BMP 命中只需一次贴图；缓存 5 KB/张，代价可控 |
| 缓存目录 | ① 与书籍缓存混用 ② 独立 `/.crosspoint/music/covers` | **②**。清理策略与生命周期不同（按 mtime 上限 200），混用会干扰书籍缓存 |
| 色深 | ① 4 级灰 ② 二值 + Atkinson 抖动 | **②**。本板灰度基帧只能 HALF（闪），与"切歌不闪"冲突；二值可直接走窗口 FAST |
| 绘制缩放 | ① 缓存原图、绘制时缩放 ② 缓存即槽位尺寸、1:1 贴图 | **②**。绘制期零缩放、零额外内存，且抖动点不会被最近邻打散 |
| 切歌刷新（2026-11-15 修订） | ① 封面单独窗口 + 文本单独窗口 ② 整个 content 一次窗口 ③ 整帧 HALF | **③**。方案 A（槽位刷白）+ 整块窗口在实机仍留残影；切歌这帧本来就要整块重画，用 HALF 一次清干净，不增加帧数 |
| 列表页缩略图 | ① v1 就做 ② 不做 | **②**。见 15.6：滚动刷新与缓存成本高，收益低 |
| 解码时机 | ① 切歌后异步分片 ② 切歌时同步、先图后声 | **②（先按此实现并实测）**。同步实现最简单；若引入可感知延迟或断音，再退回分片，接口不变 |
| APIC 如何交给转换器（实现调整） | ① 把文件 seek 到图片数据起点 ② 提取到临时文件再转换 | **②**。JPEGDEC/PNGdec 的 seek 回调按**绝对偏移**工作，无法直接解码标签内偏移；提取只有 5–500 KB 的顺序读写，且复制时正好完成去同步 |
| v2.4 逐帧去同步（实现调整） | ① 支持 ② 按不支持帧跳过 | **②**。需要为单个帧再搭一层过滤视图与独立长度记账，现实标签几乎不出现；跳过后仍会回退到同目录图片 |
| 缓存清理时机（实现调整） | ① 列表页每次 `onEnter()` ② 曲库重扫时 | **②**。`onEnter` 在每次从播放页返回时都会跑，目录枚举会给导航加可感知延迟；重扫是低频操作，且本来就是"曲库变化"的语义点 |
| 封面丢失检测（实现调整） | ① 信任缓存路径 ② 每帧打开并校验 BMP 头 | **②**。命中路径本来就要打开文件绘制，多一次头解析即可；文件被外部删除时当帧退回全宽布局，不留空洞 |
| 竖屏排布（实现调整，2026-11-15） | ① 只支持横屏两列 ② 竖屏封面在上 + 文本在下 | **②**。实机默认竖屏（内容区 440×644），旧门槛"宽 < 480 隐藏封面"在竖屏恒成立，导致封面从不显示；改为按纵横比选排布，竖屏槽位 288、文本区在其下方 |
| 封面周期重绘（2026-11-15） | ① 每帧重读缓存 BMP ② 首次画好后快照到内存回放 | **②**。1 Hz 路径不再碰 SD/解码，像素逐位稳定；代价是 10.4 KB 常驻（随 Activity 释放），见 §8.4 |
| 封面残影（2026-11-15 修订） | ① 只靠差分波形 ② 切歌先刷白再刷图 ③ 并入每 180 次整屏 HALF ④ 切歌直接整屏 HALF | **④**。② 实机仍残留且多推一帧白底；③ 要等最多 3 分钟；① 已否证。④ 复用切歌本来就要重画的那一帧，见 §8.4 |
| 封面顶边间距（2026-11-15） | ① 贴紧分界线 ② 固定 10 px ③ 按内容区比例推导 | **②**，常量 `kCoverTopGap`，从内容区几何扣减（文本区高度、横屏居中范围同步），不引用 800/480 |
| JPEG 封面解码失败（2026-11-15） | ① 换解码器 ② 修补 vendored JPEGDEC 的色度块消费 | **②**。只改灰度路径的块计数，复用整套转换器与抖动；换解码器会新增 Flash 与维护面，见 §15.3 |

### 15.11 确认与验收

四个点已由用户确认（2026-11-15）：左侧 200×200 二值抖动封面 + 右侧文本列、无封面不留空位；接受切歌不闪但残影累积到下次整屏清理（2026-11-15 第二轮改为切歌整屏 HALF，见 §8.4）；列表页 v1 不加小封面；WAV 只支持同目录图片。实现按此落地。

验收点：

1. 带 APIC 的 MP3（v2.3、v2.4 各一）能显示封面；同名 `.jpg`/`.png` 在没有 APIC 时生效；同目录 `cover.jpg` 生效。
2. 压缩/加密 APIC、损坏图片、非图片数据、8 MB 以上数据均安全回退到全宽布局，无 panic、无重试风暴。
3. 切歌（含自动续播）全程无全屏 HALF；只有窗口 FAST；页头与底部提示不变。
4. 封面 1:1 贴图，无二次缩放噪点；封面与文本不重叠、间距 ≥ 6 px。
5. 窄屏/竖屏回退到居中布局，文字不被挤压。
6. 缓存命中时切歌无可感知延迟；未命中时实测切歌延迟与是否有音频断音，记录数值。
7. 连续切 10 首后残影范围仍限制在封面槽位内，整屏清理后恢复干净。

### 15.12 串口自动化闭环（仅 dev 构建）

`platformio.ini` 的 `[mode_dev]` 定义 `-DCROSSMUX_DEBUG_SERIAL_COMMANDS`；stable/nightly 不含该宏，命令与日志都不会编译进发布固件，无 USB 场景行为不变。命令沿用 `main.cpp` 既有的 `CMD:` 通道（每行一条，`\n` 结尾，与 `CMD:SCREENSHOT` 同权限边界）：

| 命令 | 作用 | 关键日志 |
|---|---|---|
| `CMD:MUSIC` | 进入音乐列表页 | `CMD MUSIC` |
| `CMD:MUSICPLAY` | 载入 `/music` 并播放第 1 首，直接替换到播放页 | `CMD MUSICPLAY tracks=N started=1 player=1` |
| `CMD:MUSICNEXT` | 下一首 | `CMD MUSICNEXT track=N ok=1` |
| `CMD:MCOV` | 让下一次 `render()` 重跑封面管线（不离开页面） | `CMD MCOV (cover recheck)` |
| `CMD:SCREENSHOT` | 输出 `SCREENSHOT_START:<bytes>` + 裸帧缓冲 + `SCREENSHOT_END` | 见下 |

截图取 `renderer.getFrameBuffer()/getBufferSize()`（**不是** `display.getFrameBuffer()`：显示交换后它可能指向另一平面，导出的会是上一页）。本板帧缓冲是面板内存布局 800×480、每行 100 字节，竖屏屏幕坐标映射 `phyX = screen.y`、`phyY = 479 − screen.x`。

宿主机脚本（仓库外，`/tmp`）：以 `os.open(O_RDWR|O_NOCTTY|O_NONBLOCK)` + termios raw 打开串口，**不触碰 DTR/RTS**，因此不会复位设备；同一 fd 收发。一次完整闭环：等启动 → `MUSICPLAY` → 等界面稳定 → `SCREENSHOT` → 解析帧缓冲校验封面矩形。

可自动化判定：`Layout/Flush mode/reason` 的布局、刷新型号与脏区（`TRACK`/`CLEAN`/`FULL`/`NODIRTY`/`WINDOW`）、`MCOV` 的 APIC 长度/转换结果/缓存命中、`Cover draw/restore` 与矩形、帧缓冲里封面矩形的像素分布与跨曲差异。
只能人眼判定：二值抖动的实际观感、切歌整屏 HALF 的闪动幅度是否可接受、残影在真实玻璃上的可见程度。

## 16. 歌词显示（v3）

### 16.1 文件约定

| 项 | 规则 |
|---|---|
| 命名 | 与音频文件同目录、同基名的 `.lrc`；`/music/a.mp3` → `/music/a.lrc`（去掉最后一个扩展名，与封面 sidecar 推导方式一致） |
| 编码 | UTF-8；首行 UTF-8 BOM 自动跳过。不识别 GBK/GB18030，中文歌词须存为 UTF-8 |
| 格式 | 标准 LRC：`[mm:ss]`、`[mm:ss.x]`、`[mm:ss.xx]`、`[mm:ss.xxx]`；一行多时间戳；`[offset:±ms]`；`[ti:]` / `[ar:]` / `[al:]` / `[by:]` 元数据（忽略）；增强型 `<mm:ss.xx>` 词级时间戳在解析时剥离，只保留整行时间 |
| 缺失/损坏 | 无 `.lrc`、空表、只有元数据、超上限：一律视为无歌词，播放页保持信息布局，不提示、不报错 |

### 16.2 内存与上限

`LrcLyrics` 用固定上限把最坏情况钉死，解析失败或超限都只是截断，不会耗尽共享堆：

- 时间表最多 `kMaxLines = 256` 项，每项 8 字节（`timeMs` + 文本偏移/长度），约 2 KB；`reserve(64)` 起，超出按需增长后不再收缩。
- 文本 blob 固定 4 KB（`kTextCapacity`），行文本按出现顺序拼接、`offset/length` 指向其中；单行最多 `kLineCapacity = 512` 字节，超长行按前缀截断。
- 表与文本都用 `makeUniqueNoThrow` 分配，OOM 时 `LOG_ERR` 并退回无歌词。
- `clear()` 用 `std::vector<Entry>().swap()` + `unique_ptr::reset()`，切歌、退出页面时立即归还，不保留上一首的歌词。

### 16.3 布局

```
┌────────────────────────────────────────────────┐
│ 音乐                                [电量]     │  drawHeader
├────────────────────────────────────────────────┤
│             夜空中最亮的星                      │  轨道名（SMALL，1 行，居中，省略号）
│                                                │
│        我祈祷拥有一颗透明的心灵                 │  上一行
│  ██████ 给我再去相信的勇气 █████████████████    │  当前行：黑底白字，垂直居中
│        越过谎言去拥抱你                         │  下一行
│                                                │
│  ███████████████████░░░░░░░░░░░░░░░░░░░░░░░░   │  进度条（位置与信息布局完全一致）
│  02:13                                   04:31 │  已播 / 总时长
├────────────────────────────────────────────────┤
│   返回        播放|暂停    音量-     音量+      │  GUI.drawButtonHints
└────────────────────────────────────────────────┘
```

- 竖屏有封面时，轨道名垂直居中于「封面下沿」与「第一行歌词顶部」之间，歌词窗口改为从 `barTop - 8` 向上倒推行数（最多 7 行），因此行块会把空间填到进度条上方，而不是紧贴曲名堆叠；无封面或横屏两列时，轨道名仍按信息视图的紧凑锚点（内容区 6% 再上移 5 px）放在行块之上。`measureLyricLayout()` 是绘制与脏区的唯一几何来源。
- 行高取 `NOTOSANS_12_FONT_ID` 的 `getLineHeight()`；可见行数 `clamp(可用高度 / 行高, 1, 7)`，偶数行减一，保证当前行上下对称。
- 当前行固定在窗口垂直中心，窗口随播放位置滚动；首行之前（`indexAt()` 返回 -1）与尾行之后都把窗口夹在有效范围内，不画空行。
- 当前行 `fillRect` 整行宽反白后再用白字绘制，文本左右各留 8 px；超宽行用 `truncatedText()` 以 U+2026 截断，UTF-8 安全。
- 字体固定用 `NOTOSANS_12_FONT_ID`：12 pt CJK 覆盖约 3500 常用字，比 14/16/18 pt 的 747 字子集可靠；缺字仍由渲染器的字体回退链兜底。

### 16.4 交互与刷新

| 事件 | 行为 |
|---|---|
| 进入播放页 / 切歌（含自动续播） | 释放旧歌词，置 `lyricsPending_`；下一次 `render()` 开头按新曲目路径重新加载 |
| 有歌词 | `preferLyrics_` 默认 true，直接显示歌词视图 |
| 无歌词 | 保持 §5 的信息布局；`preferLyrics_` 的偏好保留到下一次进页（`onEnter()` 重置为 true） |
| 长按 Confirm | `lyrics_.empty()` 时无动作；否则翻转 `preferLyrics_`，`fullDirty_` 触发整帧 FAST 重绘，双视图切换不闪 |
| 播放中 | 复用 1 Hz 进度 tick：`indexAt(positionMs())` 与 `lastLyricIndex_` 不同才置 `lyricsDirty_` 并请求刷新 |
| 暂停 | 不刷新歌词（位置不变）；恢复播放后下一次 tick 自动对齐 |

刷新沿用 §8 的窗口模型：

- `lyricsDirty_` 只把歌词窗口（轨道名下沿到 `barTop - 8`）并入脏区；与进度带同帧时取并集，仍在 `textArea` 内，不会碰封面或页头。
- 歌词切行是窗口 FAST，不是 HALF；只有切歌/周期清洗才整屏 HALF。
- 每行重绘都会调用 `truncatedText()`（最多 7 次小 `std::string`），与既有标题/状态文本同一量级，不引入新的常驻分配。

### 16.5 验收点

1. 同目录放 UTF-8 `.lrc`，进入播放页即显示歌词；删掉 `.lrc` 后重新进入，回到信息布局且无报错。
2. 一行多时间戳、`[offset:]`、增强型 `<...>` 标签都能正确解析；`[ti:]/[ar:]` 不出现在窗口里。
3. 播放时当前行按时间滚动并反白，切换行只刷歌词窗口；进度条区不出现整屏闪黑。
4. 长按 Confirm 在歌词/信息视图间切换，进度条与时间行位置不动；无歌词时该手势无副作用。
5. 切歌后 1 秒内显示新歌词（或无歌词时的信息布局），不残留上一首的行。
6. 超长行、超多行、4 KB 以上文本：显示截断后的内容，不 panic，`clear()` 后堆水位回到加载前。


