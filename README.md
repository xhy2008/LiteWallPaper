# LiteWallPaper

极致轻量化的 Windows 动态壁纸程序。内存占用 <50MB，CPU 时间 <1%，全程硬件解码、零外部运行时依赖。

- **壁纸制作器** (`wallpaper_maker`)：通过命令行调用 ffmpeg.exe 将任意视频转码为可直接送入 D3D11 VideoDecoder 的 H.264 Annex B 流，封装为 `.lwp` 容器文件。
- **壁纸播放器** (`wallpaper_player`)：使用 D3D11/DXVA 硬件解码 H.264，在桌面图标后方渲染壁纸；带托盘图标右键菜单；前台窗口最大化/全屏时自动暂停以节省资源。

---

## 目录结构

```
LiteWallPaper/
├── CMakeLists.txt                  # 顶层 CMake（静态 CRT、Release 优化）
├── common/
│   ├── bitstream_format.h          # .lwp 容器格式定义（LwpHeader + AuEntry）
│   └── CMakeLists.txt
├── wallpaper_maker/                # 控制台工具，生成 .lwp 文件
│   ├── maker_main.cpp              # 入口、参数解析
│   ├── ffmpeg_runner.{h,cpp}       # 调用 ffmpeg.exe 转码
│   ├── h264_processor.{h,cpp}      # 扫描 NAL、构建 AU 索引表
│   ├── sps_parser.{h,cpp}          # SPS 解析（profile/level/分辨率等）
│   └── CMakeLists.txt
└── wallpaper_player/               # GUI 程序，播放 .lwp 壁纸
    ├── player_main.cpp             # wWinMain 入口
    ├── app.{h,cpp}                 # 顶层调度：窗口/解码/渲染/托盘/配置
    ├── wallpaper_window.{h,cpp}    # WorkerW 桌面层窗口
    ├── d3d11_decoder.{h,cpp}       # DXVA H.264 硬件解码
    ├── d3d11_renderer.{h,cpp}      # NV12→RGB 渲染管线
    ├── nal_parser.{h,cpp}          # NAL 分割、SPS/PPS/slice header 解析
    ├── tray_icon.{h,cpp}           # 托盘图标 + 右键菜单
    ├── config_manager.{h,cpp}      # INI 配置持久化
    ├── foreground_monitor.{h,cpp}  # 前台窗口最大化/全屏检测
    ├── resource.rc                 # 嵌入 manifest + 图标
    ├── app.manifest                # Common Controls v6、PerMonitorV2 DPI
    ├── app.svg / app.ico           # 应用图标源文件
    └── CMakeLists.txt
```

---

## 一、构建

### 依赖

- **CMake ≥ 3.20**
- **Visual Studio 2022**（MSVC 19.x，已验证 v19.51）或独立 Build Tools
- **Windows 10 SDK**（10.0.26100 或更高）
- **ffmpeg.exe**（仅制作器运行时需要，构建时不需要；需加入 PATH 或设置 `FFMPEG_PATH` 环境变量）
- 播放器**零外部依赖**：不链接 FFmpeg，不链接任何媒体库，仅使用 Windows SDK 自带的 DirectX 11 / DXVA

### 构建步骤

```bat
cd E:\LiteWallPaper
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：
- `build\wallpaper_maker\Release\wallpaper_maker.exe` — 控制台工具
- `build\wallpaper_player\Release\LiteWallPaper.exe` — 播放器（单文件，静态链接 CRT）

### 构建选项说明

顶层 [CMakeLists.txt](CMakeLists.txt) 启用了以下 Release 优化：

| 选项 | 作用 |
|---|---|
| `MultiThreaded` 静态 CRT | 无需分发 vcredist，单 exe 即可运行 |
| `/O2 /Gy /Gw` | 体积与速度优化 |
| `/OPT:REF /OPT:ICF` | 链接期死代码消除与 COMDAT 折叠 |
| `/utf-8` | 源码 UTF-8 编码 |
| `/permissive-` | 严格 C++ 标准 |

---

## 二、壁纸制作器

### 用法

```bat
wallpaper_maker <input> <output.lwp> [options]
```

**选项：**

| 参数 | 默认值 | 说明 |
|---|---|---|
| `-w <width>` | 0（保留源） | 目标宽度（偶数） |
| `-h <height>` | 0（保留源） | 目标高度（偶数） |
| `-f <fps>` | 0（保留源） | 目标帧率 |
| `--crf <n>` | 23 | x264 CRF 质量值 |
| `--preset <p>` | veryfast | x264 预设 |

**示例：**

```bat
:: 基本转换
wallpaper_maker input.mp4 wallpaper.lwp

:: 调整到 1920x1080、24fps、高质量
wallpaper_maker input.mp4 wallpaper.lwp -w 1920 -h 1080 -f 24 --crf 20 --preset slow
```

### ffmpeg.exe 定位顺序

[ffmpeg_runner.cpp](wallpaper_maker/ffmpeg_runner.cpp) 中 `find_ffmpeg_executable()` 按以下顺序查找：

1. `FFMPEG_PATH` 环境变量（可为完整路径或目录）
2. 系统 `PATH`
3. 常见安装位置：`C:\ffmpeg\bin\`、`C:\Program Files\ffmpeg\bin\`、`C:\Program Files (x86)\ffmpeg\bin\`

### 编码参数（保证 DXVA 兼容性）

[ffmpeg_runner.cpp:77-84](wallpaper_maker/ffmpeg_runner.cpp#L77-L84) 调用 ffmpeg 时固定使用以下参数：

```
-c:v libx264
-profile:v baseline              :: Baseline profile（CAVLC，无 transform_8x8）
-x264-params bframes=0:repeat_headers=1
                                 :: 无 B 帧（解码序 == 显示序）
                                 :: 每个 IDR 前重复 SPS/PPS
-pix_fmt yuv420p                 :: 8-bit 4:2:0
-f h264                          :: 原始 Annex B 输出
```

**为什么用 Baseline profile**：最大化 DXVA 兼容性，避免 CABAC、transform_8x8 等扩展特性带来的驱动兼容性问题。

**为什么无 B 帧**：使解码序等于显示序，播放器无需重排序缓冲区，DXVA PicParams 管理极简。

### 处理流程

1. **ffmpeg.exe 转码**：输入视频 → 临时 `.h264` 文件（Annex B 流）
2. **扫描 NAL**（[h264_processor.cpp](wallpaper_maker/h264_processor.cpp)）：
   - 查找 SPS（NAL type 7）和 PPS（NAL type 8）
   - 解析 SPS 获取 profile/level/位深/编码尺寸
   - 按 VCL NAL 边界切分为访问单元（AU，一帧一个）
3. **写 .lwp 文件**：64B 头 + AU 索引表 + 原始 Annex B 流

### `_popen` 引号陷阱

[ffmpeg_runner.cpp:110-114](wallpaper_maker/ffmpeg_runner.cpp#L110-L114) — `_popen` 通过 `cmd.exe /c <cmd>` 执行命令，当 `<cmd>` 含多对引号时，cmd.exe 会剥掉最外层引号。解决方法是将整个命令再包一层引号：

```cpp
cmd = "\"" + cmd + "\"";
```

---

## 三、.lwp 容器格式

详见 [common/bitstream_format.h](common/bitstream_format.h)。所有整数小端序。

### 文件布局

```
┌─────────────────────────────────┐ offset 0
│ LwpHeader (64 字节)              │
├─────────────────────────────────┤ offset = header.au_table_offset
│ AU 索引表 (frame_count × 8 字节) │
├─────────────────────────────────┤ offset = au_table_offset + frame_count*8
│ 原始 H.264 Annex B 流             │  ← payload
│  (00 00 00 01 SPS ... 00 00 00 01│
│   PPS ... 00 00 00 01 IDR ...)   │
└─────────────────────────────────┘
```

### LwpHeader 字段（64 字节）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `magic` | `0x3150574C`（`'L','W','P','1'` 小端） |
| 4 | 2 | `header_size` | 64 |
| 6 | 2 | `width` | 编码宽度（像素） |
| 8 | 2 | `height` | 编码高度（像素） |
| 10 | 2 | `fps_num` | 帧率分子（如 30） |
| 12 | 2 | `fps_den` | 帧率分母（如 1） |
| 14 | 4 | `frame_count` | 总帧数 = AU 数 |
| 18 | 4 | `sps_offset` | SPS NAL 在 payload 中的偏移 |
| 22 | 2 | `sps_size` | SPS NAL 字节数 |
| 24 | 4 | `pps_offset` | PPS NAL 在 payload 中的偏移 |
| 28 | 2 | `pps_size` | PPS NAL 字节数 |
| 30 | 4 | `payload_size` | Annex B 流字节数 |
| 34 | 2 | `profile_idc` | 从 SPS 拷贝 |
| 36 | 1 | `level_idc` | 从 SPS 拷贝 |
| 37 | 1 | `bit_depth_luma` | 8 |
| 38 | 1 | `bit_depth_chroma` | 8 |
| 39 | 1 | `reserved1` | |
| 40 | 4 | `au_table_offset` | AU 索引表的文件绝对偏移 |
| 44 | 2 | `au_entry_size` | 8（= `sizeof(AuEntry)`） |
| 46 | 2 | `reserved2` | |
| 48 | 16 | `sha128_of_payload` | 可选，0 = 未计算 |

### AuEntry（8 字节）

```cpp
struct AuEntry {
    uint32_t offset;  // AU 第一个 start code 相对 payload 起始的偏移
    uint32_t size;    // AU 字节数（到下一个 AU 的第一个 start code，或 payload 末尾）
};
```

**设计要点**：制作器预切分 AU 索引表，播放器无需运行时 NAL 扫描，只需按索引表读取单帧即可。

---

## 四、壁纸播放器

### 4.1 启动流程

[player_main.cpp](wallpaper_player/player_main.cpp) → [app.cpp](wallpaper_player/app.cpp) `App::init()`：

1. `SetProcessWorkingSetSize(-1, -1)` 主动压缩内存占用
2. `ConfigManager::load()` 从 exe 同目录读取 `LiteWallPaper.ini`
3. `WallpaperWindow::create()` 创建桌面层窗口（见 4.2）
4. `TrayIcon::create()` 创建托盘图标（使用独立消息窗口，见 4.6）
5. `ForegroundMonitor` 设置回调
6. 若 INI 中有已保存的壁纸路径，立即 `load_wallpaper()`
7. `SetTimer` 16ms 帧定时器

### 4.2 WorkerW 桌面层窗口

[wallpaper_window.cpp](wallpaper_player/wallpaper_window.cpp)

播放器窗口必须位于桌面图标层**后方**、静态壁纸色**前方**。实现方式：

1. `FindWindowW(L"Progman", nullptr)` 找到 Program Manager
2. 检查 Progman 是否已含 `SHELLDLL_DefView` 子窗口（桌面图标列表视图）
3. 若是，`SendMessageTimeoutW(progman, 0x052C, ...)` 让 Explorer 把图标层移到 WorkerW，并生成壁纸层 WorkerW
4. `EnumWindows` / `FindWindowEx` 枚举所有 WorkerW：
   - **图标 WorkerW** = 含 `SHELLDLL_DefView` 子窗口
   - **壁纸 WorkerW** = 无 `SHELLDLL_DefView` 子窗口
5. 优先选择可见且尺寸 ≥640×480 的壁纸 WorkerW（过滤掉 170×47 的图标标签 tooltip 窗口）
6. 找不到合格 WorkerW 时，回退到 Progman，通过 `SetWindowPos(hwnd_, shell, ...)` 定位到 `SHELLDLL_DefView` 后方
7. `SetParent(hwnd_, worker_w_)` 嵌入桌面层
8. `fit_to_desktop()` 覆盖整个虚拟屏幕

**关键坑**：
- `WS_EX_LAYERED` 不能用——分层窗口的 swap chain 输出会被 DWM 忽略
- 右键菜单的 owner 必须是独立消息窗口，不能是壁纸窗口——否则 `SetForegroundWindow` 会把壁纸窗口提到图标层上方

### 4.3 D3D11/DXVA 硬件解码

[d3d11_decoder.cpp](wallpaper_player/d3d11_decoder.cpp)

#### 解码管线

```
.lwp 文件 → 按帧 ReadFile → AU 字节流（含 00 00 00 01 起始码）
          → split_nals + parse_slice_header
          → DecoderBeginFrame(output_view[cur])
          → GetDecoderBuffer × 4: PP / IQ / BS / SC
          → SubmitDecoderBuffers
          → DecoderEndFrame
          → CopySubresourceRegion → shared_output_ (NV12)
```

#### 关键 D3D11 对象

| 对象 | 说明 |
|---|---|
| `ID3D11VideoDevice` | 枚举解码器 profile、创建 decoder |
| `ID3D11VideoContext` | `DecoderBeginFrame` / `GetDecoderBuffer` / `SubmitDecoderBuffers` |
| `ID3D11VideoDecoder` | 解码器实例 |
| `decode_textures_` | NV12 数组纹理（ArraySize = surface_count），每个 slice 一个 surface |
| `decoder_output_views_` | **每个 slice 一个** OutputView，ArraySlice=i |
| `shared_output_` | 单个 NV12 纹理，每帧从数组中 CopySubresourceRegion 出来供渲染器读取 |

#### Profile 选择

`D3D11_DECODER_PROFILE_H264_VLD_NOFGT`（硬件 VLD 解码，无胶粒降噪）。`ConfigBitstreamRaw = 2`（驱动接收带 Annex B 起始码的原始字节流，自行解析 slice header）。

#### Surface 池

`surface_count = max(kMaxSurfaces, sps.max_num_ref_frames + 2)`。无 B 帧 + 1 参考帧，最小 2 个 surface。每帧 `current_surface_ = (current_surface_ + 1) % surface_count` 轮转。

#### 核心 Bug 修复：per-slice OutputView

**问题**：如果所有帧都用同一个 `ArraySlice=0` 的 OutputView，解码器会始终写入 slice 0。`CurrPic.Index7Bits` 只用于驱动参考帧管理，不决定输出 slice。结果：IDR 帧（cur=0）碰巧正确，P 帧（cur=1,2,3）从空 slice 读取 → NV12(0,0,0) → 经 BT.601 转换为 RGB(0,0.53,0) ≈ 绿色 → 闪烁绿色画面。

**修复**：为每个 surface 创建独立的 OutputView（`ArraySlice=i`），`DecoderBeginFrame` 使用 `decoder_output_views_[cur]`。详见 [d3d11_decoder.h:77-83](wallpaper_player/d3d11_decoder.h#L77-L83)。

#### DXVA 缓冲区

每帧提交 4 个缓冲区：

| 类型 | 内容 |
|---|---|
| `PICTURE_PARAMETERS` | `DXVA_PicParams_H264`（CurrPic、RefFrameList、FrameNumList、FieldOrderCntList 等） |
| `INVERSE_QUANTIZATION_MATRIX` | 全 16（flat scaling，x264 默认无自定义矩阵） |
| `BITSTREAM` | 整个 AU（含 start code，ConfigBitstreamRaw=2 要求） |
| `SLICE_CONTROL` | `DXVA_Slice_H264_Short`（BSNALunitDataLocation + SliceBytesInBuffer） |

**注意**：`DataSize` 必须是**实际数据大小**，而非 `GetDecoderBuffer` 返回的缓冲区容量。

#### 参考帧管理（简化）

无 B 帧、单参考帧，PicParams 填充逻辑（[d3d11_decoder.cpp](wallpaper_player/d3d11_decoder.cpp) `fill_pic_params`）：

```
ref = (nal_type == NAL_IDR) ? 0xff : ref_surface_;
if (ref == 0xff && !first_frame_) ref = 0;
RefFrameList[0].Index7Bits = ref
```

### 4.4 NV12→RGB 渲染

[d3d11_renderer.cpp](wallpaper_player/d3d11_renderer.cpp)

#### 渲染管线

- **顶点着色器**：全屏三角形，从 `SV_VertexID` 生成位置和 UV，无需顶点缓冲
- **像素着色器**：采样 Y 平面（R8 SRV）+ UV 平面（R8G8 SRV），BT.601 limited-range 转 RGB
- **Swap chain**：`DXGI_SWAP_EFFECT_FLIP_DISCARD`，2 个缓冲区，vsync on（`Present(1, 0)`）

#### SRV 缓存

Y 和 UV 的 SRV 仅在输入纹理变化时（壁纸切换）重建，避免每帧创建 2 个 SRV。用 `cached_nv12_` 原始指针做身份检查。详见 [d3d11_renderer.h:55-59](wallpaper_player/d3d11_renderer.h#L55-L59)。

#### 像素着色器（BT.601 limited-range）

```hlsl
y = (y - 16.0/255.0) * (255.0/219.0);   // limited-range → full-range
u = u * (255.0/224.0);
v = v * (255.0/224.0);
r = y            + 1.402 * v;
g = y - 0.344*u  - 0.714 * v;
b = y + 1.772*u;
```

### 4.5 流式按需读取（极低内存）

[app.cpp](wallpaper_player/app.cpp) `decode_next_frame()`：

**不**把整个 payload 读入内存。保留 `file_handle_`，每帧用 `SetFilePointerEx` + `ReadFile` 读取单个 AU 到复用的 `au_buffer_`。

| | 之前（全量加载） | 现在（流式） |
|---|---|---|
| 33MB 视频 | ~52MB | ~1-2MB |
| 内存组成 | 整个 payload + 索引表 + D3D 资源 | 索引表 ~3KB + 单帧缓冲 ~100KB + SPS/PPS + D3D 纹理 |

`au_buffer_` 只增长到最大单帧大小（通常 50-200KB），循环播放时系统文件缓存自然命中。

### 4.6 托盘图标与菜单

[tray_icon.cpp](wallpaper_player/tray_icon.cpp)

- **独立消息窗口**：`HWND_MESSAGE` 父窗口的消息专用窗口，作为托盘通知和弹出菜单的 owner。`SetForegroundWindow` 作用于这个隐藏窗口，**不触碰壁纸窗口的 z-order**——这是避免"右键后壁纸跳到图标上方"的关键。
- **图标**：从 exe 资源加载（`LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, ...)`），资源 ID 1 定义在 [resource.rc](wallpaper_player/resource.rc)。
- **菜单项**：Select wallpaper... / Pause-Resume / Fill-Letterbox / Pause on fullscreen app / Quit

### 4.7 前台窗口暂停

[foreground_monitor.cpp](wallpaper_player/foreground_monitor.cpp)

1Hz 轮询（在 16ms 帧定时器中累积到 1s 触发）：

- `GetForegroundWindow()` 获取前台窗口
- `IsZoomed(fg)` 检测最大化
- `MonitorFromWindow` + 窗口矩形对比检测全屏
- 排除 `Progman` / `WorkerW` / 自身窗口
- 状态变化时回调 `App::set_blocking()`，暂停/恢复解码

**为什么用轮询而非 `SetWinEventHook`**：1Hz 轮询约 0% CPU，足够快；事件钩子对每次窗口移动都触发，开销更大。

### 4.8 配置持久化

[config_manager.cpp](wallpaper_player/config_manager.cpp)

INI 文件位于 exe 同目录 `LiteWallPaper.ini`，用 Win32 `GetPrivateProfileStringW` / `WritePrivateProfileStringW` 读写，无第三方依赖。

```ini
[main]
wallpaper=E:\path\to\wallpaper.lwp
pause_on_fs=1
fill=1
muted=1
target_fps=0
```

### 4.9 壁纸切换流程

`App::load_wallpaper(path)`：

1. `close_wallpaper()` — 释放 decoder、关闭文件句柄、swap 释放所有缓冲区
2. 读取 64B 头 + AU 索引表 + SPS/PPS（各几十字节）
3. 保留 `file_handle_` 供流式读取
4. 仅在视频尺寸变化或首次加载时重建 renderer；否则复用 device/swapchain
5. `decoder_.initialize()` 创建新的解码器对象
6. 持久化路径到 INI
7. 立即 `decode_next_frame()` 渲染首帧 + `InvalidateRect` 触发重绘

**关键**：文件对话框和 MessageBox 的 owner 设为 `nullptr`，避免激活壁纸窗口导致 z-order 错乱。

---

## 五、资源占用

### 实测数据（33MB / 320×240 / 30fps 测试视频）

| 指标 | 数值 | 目标 |
|---|---|---|
| 内存（工作集） | ~20MB | <50MB |
| CPU 时间 | <1% | <1% |
| exe 大小 | ~200KB | 极小 |

### 极致轻量化的设计决策

1. **静态 CRT**：无 vcredist 依赖，单 exe 分发
2. **零外部依赖**：播放器不链接任何媒体库
3. **硬件解码**：DXVA 全程 GPU，CPU 仅做 NAL 切分和 PicParams 填充
4. **流式读取**：按帧读文件，不缓存整个 payload
5. **前台暂停**：全屏应用时停止解码和渲染
6. **SRV 缓存**：每帧不创建新的 GDI 对象
7. **单进程单窗口**：D3D11 flip-discard swap chain，DWM 自动合成
8. **1Hz 前台轮询**：而非事件钩子

---

## 六、使用指南

### 首次使用

1. 构建（见第二节）
2. 准备一个视频文件（mp4、mkv、avi 等任意 ffmpeg 支持的格式）
3. 转换为 .lwp：

   ```bat
   cd build\wallpaper_maker\Release
   wallpaper_maker E:\videos\myvideo.mp4 E:\wallpapers\mywallpaper.lwp
   ```

4. 运行播放器：

   ```bat
   build\wallpaper_player\Release\LiteWallPaper.exe
   ```

5. 右键托盘图标 → "Select wallpaper..." → 选择 `.lwp` 文件

### 托盘菜单说明

| 菜单项 | 功能 |
|---|---|
| **Select wallpaper...** | 打开文件对话框选择 `.lwp` 文件 |
| **Pause / Resume** | 手动暂停/恢复播放 |
| **Fill** | 拉伸填充（忽略宽高比） |
| **Letterbox** | 保持宽高比，上下/左右加黑边 |
| **Pause on fullscreen app** | 前台应用全屏/最大化时自动暂停 |
| **Quit** | 退出程序 |

### 配置文件

程序关闭后自动保存设置到 exe 同目录 `LiteWallPaper.ini`，下次启动自动恢复。

---

## 七、技术要点与踩坑记录

### 7.1 DXVA per-slice OutputView

**症状**：IDR 帧正常，P 帧输出全零 → NV12(0,0,0) → 绿色画面闪烁。

**根因**：`D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC::Texture2D.ArraySlice` 留空为 0，解码器始终写入 slice 0。`CurrPic.Index7Bits` 只影响驱动参考帧管理，不决定输出 slice。

**修复**：为每个 surface 创建独立的 OutputView（ArraySlice=i）。见 [d3d11_decoder.cpp](wallpaper_player/d3d11_decoder.cpp) `configure_decoder_objects()`。

### 7.2 WorkerW 定位

**症状**：壁纸窗口浮在桌面图标上方，或全屏覆盖任务栏。

**根因**：
- `EnumWindows` 可能跳过不可见窗口，漏掉真正的壁纸 WorkerW
- 170×47 的图标标签 tooltip WorkerW 被误选
- 0x052C 在部分系统上不会创建 Progman 下方的 WorkerW

**修复**：
- 用 `FindWindowEx` 枚举所有 WorkerW（包括不可见的）
- 过滤尺寸 <640×480 的候选
- 优先选可见且尺寸合格的
- 找不到时回退到 Progman + `SetWindowPos` 定位到 `SHELLDLL_DefView` 后方

### 7.3 右键菜单导致壁纸上浮

**症状**：右键托盘图标后，壁纸窗口跳到桌面图标上方。

**根因**：弹出菜单需要 `SetForegroundWindow` 才能正常关闭。若以壁纸窗口为 owner，会激活并提升其 z-order。

**修复**：托盘使用独立的 `HWND_MESSAGE` 消息窗口作为 owner。

### 7.4 文件对话框导致壁纸上浮

**症状**：通过菜单选择壁纸后，壁纸窗口跳到图标上方。

**根因**：`GetOpenFileNameW` 的 `hwndOwner` 设为壁纸窗口时，对话框会激活其 owner。

**修复**：`hwndOwner = nullptr`，对话框独立显示。`MessageBoxW` 同理。

### 7.5 PPS 解析 profile 感知

**问题**：High profile 的 PPS 扩展（transform_8x8、scaling matrix、second chroma qp offset）仅在 profile ≥ High 时存在。对 Baseline 错误设置 `t8x8=1` 会导致驱动拒绝 PicParams。

**修复**：[nal_parser.cpp](wallpaper_player/nal_parser.cpp) `parse_pps()` 接受 `profile_idc` 参数，仅在 High profile 及以上解析扩展字段。

### 7.6 _popen 引号剥离

**问题**：`_popen` 通过 `cmd.exe /c <cmd>` 执行，当 `<cmd>` 含多对引号时，cmd.exe 剥掉最外层引号，破坏 exe 路径和文件名。

**修复**：将整个命令再包一层引号：`cmd = "\"" + cmd + "\""`。见 [ffmpeg_runner.cpp:110-114](wallpaper_maker/ffmpeg_runner.cpp#L110-L114)。

### 7.7 内存泄漏

**问题**：每次切换壁纸内存增加，且体积越大的壁纸占用越多。

**根因**：
1. `std::vector::clear()` 只设 size=0，capacity 保留 → swap 到更大壁纸时容量只增不减
2. 每帧创建 2 个 SRV（Y + UV），60fps 下每秒 120 个 GDI 对象
3. 诊断 staging texture 在每次切换后重新触发

**修复**：
- `close_wallpaper()` 中用 `std::vector<T>().swap(v)` 立即释放内存
- 渲染器缓存 SRV，仅在输入纹理变化时重建
- 流式按需读取，不缓存整个 payload

---

## 八、许可证

本项目为个人工具项目，未附加开源许可证。如需使用请联系作者。

---

## 九、图标资源

应用图标源文件为 [app.svg](wallpaper_player/app.svg)：两个部分重叠的空心矩形（浅蓝 `#6BAED6` + 深蓝 `#08519C`），中心粗体 "L"。

转换为 `.ico`（需 Python + Pillow）：

```bat
python -c "from PIL import Image, ImageDraw, ImageFont; img = Image.new('RGBA',(256,256),(0,0,0,0)); d = ImageDraw.Draw(img); d.rectangle([30,30,180,180],outline=(107,174,214,255),width=12); d.rectangle([76,76,226,226],outline=(8,81,156,255),width=12); f = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf',120); d.text((128,128),'L',fill=(8,48,107,255),font=f,anchor='mm'); img.save('app.ico',format='ICO',sizes=[(16,16),(32,32),(48,48),(64,64),(128,128),(256,256)])"
```

图标通过 [resource.rc](wallpaper_player/resource.rc) 嵌入 exe（`1 ICON "app.ico"`），播放器启动时用 `LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, ...)` 加载到托盘。

---

## 十、系统要求

- **操作系统**：Windows 10 1903+ / Windows 11（需 `activeCodePage` UTF-8 支持）
- **显卡**：支持 D3D11 Feature Level 11_0 + H.264 DXVA 的 GPU（几乎所有 2010 年后的 GPU 均支持）
- **DPI**：支持 PerMonitorV2
- **制作器**：需 ffmpeg.exe（PATH 中或 `FFMPEG_PATH` 环境变量指定）
