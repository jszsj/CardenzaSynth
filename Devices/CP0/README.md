# Devices/CP0 — CardputerZero (CP0) device family (P0 bring-up)

Cardenza 的 CP0 移植 **P0 最小后端**。目标：在 Linux/aarch64（CP0 同架构）上让真实固件跑起来，
并把引擎固定绘制的 **240×135** 画面**最近邻放大到 CP0 面板的 320×170**，音频走同一条 AMY→PCM 链。

## 做了什么
- `Device.cpp` —— `Device::Display`（引擎固定 240×135）。`Present()` 调用 `CP0::BuildPresented320()`
  把当前帧最近邻放大进 320×170 缓冲（设备端应 blit 到 `/dev/fb1`）。`Name()="CP0"`，`BackendName()="cp0-fb"`。
- `DeviceAudio.cpp` —— `CP0::RenderAudioBlock(int16*,n)`：设备 ALSA 输出线程与无头采集共用同一份 PCM。
- `Input.h` —— `CP0::Input` 脚本注入（`SetMatrixKey` 等），设备端换成 evdev（TCA8418）即可。
- `Storage.cpp` / `Config.cmake` / `ApplicationList.txt` / `CMakeLists.txt` / `Headless/`（CP0Shot）。

## 构建 / 运行
```bash
export PATH="$HOME/.local/opt/cmake-3.31/bin:$PATH"
cmake -S . -B build/cp0 -DDEVICE=CP0 -DCMAKE_BUILD_TYPE=Release
cmake --build build/cp0 --target CP0Shot -j

# 放大画面：真实 UI 逐状态抓帧 → 320×170 PNG
./build/cp0/Devices/CP0/CP0Shot --script tests/groove/tour.script \
    --out /tmp/cp0shots --root /tmp/cp0shots-root --scale 1

# 声音：按键试听 → CP0::RenderAudioBlock → WAV（即设备 ALSA 消费的同一 PCM）
./build/cp0/Devices/CP0/CP0Shot --wav /tmp/cp0shots/cp0_audio.wav --root /tmp/cp0shots-root
```

## 未完成（设备端 / 后续阶段）
- `Present()` → 真正 blit 到 `/dev/fb1`（现在只填 320×170 缓冲供采集）。
- ALSA 输出线程：循环 `CP0::RenderAudioBlock` → `snd_pcm_writei`（接 ES8389 codec）。
- evdev 键盘：`/dev/input/by-path/platform-3f804000.i2c-event`（TCA8418）→ 4×14 矩阵映射；
  **46 键↔56 键功能重映射是 P1 的主要设计工作。**
- 原生 320×170 布局重排（P2）；逐键灯效（CP0 无逐键 RGB，需改）。
- 真机未验证（本机为 aarch64 host 证明）。
