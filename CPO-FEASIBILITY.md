# CardenzaSynth 研究 + 复刻 CP0 版：可行性 / Spike / P0 记录

> 2026-11-12。仓库：`github.com/203-Systems/CardenzaSynth`（本地浅克隆在本目录）。CP0 = M5Stack CardputerZero。
> 本目录 `p0-artifacts/` 是全部实跑证据（320×170 帧 + WAV + 频谱）。

## 一、项目是什么
四轨 groovebox 合成器（鼓/Juno/DX7/Sampler），引擎 **AMY**（203-Systems fork，定点 DSP），ESP-IDF + **CMake 多目标**（真机 `Devices/Cardenza` / 桌面 `CardenzaSim` / Web `CardenzaWeb`）。一切存 SD，走自有 Launcher 分区约定。

## 二、官方"M5Stack Cardputer 兼容"是什么口径
产品页 *"Firmware Compatible with M5Stack Cardputer V1.0/V1.1 (via 203 Systems Launcher)"*。
- 指**通过 203 Systems Launcher 装应用**这条路，不是把本仓库 ESP-IDF 源码整片直刷。
- 本仓库 `Devices/Cardenza` 目标板是 16MB Flash + 2MB PSRAM + ES8156 codec 的自研板；GitHub Release `CardenzaSynthFirmware.bin`（v0.2.0）镜像头声明 16MB/DIO/80MHz —— 是给 16MB 板/Launcher 应用槽的，**别当整片固件往 8MB 的 Cardputer 直刷**。
- 我们研究的是**另一件事**：把它**复刻到 CP0（CardputerZero）**。

## 三、复刻到 CP0 的可行性（结论：可行，底座很有利）
CP0 = 树莓派 CM0（四核 A53@1GHz / 512MB / aarch64 Debian），320×170 屏、46 键，**ES8389 codec + 1W 喇叭 + 3.5mm**（音频比 ESP32 Cardputer 还好）。

| 维度 | 现状依据 | 到 CP0 |
|---|---|---|
| 合成引擎 AMY/音序器 | 纯 C++17，已在 ESP32+WASM 跑；**本机 aarch64 手编 `core_test` 通过** | 直接编译，算力远超 |
| 音频 | `DeviceAudio.h` 是干净的 `renderFrames(int16*,n)@44100` PCM 回调 | 接 **ALSA→ES8389**，小工作量 |
| 显示 | 引擎固定绘制 240×135 RGB565（`Device::Display`） | CP0 320×170：Present 时最近邻放大（P0 已做）/ 或原生重排（P2） |
| 键盘 | 4×14=56 键 + 逐键 RGB | **CP0 46 键、布局不同、逐键 RGB 大概率没有 → 最大难点（P1）** |
| 存储/打包 | SD；原生 framebuffer app 可打 `.deb` | 同构，可行 |
| 平台后端 | `Devices/` 已有 3 目标 + `Device.h` 抽象；但 Sim 宿主是 **Win32 专用**，Linux 无现成宿主 | **新写 `Devices/CP0`**（P0 已做后端骨架） |

## 四、Spike（先证明"能不能跑"最硬的两个假设，已用真实固件代码）
1. **音频**：编 `CardenzaDemo`（aarch64 ELF），用真实 AMY 引擎把 demo 乐谱渲染 4 轨 → 峰值 25406/18722/11330/6686、无削波；混成 20s 成品（`p0-artifacts/demo-song/`）。频谱确认真实音乐。
2. **界面**：编 `CardenzaSimShot`（跨平台无头 PNG 截帧器），重放官方 `tour.script` → 46 帧 240×135。
   - 期间修了一处工具源码 API 漂移：`render_demo.cpp` 的 `StepStore.at()` → `Edit()`。

## 五、P0 已实现（`Devices/CP0` 最小后端，本机出"声音 + 放大画面"）
**新增 `Devices/CP0/`**（克隆自已在 Linux 编译通过的 Sim 后端并改造）：
- `Device.cpp`：`Device::Display` 固定 240×135；新增 `CP0::BuildPresented320()`，`Present()` 时**最近邻放大 240×135→320×170**（设备上 blit 到 `/dev/fb1`，宿主上供 CP0Shot 抓帧）；`Name()="CP0"`，`BackendName()="cp0-fb"`。
- `DeviceAudio.cpp`：新增 `CP0::RenderAudioBlock(int16*,n)` —— 设备 ALSA 输出线程与无头采集都从这里取同一份 PCM。
- `Input.h`、`Storage.cpp`、`Config.cmake`、`ApplicationList.txt`、`CMakeLists.txt`、`Headless/`（CP0Shot）。

**构建 / 运行**（本机 aarch64 实测）：
```bash
export PATH="$HOME/.local/opt/cmake-3.31/bin:$PATH"
cmake -S . -B build/cp0 -DDEVICE=CP0 -DCMAKE_BUILD_TYPE=Release
cmake --build build/cp0 --target CP0Shot -j
# 放大画面：真实 UI 逐状态抓帧 → 320×170 PNG
./build/cp0/Devices/CP0/CP0Shot --script tests/groove/tour.script \
    --out /tmp/cp0shots --root /tmp/cp0shots-root --scale 1
# 声音：按键试听 → CP0 音频块(RenderAudioBlock) → WAV
./build/cp0/Devices/CP0/CP0Shot --wav /tmp/cp0shots/cp0_audio.wav --root /tmp/cp0shots-root
```
**P0 验证数据**：
- 320×170 帧 **46 张 / 39 唯一**，全为真实固件 UI（INSTR/MAP/MIX/TAPE/SETUP 等），满屏无空白。
- `cp0_audio.wav` 3.33s / 44100 / 立体声，**peak 17480、RMS 684、非零 8.5%**（试听段的真实音符），频谱两簇谐波可见。

## 六、剩余工作（真实工作量在"适配"，不是"重写引擎"）
- **P1 键盘**：CP0 46 键 → Cardenza 56 键功能的重映射（可能要加 fn/层）。目前 `CP0::Input` 仍是脚本注入；设备端要接 **evdev（TCA8418）** 并按 4×14 矩阵映射。
- **P2 界面**：现为"内部 240×135 放大到 320×170"（保真但非原生）。要做**原生 320×170 重排**（利用更大屏、去放大糊感）。
- **P3**：存储工程文件、打包 `.deb`（czdev，禁 root）、逐键灯效改造（CP0 无逐键 RGB，需砍/改屏幕提示）。
- **设备驱动**：`Present()` blit `/dev/fb1`、ALSA 输出线程、evdev 键盘输入（当前都走宿主脚本注入，真机未接）。
- **真机未验证**：本机是 aarch64 host 证明；CP0 实物驱动未接。

## 七、待用户拍板
1. 界面：先"放大 240×135 保真版"起步，还是必须一步到位 320×170 原生重排？
2. 按键：56→46 要"尽量还原全部操作（加 fn 层）"还是"精简功能迁就 46 键"？
3. 逐键 RGB 灯效：CP0 上是否必须保留（大概率没有）？
4. 目标：先本地能玩/出声，还是最终上 czdev 商店？

## 八、产物清单（`p0-artifacts/`）
- `cp0_montage.png`（6 格 320×170 UI 拼图）
- `cp0shot-frames/`（46 帧 320×170 PNG + leds）
- `cp0_audio.wav`（试听采样）、`cp0_audio_spec.png`、`cp0_audio_wave.png`
- `demo-song/neon-circuit-mix-20s.wav`（AMY 引擎整曲 20s）

## 九、Web 版（已上线）
项目自带完整浏览器模拟器 `Devices/CardenzaWeb`（真实产品码 `GrooveApp`+UI+AMY 编到 WASM，240×135 framebuffer + 4×14 背光键盘 + 真实音频 + IndexedDB 存档，无线程可嵌 iframe）。直接构建并部署：
```bash
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build/web -DDEVICE=CardenzaWeb -DCMAKE_BUILD_TYPE=Release
cmake --build build/web --target CardenzaWeb -j     # → build/web/site/{index.html,cardenza.js,cardenza.wasm,...}
```
**已部署：`https://cp0.asbox.net/cardenza/`**（`/home/web/html/cp0.asbox.net/cardenza/`；`cardenza.wasm` 1.58MB，nginx 已配 `application/wasm`）。
- 实测：标题「🐴 Cardenza Simulator」、canvas 240×135、booted 282ms、0 报错；鼠标/按键映射生效（状态行 `MOUSE → row X col Y`）；点击即启动音频（`Audio on` + LCD 电平表亮）；PLAY(Space) 可播当前轨。
- 键位按物理位置映射（KeyboardEvent.code），点页面键或用电脑键盘均可；音频需一次点击/按键解锁（浏览器 autoplay）；存档走 IndexedDB，刷新不丢。
- 说明：这是 **Cardenza 原生的 240×135 设备 UI**（不是 CP0 320×170 机身壳）。若要与 `/mahjong/` 完全同款的 **CP0 320×170 机身壳**，需另做一层把 240×135 放大/重排进 CP0 面板的 web 外壳（下一步可选）。

## 十、CP0 机壳 web 版（已上线，2026-11-12）
复用同一 `cardenza.js/wasm`（不再重编 C++），写一个 **CP0 极简壳驱动** `cp0-web/{index.html,cp0-web.js}`：
- 引擎 240×135 framebuffer → **最近邻放大铺满 320×170**（与 P0 `BuildPresented320` 同口径）；RGB565 LUT 与官方页一致。
- 电脑键盘（`KeyboardEvent.code` 物理位置）→ `_cardenza_web_set_key` 4×14 矩阵；`Home`→set_home；失焦/隐藏释放。
- 音频走同一 AudioWorklet（`cardenza-audio-worklet.js`）＋ ScriptProcessor 回落；会话/工程/采样仍走 IDBFS(`/sdcard`)。
- **关键 bug（已修）**：读 framebuffer 必须加基址 `_cardenza_web_framebuffer()>>1`，漏了它就从 HEAP 0 读成花屏噪声。

**已部署：`https://cp0.asbox.net/cardenza/`**（CP0 320×170 机壳版）；官方原版保留在 **`https://cp0.asbox.net/cardenza-official/`**。
实测：画布 320×170、booted、0 报错；点画面→`Audio on`；真键盘 Digit1 拍垫 + Space PLAY 生效（PLAY 键变绿、走带指示出现）、帧计数 ~60fps。

**真机外壳 = 官方模板自带（2026-11-12 定稿）**：麻将那台不是手搓 HTML，而是 CP0 官方 Template 的 **`DesktopSimulatorFrame`**（630×405，`src/app/desktop_simulator_frame.*`）——它把机身贴图 **`assets/images/cp0-light.png` / `cp0-dark.png`** 当背景，应用 320×170 的 LVGL 屏合成到 **(155,21)** 的屏幕洞，配 **`DesktopVirtualKeypad`（11×4=44 矩阵键，CP0 真机 46 键布局）**。故 CP0 web 版**直接用官方机身 PNG** 作背景：Cardenza 320×170 canvas 精确定位到 (155/630, 21/405) 屏幕洞，11×4 键位叠透明热点点击驱动 `_cardenza_web_set_key`（直连 Cardenza 矩阵；完整 46↔56 语义映射走电脑键盘，P1 另做）。像素级等于麻将那台。实测：机身+屏幕正确、点键帽启动音频、Space 播放后走带条+电平表亮起。

## 十一、真·CP0 设备 Web 构建（CP0Shell，2026-11-12，替换手写壳）
手写 HTML 壳被否（没有 emscripten logo/控制台日志/真 SDL 设备）。正解 = 在 Cardenza 自有构建里加一个 **SDL2 设备壳 web 目标**：
- 新文件 `Devices/CardenzaWeb/CP0ShellMain.cpp`：SDL2 窗口 630×405，`LoadPng("/cp0-shell-light.png")` 画官方机身（preload 的 `cp0-light.png`），引擎 `Device::Display::CurrentFrameBuffer()`（240×135 RGB565）转 ARGB 成 streaming 纹理，`SDL_RenderCopy` 到 (155,21,320,170) 屏幕洞（nearest）；SDL 键位按 `cardenza-web.js` 的 `KEYMAP`（code→[row,col]，4×14=56）驱动 `CardenzaSim::Input::SetMatrixKey`；音频 SDL 回调 `synth::audio::RenderFrames` + `PumpSequencerClock`（首次手势后再 unpause）。
- `Devices/CardenzaWeb/CMakeLists.txt` 加 `CP0Shell`：链接 `CardenzaWebDevice + CardenzaSystem`，`-sUSE_SDL=2 -sUSE_LIBPNG=1`（**编译与链接都要加**，否则 SDL_scancode 未声明）、`--preload-file cp0-light.png@/cp0-shell-light.png`，**不带 --shell-file**、`SUFFIX .html` → 默认 Emscripten 页（logo+控制台+spinner）。
- 构建：`emcmake cmake -S . -B build/webcp0 -DDEVICE=CardenzaWeb -DCMAKE_BUILD_TYPE=Release` → `cmake --build build/webcp0 --target CP0Shell` → `build/webcp0/cp0site/{index.html,index.js,index.wasm(2.3M),index.data}`。
- 实测（`https://cp0.asbox.net/cardenza/`）：canvas **630×405**、控制台刷 `CP0Shell: CardenzaWeb [sim-web] 240x135 screen 320x170`、机身+M5 屏幕洞内 Cardenza 正常；Tab 切轨(T1 DRUMS→T2 JUNO LEAD 1)、Space=PLAY 走带条亮 —— 真引擎驱动。屏上 11 键为近似映射，完整 4×14 走电脑键盘。

**音频（2026-11-12 修复）**：SDL 音频在 emscripten 默认壳里不出声——SDL 把按键处理推迟到 rAF 轮询，`resume()` 丢用户手势被自动播放策略挡；且 emscripten 的 `Module.ctx` 是 **SDL 的 WebGL 上下文**，用它当 AudioContext 守卫会误判。改为：boot 时 EM_ASM 挂**真正 DOM 手势监听**（pointerdown/keydown…），在手势栈内 `new AudioContext({sampleRate:44100})` 并 `createScriptProcessor` 拉 `cp0shell_render`；变量名用 `cp0ACtx/cp0Sp/cp0APtr` 避撞。注意 EM_ASM 宏按**括号**切参数，代码里不能有括号外的逗号（数组字面量会切碎宏，报 char 常量错），故事件监听逐条写。实测：上下文 running@44100、回调递增；**打压垫峰值 0.318（真实出声）**；PLAY 空工程静音属正常（开机工程无 pattern）。
