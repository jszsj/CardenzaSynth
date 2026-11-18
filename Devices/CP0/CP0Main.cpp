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
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "Devices/CP0/Input.h"
#include "Devices/Device.h"
#include "Library/Synth/Audio/SynthAudioRuntime.h"
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

// ---- Linux key code (evdev) -> Cardenza 4x14 matrix (mirrors CP0ShellMain::MapScancode) ----
bool LinKeyToMatrix(std::uint16_t code, int& row, int& col, bool& isHome) {
  isHome = false;
  switch (code) {
    case KEY_ESC: case KEY_GRAVE: row = 0; col = 0; return true;
    case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5:
    case KEY_6: case KEY_7: case KEY_8: case KEY_9: case KEY_0:
      row = 0; col = 1 + (code - KEY_1); return true;
    case KEY_MINUS: row = 0; col = 11; return true;
    case KEY_EQUAL: row = 0; col = 12; return true;
    case KEY_BACKSPACE: case KEY_DELETE: row = 0; col = 13; return true;
    case KEY_TAB: row = 1; col = 0; return true;
    case KEY_Q: case KEY_W: case KEY_E: case KEY_R: case KEY_T: case KEY_Y:
    case KEY_U: case KEY_I: case KEY_O: case KEY_P:
      row = 1; col = 1 + (code - KEY_Q); return true;
    case KEY_LEFTBRACE: row = 1; col = 11; return true;
    case KEY_RIGHTBRACE: row = 1; col = 12; return true;
    case KEY_BACKSLASH: row = 1; col = 13; return true;
    case KEY_CAPSLOCK: case KEY_LEFTSHIFT: row = 2; col = 0; return true;
    case KEY_RIGHTSHIFT: row = 2; col = 1; return true;
    case KEY_A: case KEY_S: case KEY_D: case KEY_F: case KEY_G: case KEY_H:
    case KEY_J: case KEY_K: case KEY_L:
      row = 2; col = 2 + (code - KEY_A); return true;
    case KEY_SEMICOLON: row = 2; col = 11; return true;
    case KEY_APOSTROPHE: row = 2; col = 12; return true;
    case KEY_ENTER: case KEY_KPENTER: row = 2; col = 13; return true;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: row = 3; col = 0; return true;
    case KEY_LEFTALT: row = 3; col = 1; return true;
    case KEY_RIGHTALT: row = 3; col = 2; return true;
    case KEY_Z: case KEY_X: case KEY_C: case KEY_V: case KEY_B: case KEY_N: case KEY_M:
      row = 3; col = 3 + (code - KEY_Z); return true;
    case KEY_COMMA: row = 3; col = 10; return true;
    case KEY_DOT: row = 3; col = 11; return true;
    case KEY_SLASH: row = 3; col = 12; return true;
    case KEY_SPACE: row = 3; col = 13; return true;
    case KEY_HOME: isHome = true; row = 0; col = 0; return true;
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
      int row = 0, col = 0; bool home = false;
      if (!LinKeyToMatrix(ev.code, row, col, home)) continue;
      if (home) {
        CP0::Input::SetHomeButton(ev.value != 0);
      } else {
        CP0::Input::SetMatrixKey(static_cast<std::size_t>(row), static_cast<std::size_t>(col), ev.value != 0);
      }
    }
    // Hold ESC ~1.2 s to quit back to the launcher (no touch screen on the CP0).
    if (escHeld && std::chrono::steady_clock::now() - escDown >
                       std::chrono::milliseconds(1200)) {
      g_running = false;
    }
  }
};

// ---- ALSA output ----
struct Alsa {
  snd_pcm_t* pcm = nullptr;
  bool ok = false;
  std::thread thread;
  std::uint64_t renderedFrames = 0;
  std::vector<std::int16_t> mono;
  std::vector<std::int16_t> stereo;

  bool Open(const char* dev) {
    // Auto / explicit device selection. The CP0 codec (ES8389, card 1) is normally held by
    // pipewire, so plughw:1,0 (direct, needs pipewire stopped) or pulse (needs libasound2-plugins)
    // trade off. Try a sensible chain; fall back so the app still runs muted if none opens.
    const char* chain[] = {dev && *dev ? dev : "plughw:1,0", "hw:1,0", "default"};
    for (const char* candidate : chain) {
      if (candidate[0] == '\0') continue;
      if (snd_pcm_open(&pcm, candidate, SND_PCM_STREAM_PLAYBACK, 0) < 0) { pcm = nullptr; continue; }
      unsigned rate = synth::audio::kSampleRate;
      // Cap the buffer at ~40 ms. plughw's default buffer is huge, which caused multi-second
      // key-to-sound lag; soft_resample lets plughw convert 44.1k S16 to the ES8389's native rate.
      if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 40000) == 0) {
        snd_pcm_prepare(pcm);
        snd_pcm_uframes_t bufsz = 0, persz = 0;
        snd_pcm_get_params(pcm, &bufsz, &persz);
        const unsigned ms = (unsigned)((std::uint64_t)bufsz * 1000ULL / (rate ? rate : 44100));
        std::printf("CP0Main: ALSA %s %u Hz stereo buffer=%lu (~%ums)\n",
                    candidate, rate, (unsigned long)bufsz, ms);
        ok = true;
        return true;
      }
      snd_pcm_close(pcm); pcm = nullptr;
    }
    std::fprintf(stderr, "CP0Main: no ALSA playback device opened (running silent)\n");
    return false;
  }

  void ApplyVolume(std::int16_t* s, std::size_t n) {
    const std::int32_t vol = Device::Audio::OutputVolume();
    if (vol >= 100) return;
    const std::int32_t gain = (vol * 32768 + 50) / 100;
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(s[i]) * gain) / 32768);
  }

  void PumpClock() {
    // Native SynthSequencer runs its own clock thread (PumpClock exists only for the
    // single-threaded web build), so nothing to pump here.
  }

  void Run() {
    static const std::size_t kChunk = 256;  // ~5.8 ms @ 44.1k — keep well under the ~40 ms buffer
    mono.assign(kChunk, 0);
    stereo.assign(kChunk * 2, 0);
    while (g_running) {
      const std::size_t len = kChunk;
      synth::audio::RenderFrames(mono.data(), len);
      ApplyVolume(mono.data(), len);
      for (std::size_t i = 0; i < len; ++i) { stereo[i * 2] = mono[i]; stereo[i * 2 + 1] = mono[i]; }
      const snd_pcm_uframes_t want = static_cast<snd_pcm_uframes_t>(len);
      snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, stereo.data(), want);
      if (wrote < 0) { snd_pcm_recover(pcm, static_cast<int>(wrote), 1); wrote = snd_pcm_writei(pcm, stereo.data(), want); }
      renderedFrames += (wrote > 0) ? static_cast<std::uint64_t>(wrote) : static_cast<std::uint64_t>(len);
    }
  }

  void Start() { thread = std::thread([this] { Run(); }); }
  void Stop() {
    if (thread.joinable()) thread.join();
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); pcm = nullptr; }
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

}  // namespace

int main(int argc, char** argv) {
  std::string selfcheckFrames, ppmPath, wavPath, fbPath = "/dev/fb0", evPath, alsaDev = "default", benchSeconds;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
    if (a == "--selfcheck") next(selfcheckFrames);
    else if (a == "--bench") next(benchSeconds);
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
  if (alsa.ok) alsa.Stop();
  std::printf("CP0Main: exit\n");
  return 0;
}
