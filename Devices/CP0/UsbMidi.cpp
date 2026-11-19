#include "Devices/CP0/UsbMidi.h"

#include "Devices/CP0/Input.h"

#include <alsa/asoundlib.h>

#include <cerrno>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace CP0 {
namespace UsbMidi {
namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_run{true};

bool DebugOn() {
  static const bool on = std::getenv("CP0_USBMIDI_DEBUG") != nullptr;
  return on;
}

// Scan /proc/asound/cards for the USB-Audio MIDI card; return its card index or -1.
int FindUsbMidiCard() {
  FILE* f = std::fopen("/proc/asound/cards", "r");
  if (!f) return -1;
  char line[256];
  int cur = -1, usb = -1;
  while (std::fgets(line, sizeof line, f)) {
    int idx = -1;
    if (std::sscanf(line, " %d [", &idx) == 1) cur = idx;  // " 2 [SMK25Mini ]: USB-Audio - ..."
    if (std::strstr(line, "USB-Audio") || std::strstr(line, "SMK25") ||
        std::strstr(line, "SINCO") || std::strstr(line, "USB MIDI") ||
        std::strstr(line, "USB-MIDI")) {
      if (cur >= 0) usb = cur;
    }
  }
  std::fclose(f);
  return usb;
}

// The panel backlight and framebuffer blank are the device's, not a userspace daemon's: the system
// "screen off" timer only sees physical console input, so a keyboard played over USB never wakes the
// panel. These keep the panel on while the keyboard is played (WakePanel is declared in the header).
int ReadSysInt(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  int v = -1;
  if (std::fscanf(f, "%d", &v) != 1) v = -1;
  std::fclose(f);
  return v;
}

void WriteSysInt(const char* path, int value) {
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "%d\n", value);
  std::fclose(f);
}

// Throttled so a fast run of notes opens sysfs at most twice a second.
void PokeWake() {
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return;
  last = now;
  WakePanel();
}

// Running-status MIDI 1.0 byte parser -> 4-byte USB-MIDI event packets fed to the firmware queue.
struct Parser {
  std::uint8_t status = 0;
  std::uint8_t data[2] = {0, 0};
  int ndata = 0;

  void Emit() {
    const std::uint8_t hi = status & 0xF0;
    const std::uint8_t d2 = (hi == 0xC0 || hi == 0xD0) ? 0 : data[1];
    const std::uint8_t packet[4] = {
        static_cast<std::uint8_t>(status >> 4), status, data[0], d2};
    if (DebugOn() && (hi == 0x90 || hi == 0x80 || hi == 0xB0)) {
      std::fprintf(stderr, "[usbmidi] %02X %02X %02X\n", status, data[0], d2);
    }
    PokeWake();  // a MIDI note is activity: wake the panel and keep it on while the keyboard plays
    CP0::Input::SendComputerMidi(packet);
  }

  void Feed(std::uint8_t b) {
    if (b & 0x80) {
      if (b >= 0xF8) return;                 // realtime (clock/start/stop): ignore
      if (b >= 0xF0) { status = 0; ndata = 0; return; }  // system common / sysex: drop
      status = b;
      ndata = 0;
      return;
    }
    if (status == 0) return;
    if (ndata < 2) data[ndata++] = b;
    const std::uint8_t hi = status & 0xF0;
    const int need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    if (ndata >= need) {
      Emit();
      ndata = 0;  // running status: keep `status`
    }
  }
};

void Run() {
  Parser parser;
  snd_rawmidi_t* in = nullptr;
  while (g_run.load()) {
    if (!in) {
      const int card = FindUsbMidiCard();
      if (card < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      char name[32];
      std::snprintf(name, sizeof name, "hw:%d,0,0", card);
      if (snd_rawmidi_open(&in, nullptr, name, SND_RAWMIDI_NONBLOCK) < 0 || !in) {
        in = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        continue;
      }
      parser = Parser{};
      CP0::Input::SetComputerConnected(true);
      std::fprintf(stderr, "[usbmidi] opened %s\n", name);
    }
    unsigned char buf[256];
    const ssize_t n = snd_rawmidi_read(in, buf, sizeof buf);
    if (n == -EAGAIN || n == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (n < 0) {
      CP0::Input::SetComputerConnected(false);
      snd_rawmidi_close(in);
      in = nullptr;
      std::fprintf(stderr, "[usbmidi] read error (%ld), rescanning\n", static_cast<long>(n));
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      continue;
    }
    for (ssize_t i = 0; i < n; ++i) parser.Feed(buf[i]);
  }
  if (in) {
    CP0::Input::SetComputerConnected(false);
    snd_rawmidi_close(in);
  }
}

}  // namespace

void WakePanel() {
  // Un-blank the framebuffer if the system powered the panel down (root-only file: ignore failure).
  if (ReadSysInt("/sys/class/graphics/fb0/blank") > 0) WriteSysInt("/sys/class/graphics/fb0/blank", 0);
  // Backlight 0 means the panel is off: restore full brightness (pi is in the video group).
  const int max = ReadSysInt("/sys/class/backlight/backlight/max_brightness");
  if (max > 0 && ReadSysInt("/sys/class/backlight/backlight/brightness") == 0)
    WriteSysInt("/sys/class/backlight/backlight/brightness", max);
}

void Start() {
  if (g_started.exchange(true)) return;
  g_run.store(true);
  std::thread(Run).detach();
}

void Stop() { g_run.store(false); }

}  // namespace UsbMidi
}  // namespace CP0
