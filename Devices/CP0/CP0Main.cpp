// Cardenza on the CardputerZero (CP0): genuine NATIVE Linux host (P1).
//
// The engine draws into Device::Display (240x135); CP0::BuildPresented320() nearest-neighbour
// upscales to the CP0 panel's 320x170. This host blits that to /dev/fb0, reads the CP0 keyboard
// straight from evdev, and streams the synth's PCM (mono int16 @44.1k) to ALSA, pumping the
// sequencer clock exactly like the web shell does.
//
// Same engine as CardenzaWeb/CP0Shot (os::system::Begin/RunFrame). Only the host I/O is new.
//
// Host self-check (no fb/evdev/ALSA needed): `CP0Main --selfcheck 120 --ppm /tmp/f.ppm --wav /tmp/a.wav`
// boots the runtime, runs N frames, writes the last frame as PPM and renders the same PCM the codec
// would get. This is how the loop is verified on a build machine.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <miniaudio.h>  // CP0 audio backend: miniaudio -> libpulse0 -> PipeWire (see third_party/miniaudio_impl.c)
#include <atomic>
#include <chrono>

#include "Devices/CP0/Input.h"
#include "Devices/CP0/UsbMidi.h"
#include "Devices/Device.h"
#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include <cmath>
#include "Applications/Synth/Runtime/ProjectStorage.h"
#include "Applications/Synth/Runtime/SynthStorage.h"
#include "OS/System/System.h"

// CP0 device facade (Devices/CP0/Device.cpp). Read-only presentation hook.
namespace CP0 {
const std::uint16_t* BuildPresented320();
int PresentWidth();
int PresentHeight();
}  // namespace CP0

namespace {

volatile std::sig_atomic_t g_running = 1;
void OnSignal(int) { g_running = 0; }

constexpr int kPresentW = 320;
constexpr int kPresentH = 170;
constexpr int kFramePeriodMs = 16;
constexpr std::uint64_t kClockBlock = 256;

// ---- Linux evdev key code -> Cardenza 4x14 matrix (position mapping) ----
// The CP0 keyboard is 11 physical columns x 4 rows. This maps each PHYSICAL column to the
// Cardenza matrix column the user asked for (the leftmost column 1/sym/shift/fn is ignored):
//   physical col 2  (2 / Q / A / ctrl)      -> matrix col 0  (ESC / TAB / FN / CTRL)
//   physical col 3..10 (3/W/S/alt .. 0/O/L/M) -> matrix col 1..8 (pads)
//   physical col 11 (DEL / P / OK / space)   -> matrix col 13 (ADD / SUB / ENTER / PLAY)
// sym/fn are DRIVER layer keys: pressing them makes the SAME physical key emit the keymap1 /
// keymap2 code. So we also map those layer codes: under the sym layer (keymap1) physical cols
// 7..10 become the 9/0/-/= sequencer grid (matrix cols 9..12); the fn layer (keymap2) and every
// other sym code behave like the base layer, so pressing sym/fn never leaves the keyboard dead.
// TAB is no longer a layer toggle (the grid lives on the sym layer).
bool LinKeyToMatrix(std::uint16_t code, bool gridLayer, int& row, int& col, bool& isHome) {
  isHome = false;
  // Momentary grid layer (a second-column function key is held): the right four PAD columns (physical
  // 7..10) become the 4x4 sequencer grid (matrix cols 9..12) — the same cells the driver sym layer
  // exposes, but held rather than one-shot.
  if (gridLayer) {
    switch (code) {
      case 8:  row = 0; col = 9;  return true;
      case 9:  row = 0; col = 10; return true;
      case 10: row = 0; col = 11; return true;
      case 11: row = 0; col = 12; return true;
      case 21: row = 1; col = 9;  return true;
      case 22: row = 1; col = 10; return true;
      case 23: row = 1; col = 11; return true;
      case 24: row = 1; col = 12; return true;
      case 35: row = 2; col = 9;  return true;
      case 36: row = 2; col = 10; return true;
      case 37: row = 2; col = 11; return true;
      case 38: row = 2; col = 12; return true;
      case 47: row = 3; col = 9;  return true;
      case 48: row = 3; col = 10; return true;
      case 49: row = 3; col = 11; return true;
      case 50: row = 3; col = 12; return true;
      default: break;
    }
  }
  if (code == KEY_ESC)  { row = 0; col = 0; return true; }   // separate ESC -> ESC role (Poll: long-hold exits)
  if (code == KEY_TAB)  { row = 1; col = 0; return true; }   // separate TAB -> TAB role
  if (code == KEY_HOME) { isHome = true; row = 0; col = 0; return true; }
  switch (code) {
    case 3: row = 0; col = 0; return true;
    case 4: row = 0; col = 1; return true;
    case 5: row = 0; col = 2; return true;
    case 6: row = 0; col = 3; return true;
    case 7: row = 0; col = 4; return true;
    case 8: row = 0; col = 5; return true;
    case 9: row = 0; col = 6; return true;
    case 10: row = 0; col = 7; return true;
    case 11: row = 0; col = 8; return true;
    case 14: row = 0; col = 13; return true;
    case 16: row = 1; col = 0; return true;
    case 17: row = 1; col = 1; return true;
    case 18: row = 1; col = 2; return true;
    case 19: row = 1; col = 3; return true;
    case 20: row = 1; col = 4; return true;
    case 21: row = 1; col = 5; return true;
    case 22: row = 1; col = 6; return true;
    case 23: row = 1; col = 7; return true;
    case 24: row = 1; col = 8; return true;
    case 25: row = 1; col = 13; return true;
    case 27: row = 0; col = 0; return true;
    case 28: row = 2; col = 13; return true;
    case 29: row = 3; col = 0; return true;
    case 30: row = 2; col = 0; return true;
    case 31: row = 2; col = 1; return true;
    case 32: row = 2; col = 2; return true;
    case 33: row = 2; col = 3; return true;
    case 34: row = 2; col = 4; return true;
    case 35: row = 2; col = 5; return true;
    case 36: row = 2; col = 6; return true;
    case 37: row = 2; col = 7; return true;
    case 38: row = 2; col = 8; return true;
    case 39: row = 0; col = 1; return true;
    case 40: row = 0; col = 2; return true;
    case 41: row = 0; col = 3; return true;
    case 43: row = 0; col = 4; return true;
    case 44: row = 3; col = 2; return true;
    case 45: row = 3; col = 3; return true;
    case 46: row = 3; col = 4; return true;
    case 47: row = 3; col = 5; return true;
    case 48: row = 3; col = 6; return true;
    case 49: row = 3; col = 7; return true;
    case 50: row = 3; col = 8; return true;
    case 51: row = 0; col = 9; return true;
    case 52: row = 0; col = 10; return true;
    case 53: row = 0; col = 11; return true;
    case 55: row = 1; col = 0; return true;
    case 56: row = 3; col = 1; return true;
    case 57: row = 3; col = 13; return true;
    case 60: row = 0; col = 0; return true;
    case 61: row = 0; col = 1; return true;
    case 62: row = 0; col = 2; return true;
    case 63: row = 0; col = 3; return true;
    case 64: row = 0; col = 4; return true;
    case 65: row = 0; col = 5; return true;
    case 66: row = 0; col = 6; return true;
    case 67: row = 0; col = 7; return true;
    case 68: row = 0; col = 8; return true;
    case 69: row = 1; col = 1; return true;
    case 70: row = 1; col = 2; return true;
    case 71: row = 1; col = 3; return true;
    case 72: row = 1; col = 4; return true;
    case 73: row = 1; col = 9; return true;
    case 74: row = 1; col = 10; return true;
    case 75: row = 1; col = 11; return true;
    case 76: row = 1; col = 12; return true;
    case 77: row = 1; col = 13; return true;
    case 79: row = 2; col = 0; return true;
    case 80: row = 2; col = 1; return true;
    case 81: row = 2; col = 2; return true;
    case 82: row = 2; col = 4; return true;
    case 83: row = 2; col = 9; return true;
    case 85: row = 2; col = 10; return true;
    case 86: row = 2; col = 11; return true;
    case 87: row = 1; col = 8; return true;
    case 88: row = 1; col = 13; return true;
    case 89: row = 2; col = 12; return true;
    case 90: row = 3; col = 9; return true;
    case 91: row = 3; col = 10; return true;
    case 92: row = 3; col = 11; return true;
    case 93: row = 3; col = 12; return true;
    case 94: row = 0; col = 12; return true;
    case 99: row = 2; col = 6; return true;
    case 102: row = 2; col = 7; return true;
    case 103: row = 2; col = 3; return true;
    case 104: row = 2; col = 8; return true;
    case 105: row = 3; col = 2; return true;
    case 106: row = 3; col = 4; return true;
    case 107: row = 3; col = 7; return true;
    case 108: row = 3; col = 3; return true;
    case 109: row = 3; col = 8; return true;
    case 110: row = 3; col = 6; return true;
    case 111: row = 0; col = 13; return true;
    case 113: row = 2; col = 0; return true;
    case 114: row = 2; col = 1; return true;
    case 115: row = 2; col = 2; return true;
    case 138: row = 2; col = 5; return true;
    case 163: row = 1; col = 2; return true;
    case 164: row = 1; col = 0; return true;
    case 165: row = 1; col = 1; return true;
    case 224: row = 1; col = 6; return true;
    case 225: row = 1; col = 7; return true;
    default: return false;
  }
}

// ---- Framebuffer ----
struct Framebuffer {
  int fd = -1;
  std::uint8_t* mem = nullptr;
  std::size_t len = 0;
  fb_var_screeninfo var{};
  fb_fix_screeninfo fix{};
  bool ok = false;

  bool Open(const char* path) {
    fd = ::open(path, O_RDWR);
    if (fd < 0) {
      std::fprintf(stderr, "CP0Main: open %s: %s\n", path, std::strerror(errno));
      return false;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
      std::fprintf(stderr, "CP0Main: fb ioctl: %s\n", std::strerror(errno));
      ::close(fd); fd = -1; return false;
    }
    len = fix.smem_len ? static_cast<std::size_t>(fix.smem_len)
                       : static_cast<std::size_t>(fix.line_length) * var.yres_virtual;
    mem = static_cast<std::uint8_t*>(mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mem == MAP_FAILED) {
      std::fprintf(stderr, "CP0Main: mmap fb: %s\n", std::strerror(errno));
      ::close(fd); fd = -1; mem = nullptr; return false;
    }
    std::printf("CP0Main: fb %dx%d bpp=%u line=%u\n", var.xres, var.yres, var.bits_per_pixel, fix.line_length);
    ok = true;
    return true;
  }

  // Blit the 320x170 RGB565 presented frame. Handles 16bpp RGB565 and 32bpp XRGB8888.
  void Blit(const std::uint16_t* src) {
    if (!ok) return;
    const int w = var.xres < kPresentW ? var.xres : kPresentW;
    const int h = var.yres < kPresentH ? var.yres : kPresentH;
    if (var.bits_per_pixel == 16) {
      for (int y = 0; y < h; ++y) {
        std::uint8_t* dst = mem + static_cast<std::size_t>(y) * fix.line_length;
        std::memcpy(dst, src + static_cast<std::size_t>(y) * kPresentW, static_cast<std::size_t>(w) * 2);
      }
    } else if (var.bits_per_pixel == 32) {
      for (int y = 0; y < h; ++y) {
        std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(mem + static_cast<std::size_t>(y) * fix.line_length);
        const std::uint16_t* s = src + static_cast<std::size_t>(y) * kPresentW;
        for (int x = 0; x < w; ++x) {
          const std::uint16_t c = s[x];
          const std::uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
          dst[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
      }
    }
  }
};

// ---- evdev keyboard ----
struct Keyboard {
  int fd = -1;
  bool ok = false;
  bool escHeld = false;
  std::chrono::steady_clock::time_point escDown{};
  int funcColHeld = 0;  // second-column function keys currently held -> momentary grid layer
  std::unordered_map<std::uint16_t, std::pair<int, int>> heldCells;  // code -> matrix cell (-1 row = home)

  static bool NameLooksKeyboard(int fd) {
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return false;
    // Prefer the CP0 internal keyboard; fall back to anything that reports letter keys.
    unsigned long keys[1 + KEY_MAX / (8 * sizeof(unsigned long))] = {0};
    const bool hasLetters = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
                            (keys[KEY_A / (8 * sizeof(unsigned long))] >> (KEY_A % (8 * sizeof(unsigned long))) & 1UL);
    const std::string n(name);
    return hasLetters || n.find("cardputer") != std::string::npos || n.find("internal") != std::string::npos;
  }

  bool Open(const char* path) {
    if (path && *path) {
      fd = ::open(path, O_RDONLY | O_NONBLOCK);
      ok = fd >= 0;
      if (ok) std::printf("CP0Main: kbd %s\n", path);
      else std::fprintf(stderr, "CP0Main: open kbd %s: %s\n", path, std::strerror(errno));
      return ok;
    }
    // No path given: scan /dev/input/event* and pick the first keyboard-like device.
    for (int i = 0; i < 32; ++i) {
      char p[32];
      std::snprintf(p, sizeof(p), "/dev/input/event%d", i);
      int f = ::open(p, O_RDONLY | O_NONBLOCK);
      if (f < 0) continue;
      if (NameLooksKeyboard(f)) { fd = f; ok = true; std::printf("CP0Main: kbd %s\n", p); return true; }
      ::close(f);
    }
    std::fprintf(stderr, "CP0Main: no keyboard evdev found\n");
    return false;
  }

  void Poll() {
    if (!ok) return;
    input_event ev{};
    while (true) {
      const ssize_t n = ::read(fd, &ev, sizeof(ev));
      if (n != static_cast<ssize_t>(sizeof(ev))) break;
      if (ev.type != EV_KEY) continue;
      if (ev.code == KEY_ESC) {
        if (ev.value == 1) { escDown = std::chrono::steady_clock::now(); escHeld = true; }
        else if (ev.value == 0) { escHeld = false; }
      }
      // Count held second-column keys — exactly 2 / q / a / ctrl (their base + sym/fn-layer codes all
      // land on matrix col 0, but ESC/TAB also do and are NOT in this column, so exclude them). While
      // at least one of these four is down the momentary grid layer is active.
      {
        int r0 = 0, c0 = 0; bool h0 = false;
        if (ev.code != KEY_ESC && ev.code != KEY_TAB &&
            LinKeyToMatrix(ev.code, false, r0, c0, h0) && !h0 && c0 == 0) {
          if (ev.value == 1) ++funcColHeld;
          else if (ev.value == 0 && funcColHeld > 0) --funcColHeld;
        }
      }
      const bool gridLayer = funcColHeld > 0;
      int row = 0, col = 0; bool home = false;
      if (ev.value != 0) {
        // Press / auto-repeat: map with the current layer and remember the cell so the release matches
        // even if the grid layer changes while the key is held.
        if (!LinKeyToMatrix(ev.code, gridLayer, row, col, home)) continue;
        if (ev.value == 1) heldCells[ev.code] = home ? std::make_pair(-1, -1) : std::make_pair(row, col);
        if (home) CP0::Input::SetHomeButton(true);
        else CP0::Input::SetMatrixKey(static_cast<std::size_t>(row), static_cast<std::size_t>(col), true);
      } else {
        // Release: prefer the cell this key actually pressed; fall back to the current mapping.
        auto it = heldCells.find(ev.code);
        if (it != heldCells.end()) {
          row = it->second.first; col = it->second.second; home = row < 0;
          heldCells.erase(it);
        } else if (!LinKeyToMatrix(ev.code, gridLayer, row, col, home)) {
          continue;
        }
        if (home) CP0::Input::SetHomeButton(false);
        else CP0::Input::SetMatrixKey(static_cast<std::size_t>(row), static_cast<std::size_t>(col), false);
      }
    }
    // Hold ESC ~1.2 s to quit back to the launcher (no touch screen on the CP0).
    if (escHeld && std::chrono::steady_clock::now() - escDown >
                       std::chrono::milliseconds(1200)) {
      g_running = false;
    }
  }
};

// ---- Audio output (miniaudio -> libpulse0 -> PipeWire/PulseAudio) ----
// Same backend as the factory CP0 apps (e.g. Piano): miniaudio over PulseAudio -> pipewire-pulse ->
// ES8389, depending only on libpulse0 (base image). No libasound2 / pipewire-alsa for audio.
//
// Rendering runs on this object's own thread, NOT inside miniaudio's real-time PulseAudio callback:
// RenderFrames takes g_renderMutex and does real work (sampler voices, AMY fetch), so calling it from
// the RT audio callback let UI/render-lock contention or a slow block miss the callback deadline and
// drop frames (heard as sampler playback stutter). We instead render into an SPSC ring here and the
// callback only memcpy's out — microsecond-fast, never blocks — which is how the old ALSA path
// (dedicated thread + ~40ms buffer) stayed smooth.
struct Alsa {
  static constexpr std::size_t kChunk = 256;        // render block (~5.8ms @44.1k), matches the engine
  static constexpr std::size_t kRingFrames = 2048;  // ~46ms of stereo slack (old ALSA cap was ~40ms)

  ma_context context{};
  ma_device device{};
  std::thread renderThread;
  bool ok = false;
  bool running = false;
  bool contextInit = false;
  bool deviceInit = false;
  std::atomic<bool> renderRun{false};
  std::atomic<std::uint64_t> renderedFrames{0};
  std::atomic<std::uint64_t> underruns{0};

  // SPSC ring of interleaved stereo int16. head/tail count frames and wrap with % kRingFrames. Only
  // the render thread advances head, only the PulseAudio callback advances tail, so acquire/release
  // on these two atomics is enough (single-producer / single-consumer).
  std::vector<std::int16_t> ring = std::vector<std::int16_t>(kRingFrames * 2, 0);
  std::atomic<std::size_t> head{0};
  std::atomic<std::size_t> tail{0};

  void ApplyVolume(std::int16_t* s, std::size_t n) {
    const std::int32_t vol = Device::Audio::OutputVolume();
    if (vol >= 100) return;
    const std::int32_t gain = (vol * 32768 + 50) / 100;
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(s[i]) * gain) / 32768);
  }

  // Producer: render continuously into the ring, paced by how much the callback drains.
  void RenderLoop() {
    std::vector<std::int16_t> mono(kChunk);
    while (renderRun.load(std::memory_order_relaxed)) {
      const std::size_t h = head.load(std::memory_order_relaxed);
      const std::size_t t = tail.load(std::memory_order_acquire);
      if ((h - t) + kChunk > kRingFrames) {  // ring full: let the device catch up
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        continue;
      }
      synth::audio::RenderFrames(mono.data(), kChunk);
      ApplyVolume(mono.data(), kChunk);
      for (std::size_t i = 0; i < kChunk; ++i) {
        const std::size_t slot = (h + i) % kRingFrames;
        ring[slot * 2] = mono[i];
        ring[slot * 2 + 1] = mono[i];
      }
      head.store(h + kChunk, std::memory_order_release);
      renderedFrames.store(renderedFrames.load(std::memory_order_relaxed) + kChunk, std::memory_order_relaxed);
    }
  }

  // Consumer: runs on miniaudio's PulseAudio thread. Copy queued stereo and zero-fill any shortfall
  // (an underrun — should not happen once running; counted for diagnostics). Never blocks.
  static void DataCallback(ma_device* dev, void* output, const void* input, ma_uint32 frameCount) {
    (void)input;
    Alsa* self = static_cast<Alsa*>(dev->pUserData);
    std::int16_t* out = static_cast<std::int16_t*>(output);
    if (out == nullptr) return;
    if (self == nullptr) { std::memset(out, 0, static_cast<std::size_t>(frameCount) * 2 * sizeof(std::int16_t)); return; }
    const std::size_t t = self->tail.load(std::memory_order_relaxed);
    const std::size_t h = self->head.load(std::memory_order_acquire);
    std::size_t avail = h - t;
    if (avail > frameCount) avail = frameCount;
    for (std::size_t i = 0; i < avail; ++i) {
      const std::size_t slot = (t + i) % kRingFrames;
      out[i * 2] = self->ring[slot * 2];
      out[i * 2 + 1] = self->ring[slot * 2 + 1];
    }
    for (ma_uint32 i = static_cast<ma_uint32>(avail); i < frameCount; ++i) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
    if (avail < frameCount) self->underruns.store(self->underruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    self->tail.store(t + avail, std::memory_order_release);
  }

  bool Open(const char* dev) {
    (void)dev;  // PulseAudio picks the default sink; the system volume/mute applies to us.
    ma_backend backends[] = {ma_backend_pulseaudio};
    if (ma_context_init(backends, 1, nullptr, &context) != MA_SUCCESS) {
      std::fprintf(stderr, "CP0Main: PulseAudio context init failed (pipewire-pulse / libpulse0 present?)\n");
      return false;
    }
    contextInit = true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(synth::audio::kSampleRate);
    cfg.periodSizeInMilliseconds = 20;  // small period; our ring supplies the real slack
    cfg.dataCallback = &Alsa::DataCallback;
    cfg.pUserData = this;
    if (ma_device_init(&context, &cfg, &device) != MA_SUCCESS) {
      std::fprintf(stderr, "CP0Main: PulseAudio playback device init failed\n");
      ma_context_uninit(&context); contextInit = false;
      return false;
    }
    deviceInit = true;
    ok = true;
    std::printf("CP0Main: miniaudio %s %u Hz stereo period=%uframes ring=%zums (pulse, threaded)\n",
                ma_get_backend_name(context.backend), cfg.sampleRate,
                device.playback.internalPeriodSizeInFrames,
                (std::size_t)((std::uint64_t)kRingFrames * 1000ull / (cfg.sampleRate ? cfg.sampleRate : 44100u)));
    return true;
  }

  void PumpClock() {
    // Native SynthSequencer runs its own clock thread (PumpClock exists only for the
    // single-threaded web build), so nothing to pump here.
  }

  void Start() {
    if (!ok || running) return;
    renderRun.store(true, std::memory_order_relaxed);
    // Pre-fill the ring so the first PulseAudio callback has data (avoids a start-up underrun/click).
    {
      std::vector<std::int16_t> mono(kChunk);
      std::size_t h = head.load(std::memory_order_relaxed);
      while (h + kChunk <= kRingFrames) {
        synth::audio::RenderFrames(mono.data(), kChunk);
        ApplyVolume(mono.data(), kChunk);
        for (std::size_t i = 0; i < kChunk; ++i) { ring[(h + i) * 2] = mono[i]; ring[(h + i) * 2 + 1] = mono[i]; }
        h += kChunk;
      }
      head.store(h, std::memory_order_release);
      renderedFrames.store(renderedFrames.load(std::memory_order_relaxed) + h, std::memory_order_relaxed);
    }
    if (ma_device_start(&device) != MA_SUCCESS) { renderRun.store(false, std::memory_order_relaxed); return; }
    renderThread = std::thread([this] { RenderLoop(); });
    running = true;
  }

  void Stop() {
    renderRun.store(false, std::memory_order_relaxed);
    if (deviceInit) { ma_device_stop(&device); }
    if (renderThread.joinable()) renderThread.join();
    running = false;
    if (deviceInit) { ma_device_uninit(&device); deviceInit = false; }
    if (contextInit) { ma_context_uninit(&context); contextInit = false; }
    ok = false;
    std::printf("CP0Main: audio stopped, rendered=%llu underruns=%llu\n",
                (unsigned long long)renderedFrames.load(), (unsigned long long)underruns.load());
  }
};

std::vector<std::uint8_t> FrameToRgb(const std::uint16_t* px) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kPresentW) * kPresentH * 3);
  for (int i = 0; i < kPresentW * kPresentH; ++i) {
    const std::uint16_t c = px[i];
    rgb[i * 3 + 0] = static_cast<std::uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    rgb[i * 3 + 1] = static_cast<std::uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
    rgb[i * 3 + 2] = static_cast<std::uint8_t>((c & 0x1F) * 255 / 31);
  }
  return rgb;
}

void WritePpm(const char* path, const std::uint16_t* px) {
  const auto rgb = FrameToRgb(px);
  FILE* f = std::fopen(path, "wb");
  if (!f) { std::fprintf(stderr, "CP0Main: cannot write %s\n", path); return; }
  std::fprintf(f, "P6\n%d %d\n255\n", kPresentW, kPresentH);
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
  std::printf("CP0Main: wrote %s\n", path);
}

void WriteWavMono(const char* path, const std::vector<std::int16_t>& pcm) {
  FILE* f = std::fopen(path, "wb");
  if (!f) { std::fprintf(stderr, "CP0Main: cannot write %s\n", path); return; }
  auto put32 = [&](std::uint32_t v) { std::uint8_t b[4] = {(std::uint8_t)v, (std::uint8_t)(v >> 8), (std::uint8_t)(v >> 16), (std::uint8_t)(v >> 24)}; std::fwrite(b, 1, 4, f); };
  auto put16 = [&](std::uint16_t v) { std::uint8_t b[2] = {(std::uint8_t)v, (std::uint8_t)(v >> 8)}; std::fwrite(b, 1, 2, f); };
  const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size() * 2);
  std::fwrite("RIFF", 1, 4, f); put32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f); put32(16); put16(1); put16(1); put32(44100); put32(88200); put16(2); put16(16);
  std::fwrite("data", 1, 4, f); put32(dataBytes);
  std::fwrite(pcm.data(), 1, pcm.size() * 2, f);
  std::fclose(f);
  std::printf("CP0Main: wrote %s (%zu frames)\n", path, pcm.size());
}

// Headless self-check: run N frames, no fb/evdev/ALSA. Writes last frame PPM + audio WAV.
int RunSelfCheck(int frames, const char* ppmPath, const char* wavPath) {
  if (!os::system::Begin()) { std::fprintf(stderr, "CP0Main: boot failed\n"); return 1; }
  std::printf("CP0Main[selfcheck]: %s [%s] presented %dx%d\n", Device::Name().c_str(),
              Device::BackendName().c_str(), CP0::PresentWidth(), CP0::PresentHeight());
  std::vector<std::int16_t> pcm;
  std::vector<std::int16_t> block(512);
  for (int i = 0; i < frames; ++i) {
    if (i == 12) CP0::Input::SetMatrixKey(2, 13, true);   // Enter: dismiss the boot splash
    if (i == 14) CP0::Input::SetMatrixKey(2, 13, false);
    if (i == 40) CP0::Input::SetMatrixKey(2, 13, true);   // Enter again: start the app
    if (i == 42) CP0::Input::SetMatrixKey(2, 13, false);
    os::system::RunFrame();
    const std::size_t got = synth::audio::RenderFrames(block.data(), block.size());
    pcm.insert(pcm.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got));
  }
  const std::uint16_t* px = CP0::BuildPresented320();
  if (ppmPath && *ppmPath) WritePpm(ppmPath, px);
  if (wavPath && *wavPath) WriteWavMono(wavPath, pcm);
  std::printf("CP0Main[selfcheck]: %d frames ok\n", frames);
  return 0;
}

// --bench S: render S seconds of audio and report how fast vs realtime (does the CPU keep up?).
int RunBench(double seconds) {
  if (!os::system::Begin()) { std::fprintf(stderr, "CP0Main: boot failed\n"); return 1; }
  const std::size_t chunk = 256;
  std::vector<std::int16_t> buf(chunk, 0);
  for (int i = 0; i < 64; ++i) synth::audio::RenderFrames(buf.data(), chunk);  // warm up
  const std::size_t frames = (std::size_t)(seconds * (double)synth::audio::kSampleRate);
  std::size_t done = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (done < frames) { synth::audio::RenderFrames(buf.data(), chunk); done += chunk; }
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double audio = (double)done / (double)synth::audio::kSampleRate;
  std::printf("CP0Main[bench]: rendered %.2fs audio in %.3fs wall -> %.2fx realtime (%.0f%% of one core)\n",
              audio, wall, wall > 0 ? audio / wall : 0.0, wall > 0 ? 100.0 * wall / audio : 0.0);
  return 0;
}

// --mic S: capture S seconds straight from the codec through the sampler's audio-input path,
// report peak/RMS and drop a WAV. Verifies on-device recording without the UI or /dev/fb0.
int RunMicTest(double seconds) {
  using namespace Device::Audio;
  const InputStreamInfo info = InputInfo();
  std::printf("CP0Main[mic]: info rate=%u ch=%u bits=%u\n",
              info.sampleRate, info.channelCount, info.bitsPerSample);
  const InputStatus st = StartInput();
  std::printf("CP0Main[mic]: StartInput=%s\n",
              st == InputStatus::Ready ? "READY" : (st == InputStatus::NotReady ? "NOTREADY" : "ERR"));
  if (st != InputStatus::Ready) return 2;
  const std::size_t total = static_cast<std::size_t>(seconds * static_cast<double>(info.sampleRate));
  std::vector<std::int16_t> pcm;
  pcm.reserve(total);
  std::vector<std::int16_t> chunk(1024);
  int peak = 0;
  double sumsq = 0.0;
  int idleReads = 0;
  const int kMaxIdleReads = 1200;  // ~6s of empty reads -> give up instead of spinning forever
  while (pcm.size() < total) {
    std::size_t want = std::min<std::size_t>(chunk.size(), total - pcm.size());
    std::size_t framesRead = 0;
    const InputStatus r = ReadInputFrames(chunk.data(), want, framesRead, 500);
    if (framesRead == 0) {
      if (++idleReads > kMaxIdleReads) { std::printf("CP0Main[mic]: no input frames (capture not delivering)\n"); break; }
      if (r == InputStatus::NotReady) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
      break;
    }
    idleReads = 0;
    for (std::size_t i = 0; i < framesRead; ++i) {
      const int s = chunk[i];
      const int a = s < 0 ? -s : s;
      if (a > peak) peak = a;
      sumsq += static_cast<double>(s) * static_cast<double>(s);
    }
    pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(framesRead));
  }
  StopInput();
  const double rms = pcm.empty() ? 0.0 : std::sqrt(sumsq / static_cast<double>(pcm.size()));
  std::printf("CP0Main[mic]: captured %zu frames  peak=%d (%.1f%%)  rms=%.1f  (~%.1f dBFS)\n",
              pcm.size(), peak, 100.0 * peak / 32768.0, rms,
              20.0 * std::log10((rms < 1e-6 ? 1e-6 : rms) / 32768.0));
  WriteWavMono("/tmp/mic_test.wav", pcm);
  return 0;
}

// --persist [w|r|rw]: exercise the real project save/load on the card. `w` writes a project with
// distinctive values, `r` reads it back (a fresh process = the storage surviving a restart), `rw`
// (default) does both. Compares the bytes-fingerprint and key fields so a format/IO regression shows.
int RunPersist(const std::string& mode) {
  using namespace synth;
  storage::EnsureLayout();
  const std::string autosave = storage::SessionDirectory() + "/autosave.ccproj";
  const std::string path = storage::ProjectDirectory() + "/zz_persist_test.ccproj";
  if (mode != "r") {
    ProjectData base;
    if (LoadProjectFileAt(autosave, base) != ProjectResult::Ok) {
      std::printf("CP0Main[persist]: no valid autosave to seed from (%s)\n", autosave.c_str());
      return 3;
    }
    base.sequence->bpm = 111;  // distinctive edit (within tempo range)
    base.mixer.master = 42;
    const std::uint32_t fp = ProjectFingerprint(base);
    const ProjectResult r = SaveProjectFileAt(path, base);
    std::printf("CP0Main[persist]: WRITE save=%d fp=0x%08x bpm=%u master=%u\n",
                (int)r, fp, (unsigned)base.sequence->bpm, base.mixer.master);
    if (r != ProjectResult::Ok) return 1;
    if (mode == "w") return 0;
  }
  ProjectData q;
  const ProjectResult r = LoadProjectFileAt(path, q);
  const std::uint32_t fp = q.sequence ? ProjectFingerprint(q) : 0u;
  std::printf("CP0Main[persist]: READ load=%d fp=0x%08x bpm=%u swing=%u master=%u tracks=%zu\n",
              (int)r, fp,
              q.sequence ? (unsigned)q.sequence->bpm : 0u,
              q.sequence ? (unsigned)q.sequence->swing : 0u,
              q.mixer.master, q.sequence ? q.sequence->tracks.size() : 0u);
  const bool ok = r == ProjectResult::Ok && q.sequence && q.sequence->bpm == 111 && q.mixer.master == 42;
  std::printf("CP0Main[persist]: FIELDS %s\n", ok ? "OK" : "MISMATCH");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string selfcheckFrames, ppmPath, wavPath, fbPath = "/dev/fb0", evPath, alsaDev = "default", benchSeconds, micSeconds, persistMode;
  bool persistFlag = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
    if (a == "--selfcheck") next(selfcheckFrames);
    else if (a == "--bench") next(benchSeconds);
    else if (a == "--mic") next(micSeconds);
    else if (a == "--persist") { persistFlag = true; if (i + 1 < argc && argv[i + 1][0] != '-') persistMode = argv[++i]; }
    else if (a == "--ppm") next(ppmPath);
    else if (a == "--wav") next(wavPath);
    else if (a == "--fb") next(fbPath);
    else if (a == "--evdev") next(evPath);
    else if (a == "--alsa") next(alsaDev);
    else if (a == "-h" || a == "--help") {
      std::printf("usage: CP0Main [--fb /dev/fb0] [--evdev PATH] [--alsa default]\n"
                  "       CP0Main --selfcheck N [--ppm out.ppm] [--wav out.wav]\n");
      return 0;
    }
  }

  if (!selfcheckFrames.empty()) {
    return RunSelfCheck(std::atoi(selfcheckFrames.c_str()), ppmPath.c_str(), wavPath.c_str());
  }
  if (!benchSeconds.empty()) {
    return RunBench(std::atof(benchSeconds.c_str()));
  }
  if (!micSeconds.empty()) {
    return RunMicTest(std::atof(micSeconds.c_str()));
  }
  if (persistFlag) {
    return RunPersist(persistMode.empty() ? "rw" : persistMode);
  }

  // The device keeps its card in the user's data dir; run from there so Device::Storage's
  // working-directory sdcard/ resolves to it whatever directory the launcher starts us in.
  {
    std::error_code ec;
    const char* home = std::getenv("HOME");
    const std::filesystem::path dataDir = std::filesystem::path(home ? home : ".") / ".local/share/cardenza";
    std::filesystem::create_directories(dataDir, ec);
    std::filesystem::current_path(dataDir, ec);
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  Framebuffer fb;
  fb.Open(fbPath.c_str());  // non-fatal: run headless (draw nowhere) if absent
  Keyboard kbd;
  kbd.Open(evPath.c_str());
  Alsa alsa;
  alsa.Open(alsaDev.c_str());

  if (!os::system::Begin()) { std::fprintf(stderr, "CP0Main: boot failed\n"); return 1; }
  std::printf("CP0Main: %s [%s] live\n", Device::Name().c_str(), Device::BackendName().c_str());

  if (alsa.ok) alsa.Start();
  CP0::UsbMidi::Start();  // plug a USB-MIDI keyboard into the CP0 host port: it plays the synth
  CP0::UsbMidi::WakePanel();  // turn the panel on: run standalone, nothing else enables the backlight

  auto next = std::chrono::steady_clock::now();
  while (g_running) {
    next += std::chrono::milliseconds(kFramePeriodMs);
    kbd.Poll();
    os::system::RunFrame();
    fb.Blit(CP0::BuildPresented320());
    const auto now = std::chrono::steady_clock::now();
    if (now < next) std::this_thread::sleep_until(next);
    else next = now;
  }
  g_running = 0;
  CP0::UsbMidi::Stop();
  if (alsa.ok) alsa.Stop();
  std::printf("CP0Main: exit\n");
  return 0;
}
