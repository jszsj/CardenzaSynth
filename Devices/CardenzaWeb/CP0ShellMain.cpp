// Cardenza on the CardputerZero (CP0): genuine web host.
//
// Renders the OFFICIAL CP0 device shell (630x405, the same assets/images/cp0-light.png the
// CardputerZero Template's DesktopSimulatorFrame composites) with the Cardenza engine frame
// composited into the 320x170 screen at (155,21), using SDL2 exactly like
// DesktopSimulatorFrame/DesktopVirtualKeypad do. Built with Emscripten WITHOUT --shell-file,
// so the page is the default Emscripten shell (logo + console + spinner), the same nature as
// the mahjong page.
//
// The engine side reuses the proven CardenzaSim web device facade + CardenzaSystem (the same
// wasm that CardenzaWeb ships): os::system::Begin/RunFrame, Device::Display framebuffer and
// synth::audio::RenderFrames. Only the host (device drawing, input, audio out) is new here.

#include <emscripten/emscripten.h>

#include <SDL.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Applications/Synth/GrooveApp.h"
#include "Applications/Synth/UI/Groove/GrooveHost.h"
#include "Devices/CardenzaSim/SimulatorInput.h"
#include "Devices/Device.h"
#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include "OS/System/System.h"

namespace {

// Device window (the CP0 body art) and the screen hole the engine frame is composited into.
constexpr int kDevW = 630;
constexpr int kDevH = 405;
constexpr int kScreenX = 155;
constexpr int kScreenY = 21;
constexpr int kScreenW = 320;
constexpr int kScreenH = 170;

// Keypad geometry in device (630x405) coordinates, identical to DesktopVirtualKeypad.
constexpr int kKeyX = 8;
constexpr int kKeyY = 195;
constexpr int kKeyW = 50;
constexpr int kKeyH = 47;
constexpr int kKeyCols = 11;
constexpr int kKeyRows = 4;
constexpr int kColStep = 57;
constexpr int kRowStep = 49;

constexpr double kFramePeriodMs = 16.0;
constexpr std::uint64_t kClockBlock = 256;

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_shell = nullptr;
SDL_Texture* g_fb = nullptr;

int g_fbW = 240;
int g_fbH = 135;
std::vector<std::uint32_t> g_argb;
std::vector<std::int16_t> g_audioPcm(4096);

bool g_booted = false;
double g_nextFrameMs = 0.0;
std::uint32_t g_frameCounter = 0;
std::uint64_t g_renderedFrames = 0;

bool g_keyDown[kKeyRows][kKeyCols] = {};

using RowCol = std::pair<int, int>;

// Computer keyboard -> Cardenza 4x14 matrix, by physical key (the same map cardenza-web.js uses).
RowCol MapScancode(SDL_Scancode sc, bool& isHome) {
  isHome = false;
  switch (sc) {
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_GRAVE:
      return {0, 0};
    case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3: case SDL_SCANCODE_4:
    case SDL_SCANCODE_5: case SDL_SCANCODE_6: case SDL_SCANCODE_7: case SDL_SCANCODE_8:
    case SDL_SCANCODE_9: case SDL_SCANCODE_0:
      return {0, 1 + (sc - SDL_SCANCODE_1)};
    case SDL_SCANCODE_MINUS: return {0, 11};
    case SDL_SCANCODE_EQUALS: return {0, 12};
    case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_DELETE: return {0, 13};
    case SDL_SCANCODE_TAB: return {1, 0};
    case SDL_SCANCODE_Q: case SDL_SCANCODE_W: case SDL_SCANCODE_E: case SDL_SCANCODE_R:
    case SDL_SCANCODE_T: case SDL_SCANCODE_Y: case SDL_SCANCODE_U: case SDL_SCANCODE_I:
    case SDL_SCANCODE_O: case SDL_SCANCODE_P:
      return {1, 1 + (sc - SDL_SCANCODE_Q)};
    case SDL_SCANCODE_LEFTBRACKET: return {1, 11};
    case SDL_SCANCODE_RIGHTBRACKET: return {1, 12};
    case SDL_SCANCODE_BACKSLASH: return {1, 13};
    case SDL_SCANCODE_CAPSLOCK: case SDL_SCANCODE_LSHIFT: return {2, 0};
    case SDL_SCANCODE_RSHIFT: return {2, 1};
    case SDL_SCANCODE_A: case SDL_SCANCODE_S: case SDL_SCANCODE_D: case SDL_SCANCODE_F:
    case SDL_SCANCODE_G: case SDL_SCANCODE_H: case SDL_SCANCODE_J: case SDL_SCANCODE_K:
    case SDL_SCANCODE_L:
      return {2, 2 + (sc - SDL_SCANCODE_A)};
    case SDL_SCANCODE_SEMICOLON: return {2, 11};
    case SDL_SCANCODE_APOSTROPHE: return {2, 12};
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return {2, 13};
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return {3, 0};
    case SDL_SCANCODE_LALT: return {3, 1};
    case SDL_SCANCODE_RALT: return {3, 2};
    case SDL_SCANCODE_Z: case SDL_SCANCODE_X: case SDL_SCANCODE_C: case SDL_SCANCODE_V:
    case SDL_SCANCODE_B: case SDL_SCANCODE_N: case SDL_SCANCODE_M:
      return {3, 3 + (sc - SDL_SCANCODE_Z)};
    case SDL_SCANCODE_COMMA: return {3, 10};
    case SDL_SCANCODE_PERIOD: return {3, 11};
    case SDL_SCANCODE_SLASH: return {3, 12};
    case SDL_SCANCODE_SPACE: return {3, 13};
    case SDL_SCANCODE_HOME: isHome = true; return {0, 0};
    default: return {-1, -1};
  }
}

void PumpSequencerClock() {
  if (auto* host = dynamic_cast<groove::Host*>(os::system::ActiveApplication())) {
    const std::uint64_t micros = g_renderedFrames * 1000000ULL / synth::audio::kSampleRate;
    host->Sequencer().PumpClock(micros);
  }
}

// Web Audio pulls float samples from here (called from the page's ScriptProcessor).
extern "C" EMSCRIPTEN_KEEPALIVE void cp0shell_render(float* dest, int frames) {
  if (frames <= 0) return;
  if (frames > 4096) frames = 4096;
  if (!g_booted) {
    for (int i = 0; i < frames; ++i) dest[i] = 0.0f;
    return;
  }
  int done = 0;
  while (done < frames) {
    const int offset = static_cast<int>(g_renderedFrames % kClockBlock);
    const int length = std::min(frames - done, static_cast<int>(kClockBlock) - offset);
    if (offset == 0) PumpSequencerClock();
    synth::audio::RenderFrames(g_audioPcm.data() + done, static_cast<std::size_t>(length));
    done += length;
    g_renderedFrames += static_cast<std::uint64_t>(length);
  }
  const float gain = static_cast<float>(Device::Audio::OutputVolume()) / (100.0f * 32768.0f);
  for (int i = 0; i < frames; ++i) dest[i] = static_cast<float>(g_audioPcm[i]) * gain;
}

SDL_Texture* LoadPng(const char* path) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, path)) {
    std::fprintf(stderr, "CP0Shell: cannot read %s: %s\n", path, image.message);
    return nullptr;
  }
  image.format = PNG_FORMAT_RGBA;
  std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
    std::fprintf(stderr, "CP0Shell: cannot decode %s: %s\n", path, image.message);
    png_image_free(&image);
    return nullptr;
  }
  auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(
      pixels.data(), static_cast<int>(image.width), static_cast<int>(image.height), 32,
      static_cast<int>(image.width * 4U), SDL_PIXELFORMAT_RGBA32);
  auto* texture = surface ? SDL_CreateTextureFromSurface(g_renderer, surface) : nullptr;
  if (surface) SDL_FreeSurface(surface);
  png_image_free(&image);
  return texture;
}

void ConvertFrame() {
  const auto& fb = Device::Display::CurrentFrameBuffer();
  const std::uint16_t* src = fb.pixels;
  g_argb.resize(static_cast<size_t>(g_fbW) * g_fbH);
  for (int i = 0; i < g_fbW * g_fbH; ++i) {
    const std::uint16_t c = src[i];
    const std::uint32_t r = (c >> 11) & 0x1F;
    const std::uint32_t g = (c >> 5) & 0x3F;
    const std::uint32_t b = c & 0x1F;
    g_argb[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
  }
}

void RunDeviceFrame() {
  os::system::RunFrame();
  ConvertFrame();
  SDL_UpdateTexture(g_fb, nullptr, g_argb.data(), g_fbW * static_cast<int>(sizeof(std::uint32_t)));
  ++g_frameCounter;
}

void Render() {
  SDL_SetRenderDrawColor(g_renderer, 0x20, 0x21, 0x24, 0xFF);
  SDL_RenderClear(g_renderer);
  if (g_shell) SDL_RenderCopy(g_renderer, g_shell, nullptr, nullptr);

  // Pressed key highlight over the (statically drawn) keycaps in the shell art.
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  for (int r = 0; r < kKeyRows; ++r) {
    for (int c = 0; c < kKeyCols; ++c) {
      if (!g_keyDown[r][c]) continue;
      SDL_SetRenderDrawColor(g_renderer, 140, 200, 255, 90);
      SDL_Rect kr{kKeyX + c * kColStep, kKeyY + r * kRowStep, kKeyW, kKeyH};
      SDL_RenderFillRect(g_renderer, &kr);
    }
  }

  const SDL_Rect screen{kScreenX, kScreenY, kScreenW, kScreenH};
  if (g_fb) SDL_RenderCopy(g_renderer, g_fb, nullptr, &screen);
  SDL_RenderPresent(g_renderer);
}

void StartAudioOnGesture() {
  EM_ASM({
    if (typeof Module.cp0Audio === 'function') Module.cp0Audio();
  });
}

void SetMatrix(int row, int column, bool pressed) {
  if (row < 0 || row >= 4 || column < 0 || column >= 14) return;
  CardenzaSim::Input::SetMatrixKey(static_cast<std::size_t>(row), static_cast<std::size_t>(column), pressed);
  RunDeviceFrame();
  Render();
}

void HandleKey(const SDL_KeyboardEvent& key) {
  if (key.repeat) return;
  StartAudioOnGesture();
  bool isHome = false;
  const RowCol rc = MapScancode(key.keysym.scancode, isHome);
  if (isHome) {
    if (g_booted) {
      CardenzaSim::Input::SetHomeButton(key.type == SDL_KEYDOWN);
      RunDeviceFrame();
      Render();
    }
    return;
  }
  if (rc.first < 0) return;
  SetMatrix(rc.first, rc.second, key.type == SDL_KEYDOWN);
}

void HandleButton(const SDL_MouseButtonEvent& button) {
  if (button.button != SDL_BUTTON_LEFT) return;
  StartAudioOnGesture();
  float lx = 0.0f, ly = 0.0f;
  SDL_RenderWindowToLogical(g_renderer, button.x, button.y, &lx, &ly);
  const int x = static_cast<int>(lx);
  const int y = static_cast<int>(ly);
  // Keypad region -> device key -> Cardenza matrix (11 cols -> nearest 4x14 column).
  if (y >= kKeyY && y < kKeyY + kKeyRows * kRowStep && x >= kKeyX) {
    const int devc = (x - kKeyX) / kColStep;
    const int row = (y - kKeyY) / kRowStep;
    if (row >= 0 && row < kKeyRows && devc >= 0 && devc < kKeyCols) {
      const int col = std::min(13, devc);
      const bool pressed = button.type == SDL_MOUSEBUTTONDOWN;
      g_keyDown[row][devc] = pressed;
      SetMatrix(row, col, pressed);
    }
  }
}

void MainLoopTick() {
  const double now = emscripten_get_now();
  if (now + 4.0 < g_nextFrameMs) {
    return;
  }
  g_nextFrameMs += kFramePeriodMs;
  if (g_nextFrameMs < now) g_nextFrameMs = now + kFramePeriodMs;

  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        HandleKey(event.key);
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        HandleButton(event.button);
        break;
      default:
        break;
    }
  }

  RunDeviceFrame();
  Render();
}

bool Init() {
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest, matches BuildPresented320
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "CP0Shell: SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  g_window = SDL_CreateWindow("Cardenza", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kDevW,
                              kDevH, SDL_WINDOW_ALLOW_HIGHDPI);
  if (!g_window) {
    std::fprintf(stderr, "CP0Shell: SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
  if (!g_renderer) {
    std::fprintf(stderr, "CP0Shell: SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_RenderSetLogicalSize(g_renderer, kDevW, kDevH);

  g_shell = LoadPng("/cp0-shell-light.png");
  if (!g_shell) {
    std::fprintf(stderr, "CP0Shell: shell art missing\n");
    return false;
  }

  g_fbW = Device::Display::kWidth;
  g_fbH = Device::Display::kHeight;
  g_fb = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                           g_fbW, g_fbH);
  if (!g_fb) {
    std::fprintf(stderr, "CP0Shell: framebuffer texture failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetTextureScaleMode(g_fb, SDL_ScaleModeNearest);

  return true;
}

}  // namespace

int main() {
  if (!Init()) return 1;
  if (!os::system::Begin()) {
    std::fprintf(stderr, "CP0Shell: failed to boot the Cardenza runtime\n");
    return 1;
  }
  g_booted = true;
  std::printf("CP0Shell: %s [%s] %dx%d screen %dx%d\n", Device::Name().c_str(),
              Device::BackendName().c_str(), g_fbW, g_fbH, kScreenW, kScreenH);

  RunDeviceFrame();
  Render();

  // Web Audio output. The AudioContext must be created AND resumed inside a real DOM gesture
  // handler: SDL defers key/mouse handling to the rAF poll, which loses the gesture and the
  // browser autoplay policy then refuses to start audio. So arm DOM listeners here; a
  // ScriptProcessor pulls float samples from cp0shell_render at the engine's 44.1 kHz.
  EM_ASM({
    Module.cp0AudioBlocks = 0;
    Module.cp0Audio = function() {
      if (Module.cp0ACtx) { if (Module.cp0ACtx.state !== 'running') Module.cp0ACtx.resume(); return; }
      try {
        var Ctor = window.AudioContext || window.webkitAudioContext;
        Module.cp0ACtx = new Ctor({ sampleRate: 44100, latencyHint: 'interactive' });
        var n = 1024;
        Module.cp0Sp = Module.cp0ACtx.createScriptProcessor(n, 0, 1);
        Module.cp0APtr = _malloc(n * 4);
        Module.cp0Sp.onaudioprocess = function(e) {
          try {
            var out = e.outputBuffer.getChannelData(0);
            var len = out.length;
            _cp0shell_render(Module.cp0APtr, len);
            out.set(HEAPF32.subarray(Module.cp0APtr >> 2, (Module.cp0APtr >> 2) + len));
            var mxp = 0; for (var pk = 0; pk < len; pk++) { var av = Math.abs(out[pk]); if (av > mxp) mxp = av; } Module.cp0AudioPeak = mxp;
            Module.cp0AudioBlocks = (Module.cp0AudioBlocks | 0) + 1;
          } catch (err) { Module.cp0AudioErr = String(err); }
        };
        Module.cp0Sp.connect(Module.cp0ACtx.destination);
        Module.cp0ACtx.resume();
        console.log('CP0Shell: audio ' + Module.cp0ACtx.state + ' @ ' + Module.cp0ACtx.sampleRate);
      } catch (err) { Module.cp0AudioErr = String(err); console.error('CP0Shell audio error', err); }
    };
    window.addEventListener('pointerdown', function() { try { Module.cp0Audio(); } catch (e) {} }, { passive: true });
    window.addEventListener('touchstart', function() { try { Module.cp0Audio(); } catch (e) {} }, { passive: true });
    window.addEventListener('keydown', function() { try { Module.cp0Audio(); } catch (e) {} }, { passive: true });
    window.addEventListener('mousedown', function() { try { Module.cp0Audio(); } catch (e) {} }, { passive: true });
  });

  g_nextFrameMs = emscripten_get_now();
  emscripten_set_main_loop(MainLoopTick, 0, true);
  return 0;
}
