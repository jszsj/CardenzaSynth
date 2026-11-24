#include "Devices/CP0/Input.h"
#include "Devices/Device.h"
#include "Devices/DeviceAudio.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include <array>
#include <chrono>
#include <thread>
#endif

// Input noise cleanup toggle (declared in Devices/Device.h). Kept at file scope so the shared Groove
// UI links on every target; the ALSA capture backend below is what actually shapes the stream.
static bool CleanupDefaultFromEnv()
{
  const char* env = std::getenv("CARDENZA_INPUT_CLEAN");
  return !(env != nullptr && env[0] == '0');
}
bool g_inputNoiseCleanup = CleanupDefaultFromEnv();
void Device::Audio::SetInputNoiseCleanup(bool enabled) { g_inputNoiseCleanup = enabled; }
bool Device::Audio::InputNoiseCleanup() { return g_inputNoiseCleanup; }

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <miniaudio.h>  // CP0 capture backend: miniaudio -> libpulse0 -> PipeWire (see third_party/miniaudio_impl.c)

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#endif

namespace {

device::SynthAudioRenderProvider g_audioProvider{};
std::mutex g_audioProviderMutex;
bool g_audioInputEnabled = false;
Device::Audio::InputStreamInfo g_inputStreamInfo{};
// The microphone: the host's own (Windows wave-in) until a test picks a simulated one or none.
enum class Microphone { Host, Simulated, Absent };
Microphone g_microphone = Microphone::Host;
std::size_t g_simulatedMicrophoneFrames = 0;
std::uint32_t g_simulatedMicrophonePhase = 0;
std::atomic<std::uint8_t> g_outputVolume{Device::Audio::kDefaultOutputVolume};

device::SynthAudioRenderProvider CurrentAudioProvider()
{
  std::lock_guard<std::mutex> lock(g_audioProviderMutex);
  return g_audioProvider;
}

std::size_t RenderSynthFrames(std::int16_t* destination, std::size_t frameCount)
{
  const device::SynthAudioRenderProvider provider = CurrentAudioProvider();
  if (provider.renderFrames == nullptr)
  {
    std::fill(destination, destination + frameCount, 0);
    return frameCount;
  }

  return provider.renderFrames(destination, frameCount);
}

#if defined(_WIN32)

constexpr std::size_t kWaveBufferFrameCount = 512;
constexpr std::size_t kWaveInputMaxChannels = 2;
constexpr std::size_t kWaveBufferCount = 4;
std::mutex g_waveInCaptureMutex;

// Same integer scaling as the firmware output task; 100% leaves the samples untouched.
void ApplyOutputVolume(std::int16_t* samples, std::size_t sampleCount)
{
  const std::int32_t volume = g_outputVolume.load(std::memory_order_relaxed);
  if (volume >= 100)
  {
    return;
  }

  const std::int32_t gain = (volume * 32768 + 50) / 100;
  for (std::size_t index = 0; index < sampleCount; ++index)
  {
    samples[index] = static_cast<std::int16_t>((static_cast<std::int32_t>(samples[index]) * gain) / 32768);
  }
}

class WaveOutPump {
public:
  bool Start()
  {
    if (running_)
    {
      return true;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = CurrentAudioProvider().sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
      device_ = nullptr;
      return false;
    }

    for (std::size_t index = 0; index < kWaveBufferCount; ++index)
    {
      headers_[index] = {};
      headers_[index].lpData = reinterpret_cast<LPSTR>(buffers_[index].data());
      headers_[index].dwBufferLength = static_cast<DWORD>(buffers_[index].size() * sizeof(std::int16_t));
      if (waveOutPrepareHeader(device_, &headers_[index], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
      {
        Stop();
        return false;
      }

      RefillBuffer(index);
      if (waveOutWrite(device_, &headers_[index], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
      {
        Stop();
        return false;
      }
    }

    running_ = true;
    thread_ = std::thread([this]() {
      Run();
    });
    return true;
  }

  ~WaveOutPump()
  {
    Stop();
  }

  bool IsRunning() const
  {
    return running_;
  }

private:
  void RefillBuffer(std::size_t index)
  {
    RenderSynthFrames(buffers_[index].data(), buffers_[index].size());
    ApplyOutputVolume(buffers_[index].data(), buffers_[index].size());
    headers_[index].dwFlags &= ~WHDR_DONE;
    headers_[index].dwBufferLength = static_cast<DWORD>(buffers_[index].size() * sizeof(std::int16_t));
  }

  void Run()
  {
    while (running_)
    {
      bool refilledAny = false;
      for (std::size_t index = 0; index < kWaveBufferCount; ++index)
      {
        if ((headers_[index].dwFlags & WHDR_DONE) == 0)
        {
          continue;
        }

        RefillBuffer(index);
        if (waveOutWrite(device_, &headers_[index], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
        {
          refilledAny = true;
        }
      }

      if (!refilledAny)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }

  void Stop()
  {
    if (!device_)
    {
      return;
    }

    running_ = false;
    if (thread_.joinable())
    {
      thread_.join();
    }

    waveOutReset(device_);
    for (WAVEHDR& header : headers_)
    {
      if (header.lpData != nullptr)
      {
        waveOutUnprepareHeader(device_, &header, sizeof(WAVEHDR));
      }
    }
    waveOutClose(device_);
    device_ = nullptr;
  }

  HWAVEOUT device_ = nullptr;
  std::array<std::array<std::int16_t, kWaveBufferFrameCount>, kWaveBufferCount> buffers_{};
  std::array<WAVEHDR, kWaveBufferCount> headers_{};
  std::thread thread_{};
  bool running_ = false;
};

class WaveInCapture {
public:
  bool Start()
  {
    if (running_)
    {
      return true;
    }

    WAVEFORMATEX format{};
    const std::array<std::pair<DWORD, WORD>, 4> formatCandidates = {{
        {Device::Audio::kDefaultInputSampleRate, 1},
        {48000, 1},
        {48000, 2},
        {Device::Audio::kDefaultInputSampleRate, 2},
    }};
    for (const auto& candidate : formatCandidates)
    {
      format = {};
      format.wFormatTag = WAVE_FORMAT_PCM;
      format.nChannels = candidate.second;
      format.nSamplesPerSec = candidate.first;
      format.wBitsPerSample = Device::Audio::kInputBitsPerSample;
      format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
      format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

      if (waveInOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
      {
        inputChannelCount_ = format.nChannels;
        inputSampleRate_ = format.nSamplesPerSec;
        break;
      }
    }

    if (device_ == nullptr)
    {
      return false;
    }

    g_inputStreamInfo.sampleRate = inputSampleRate_;
    g_inputStreamInfo.channelCount = 1;
    g_inputStreamInfo.bitsPerSample = Device::Audio::kInputBitsPerSample;

    for (std::size_t index = 0; index < kWaveBufferCount; ++index)
    {
      headers_[index] = {};
      headers_[index].lpData = reinterpret_cast<LPSTR>(buffers_[index].data());
      headers_[index].dwBufferLength = static_cast<DWORD>(kWaveBufferFrameCount * inputChannelCount_ * sizeof(std::int16_t));
      if (waveInPrepareHeader(device_, &headers_[index], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
      {
        Stop();
        return false;
      }

      if (waveInAddBuffer(device_, &headers_[index], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
      {
        Stop();
        return false;
      }
    }

    if (waveInStart(device_) != MMSYSERR_NOERROR)
    {
      Stop();
      return false;
    }

    running_ = true;
    return true;
  }

  ~WaveInCapture()
  {
    Stop();
  }

  bool IsRunning() const
  {
    return running_;
  }

  void Stop()
  {
    if (!device_)
    {
      return;
    }

    running_ = false;
    waveInStop(device_);
    waveInReset(device_);

    {
      std::lock_guard<std::mutex> lock(dataMutex_);
      capturedFrames_.clear();
    }

    for (WAVEHDR& header : headers_)
    {
      if (header.lpData != nullptr)
      {
        waveInUnprepareHeader(device_, &header, sizeof(WAVEHDR));
      }
    }

    waveInClose(device_);
    device_ = nullptr;
  }

  Device::Audio::InputStatus ReadFrames(std::int16_t* destination,
                                        std::size_t frameCount,
                                        std::size_t& framesRead,
                                        std::uint32_t timeoutMs)
  {
    framesRead = 0;
    if (!running_)
    {
      return Device::Audio::InputStatus::NotReady;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
      HarvestCompletedBuffers();
      {
        std::lock_guard<std::mutex> lock(dataMutex_);
        const std::size_t availableFrames = capturedFrames_.size();
        if (availableFrames != 0 || timeoutMs == 0)
        {
          framesRead = std::min(frameCount, availableFrames);
          for (std::size_t index = 0; index < framesRead; ++index)
          {
            destination[index] = capturedFrames_.front();
            capturedFrames_.pop_front();
          }
          return Device::Audio::InputStatus::Ready;
        }
      }

      if (std::chrono::steady_clock::now() >= deadline)
      {
        return Device::Audio::InputStatus::Ready;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

private:
  void HarvestCompletedBuffers()
  {
    if (!device_)
    {
      return;
    }

    for (std::size_t index = 0; index < kWaveBufferCount; ++index)
    {
      if ((headers_[index].dwFlags & WHDR_DONE) == 0)
      {
        continue;
      }

      const std::size_t capturedSampleCount = static_cast<std::size_t>(headers_[index].dwBytesRecorded / sizeof(std::int16_t));
      if (capturedSampleCount > 0)
      {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (inputChannelCount_ <= 1)
        {
          capturedFrames_.insert(capturedFrames_.end(),
                                 buffers_[index].begin(),
                                 buffers_[index].begin() + static_cast<std::ptrdiff_t>(capturedSampleCount));
        }
        else
        {
          const std::size_t capturedFrameCount = capturedSampleCount / inputChannelCount_;
          for (std::size_t frame = 0; frame < capturedFrameCount; ++frame)
          {
            std::int32_t sum = 0;
            for (std::size_t channel = 0; channel < inputChannelCount_; ++channel)
            {
              sum += buffers_[index][frame * inputChannelCount_ + channel];
            }
            capturedFrames_.push_back(static_cast<std::int16_t>(sum / static_cast<std::int32_t>(inputChannelCount_)));
          }
        }
      }

      headers_[index].dwBytesRecorded = 0;
      headers_[index].dwFlags &= ~WHDR_DONE;
      if (running_ && waveInAddBuffer(device_, &headers_[index], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
      {
        running_ = false;
        return;
      }
    }
  }

  HWAVEIN device_ = nullptr;
  std::array<std::array<std::int16_t, kWaveBufferFrameCount * kWaveInputMaxChannels>, kWaveBufferCount> buffers_{};
  std::array<WAVEHDR, kWaveBufferCount> headers_{};
  std::mutex dataMutex_;
  std::deque<std::int16_t> capturedFrames_{};
  std::size_t inputChannelCount_ = 1;
  std::uint32_t inputSampleRate_ = Device::Audio::kDefaultInputSampleRate;
  bool running_ = false;
};

std::unique_ptr<WaveInCapture>& WaveInCaptureInstance()
{
  static std::unique_ptr<WaveInCapture> capture;
  return capture;
}

bool EnsureWaveOutPumpStarted()
{
  static std::mutex mutex;
  static std::unique_ptr<WaveOutPump> pump;
  std::lock_guard<std::mutex> lock(mutex);

  if (pump && pump->IsRunning())
  {
    return true;
  }

  pump = std::make_unique<WaveOutPump>();
  if (!pump->Start())
  {
    pump.reset();
    return false;
  }

  return true;
}

bool EnsureWaveInCaptureStarted()
{
  std::unique_ptr<WaveInCapture>& capture = WaveInCaptureInstance();
  std::lock_guard<std::mutex> lock(g_waveInCaptureMutex);

  if (capture && capture->IsRunning())
  {
    return true;
  }

  capture = std::make_unique<WaveInCapture>();
  if (!capture->Start())
  {
    capture.reset();
    return false;
  }

  return true;
}

void StopWaveInCapture()
{
  std::unique_ptr<WaveInCapture>& capture = WaveInCaptureInstance();
  std::lock_guard<std::mutex> lock(g_waveInCaptureMutex);
  if (!capture)
  {
    return;
  }

  capture->Stop();
  capture.reset();
}

Device::Audio::InputStatus ReadWaveInFrames(std::int16_t* destination,
                                            std::size_t frameCount,
                                            std::size_t& framesRead,
                                            std::uint32_t timeoutMs)
{
  std::unique_ptr<WaveInCapture>& capture = WaveInCaptureInstance();
  std::lock_guard<std::mutex> lock(g_waveInCaptureMutex);
  if (!capture)
  {
    framesRead = 0;
    return Device::Audio::InputStatus::NotReady;
  }

  return capture->ReadFrames(destination, frameCount, framesRead, timeoutMs);
}

#elif defined(__linux__) && !defined(__EMSCRIPTEN__)

// Linux on-device capture (CP0): the ES8389 codec's capture side. The output is streamed by the app
// (CP0Main's ALSA thread), so the output pump stays idle here; input opens the codec directly and
// hands mono frames (downmixed from stereo when the codec only does stereo) to the sampler.
std::mutex g_inCaptureMutex;
bool g_captureGainApplied = false;

// miniaudio capture backend (PulseAudio). The capture callback downmixes to mono int16 and pushes
// into g_inRing; the sampler's UI-thread PumpInput drains it non-blockingly via ReadWaveInFrames
// (never waits on the audio device). No libasound2 / pipewire-alsa needed.
std::mutex g_inRingMutex;
std::deque<std::int16_t> g_inRing;
ma_context g_capContext{};
ma_device g_capDevice{};
bool g_capContextInit = false;
bool g_capDeviceInit = false;

// Capture cleanup DSP state: a 1st-order DC-blocking high-pass (~80 Hz — removes the DC offset and
// low rumble with minimal transient overshoot; a 2nd-order HPF overshot to clipping on sharp loud
// onsets) plus a soft noise gate (hysteresis + smoothed gain) that attenuates the hiss floor while no
// signal is present. Re-initialised whenever capture opens so a new take starts clean.
double g_hpfCoef = 0.0;                        // a = exp(-2*pi*fc/rate)
double g_hpfX1 = 0.0, g_hpfY1 = 0.0;
double g_gateEnv = 0.0, g_gateGain = 1.0;
bool g_gateOpen = false;
bool g_cleanupReady = false;

void ResetInputCleanup(unsigned rate)
{
  constexpr double kPi = 3.14159265358979323846;
  const double fc = 80.0;
  g_hpfCoef = std::exp(-2.0 * kPi * fc / static_cast<double>(rate));
  g_hpfX1 = 0.0;
  g_hpfY1 = 0.0;
  g_gateEnv = 0.0;
  g_gateGain = 1.0;
  g_gateOpen = false;
  g_cleanupReady = true;
}

// In-place shaping of the captured mono frames. Thresholds are int16 units, set from real cp002
// measurements: the ES8389 noise PEAK is ~4216 (-17.8 dBFS), signal peak ~16942 (-5.7 dBFS) — only
// ~12 dB apart — so the gate opens above the noise (~7000, -13.4 dBFS) and closes below (~5500,
// -15.4 dBFS) with hysteresis. A soft limiter guards the ~-0.5 dBFS ceiling so loud onsets never clip.
void ApplyInputCleanup(std::int16_t* buf, std::size_t n, unsigned rate)
{
  if (!g_inputNoiseCleanup)
  {
    return;
  }
  if (!g_cleanupReady)
  {
    ResetInputCleanup(rate);
  }
  const double thrOpen = 7000.0;   // ~ -13.4 dBFS
  const double thrClose = 5500.0;  // ~ -15.4 dBFS
  const double closedGain = 0.08;  // ~ -22 dB when the gate is shut
  const double envRelease = 1.0 - std::exp(-1.0 / (0.050 * static_cast<double>(rate)));  // 50 ms
  const double gainAttack = 1.0 - std::exp(-1.0 / (0.002 * static_cast<double>(rate)));  // 2 ms
  const double gainRelease = 1.0 - std::exp(-1.0 / (0.150 * static_cast<double>(rate))); // 150 ms
  const double knee = 31000.0;                       // ~ -0.5 dBFS: only extreme overshoot is limited
  const double headroom = 32767.0 - knee;
  for (std::size_t i = 0; i < n; ++i)
  {
    const double x = static_cast<double>(buf[i]);
    const double y = g_hpfCoef * (g_hpfY1 + x - g_hpfX1);  // 1st-order DC-block high-pass
    g_hpfX1 = x;
    g_hpfY1 = y;
    const double ay = y < 0.0 ? -y : y;
    if (ay > g_gateEnv) g_gateEnv = ay; else g_gateEnv += (ay - g_gateEnv) * envRelease;
    if (g_gateOpen)
    {
      if (g_gateEnv < thrClose) g_gateOpen = false;
    }
    else if (g_gateEnv > thrOpen)
    {
      g_gateOpen = true;
    }
    const double target = g_gateOpen ? 1.0 : closedGain;
    const double coef = target > g_gateGain ? gainAttack : gainRelease;
    g_gateGain += (target - g_gateGain) * coef;
    double out = y * g_gateGain;
    if (out > knee) out = knee + headroom * std::tanh((out - knee) / headroom);
    else if (out < -knee) out = -knee - headroom * std::tanh((-out - knee) / headroom);
    if (out > 32767.0) out = 32767.0; else if (out < -32768.0) out = -32768.0;
    buf[i] = static_cast<std::int16_t>(out);
  }
}

namespace {
// miniaudio capture callback (PulseAudio thread): downmix any channels to mono int16 and push into
// the ring. Capped at ~2 s so a stalled UI cannot grow memory without bound.
void CaptureCallback(ma_device* dev, void* output, const void* input, ma_uint32 frameCount)
{
  (void)output;
  const std::int16_t* in = static_cast<const std::int16_t*>(input);
  if (in == nullptr) return;
  const ma_uint32 ch = (dev && dev->capture.channels) ? dev->capture.channels : 1u;
  const std::size_t kMax = static_cast<std::size_t>(Device::Audio::kDefaultInputSampleRate) * 2;  // ~2 s
  std::lock_guard<std::mutex> lock(g_inRingMutex);
  for (ma_uint32 i = 0; i < frameCount; ++i)
  {
    std::int32_t s;
    if (ch <= 1u) { s = in[i]; }
    else { std::int32_t sum = 0; for (ma_uint32 c = 0; c < ch; ++c) sum += in[i * ch + c]; s = sum / static_cast<std::int32_t>(ch); }
    g_inRing.push_back(static_cast<std::int16_t>(s));
  }
  while (g_inRing.size() > kMax) g_inRing.pop_front();
}
}  // namespace

bool EnsureWaveOutPumpStarted()
{
  return false;  // output is streamed by the app (CP0Main)
}

bool EnsureWaveInCaptureStarted()
{
  std::lock_guard<std::mutex> lock(g_inCaptureMutex);
  if (g_capDeviceInit)
  {
    return true;
  }
  if (!g_captureGainApplied)
  {
    // The analog mic (AMIC) is quiet; set a fixed capture gain once per process so sampling does not
    // depend on the boot-time mixer state. numids are stable for the ES8389 codec (PGA L/R + capture
    // L/R + ALC off for clean manual gain).
    std::system("amixer -c1 cset numid=4 14 >/dev/null 2>&1; "
                "amixer -c1 cset numid=5 14 >/dev/null 2>&1; "
                "amixer -c1 cset numid=2 255 >/dev/null 2>&1; "
                "amixer -c1 cset numid=3 255 >/dev/null 2>&1; "
                "amixer -c1 cset numid=8 0 >/dev/null 2>&1");
    g_captureGainApplied = true;
  }
  ma_backend backends[] = {ma_backend_pulseaudio};
  if (ma_context_init(backends, 1, nullptr, &g_capContext) != MA_SUCCESS)
  {
    return false;
  }
  g_capContextInit = true;
  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.format = ma_format_s16;
  cfg.capture.channels = 1;  // request mono; PulseAudio / PipeWire downmixes the codec's stereo
  cfg.sampleRate = static_cast<ma_uint32>(Device::Audio::kDefaultInputSampleRate);
  cfg.periodSizeInMilliseconds = 20;
  cfg.dataCallback = &CaptureCallback;
  if (ma_device_init(&g_capContext, &cfg, &g_capDevice) != MA_SUCCESS)
  {
    ma_context_uninit(&g_capContext);
    g_capContextInit = false;
    return false;
  }
  g_capDeviceInit = true;
  {
    std::lock_guard<std::mutex> ringLock(g_inRingMutex);
    g_inRing.clear();
  }
  if (ma_device_start(&g_capDevice) != MA_SUCCESS)
  {
    ma_device_uninit(&g_capDevice);
    g_capDeviceInit = false;
    ma_context_uninit(&g_capContext);
    g_capContextInit = false;
    return false;
  }
  g_cleanupReady = false;  // start a fresh high-pass / gate state for this capture stream
  g_inputStreamInfo.sampleRate = Device::Audio::kDefaultInputSampleRate;
  g_inputStreamInfo.channelCount = 1;  // the sampler reads mono
  g_inputStreamInfo.bitsPerSample = 16;
  return true;
}

void StopWaveInCapture()
{
  std::lock_guard<std::mutex> lock(g_inCaptureMutex);
  if (g_capDeviceInit)
  {
    ma_device_stop(&g_capDevice);
    ma_device_uninit(&g_capDevice);
    g_capDeviceInit = false;
  }
  if (g_capContextInit)
  {
    ma_context_uninit(&g_capContext);
    g_capContextInit = false;
  }
  std::lock_guard<std::mutex> ringLock(g_inRingMutex);
  g_inRing.clear();
}

Device::Audio::InputStatus ReadWaveInFrames(std::int16_t* destination,
                                            std::size_t frameCount,
                                            std::size_t& framesRead,
                                            std::uint32_t timeoutMs)
{
  framesRead = 0;
  (void)timeoutMs;  // never block the UI thread: drain whatever the capture callback has queued
  {
    std::lock_guard<std::mutex> lock(g_inRingMutex);
    if (g_inRing.empty())
    {
      return Device::Audio::InputStatus::NotReady;
    }
    const std::size_t n = frameCount < g_inRing.size() ? frameCount : g_inRing.size();
    for (std::size_t i = 0; i < n; ++i)
    {
      destination[i] = g_inRing.front();
      g_inRing.pop_front();
    }
    framesRead = n;
  }
  ApplyInputCleanup(destination, framesRead, Device::Audio::kDefaultInputSampleRate);
  return Device::Audio::InputStatus::Ready;
}

#else

bool EnsureWaveOutPumpStarted()
{
  return false;
}

bool EnsureWaveInCaptureStarted()
{
  return false;
}

void StopWaveInCapture()
{
}

Device::Audio::InputStatus ReadWaveInFrames(std::int16_t*,
                                            std::size_t,
                                            std::size_t& framesRead,
                                            std::uint32_t)
{
  framesRead = 0;
  return Device::Audio::InputStatus::NotReady;
}

#endif

}  // namespace

namespace Device::Audio {

void SetOutputVolume(std::uint8_t percent)
{
  // Linux builds have no audio output; the value is still kept for the UI and project settings.
  g_outputVolume.store((std::min)(percent, std::uint8_t{100}), std::memory_order_relaxed);
}

std::size_t ReadComputerAudio(std::int16_t*, std::size_t) { return 0; }

RenderTiming Timing() { return {}; }

std::uint8_t OutputVolume()
{
  return g_outputVolume.load(std::memory_order_relaxed);
}

InputStreamInfo InputInfo()
{
  if (g_microphone == Microphone::Simulated) return InputStreamInfo{kDefaultInputSampleRate, kInputChannels, kInputBitsPerSample};
  return g_inputStreamInfo;
}

InputStatus StartInput()
{
  if (g_microphone == Microphone::Absent) return InputStatus::NotReady;
  if (g_microphone == Microphone::Simulated)
  {
    g_audioInputEnabled = true;
    g_simulatedMicrophoneFrames = 0;
    return InputStatus::Ready;
  }
  g_audioInputEnabled = EnsureWaveInCaptureStarted();
  return g_audioInputEnabled ? InputStatus::Ready : InputStatus::NotReady;
}

void StopInput()
{
  g_audioInputEnabled = false;
  StopWaveInCapture();
}

InputStatus ReadInputFrames(std::int16_t* destination,
                            std::size_t frameCount,
                            std::size_t& framesRead,
                            std::uint32_t timeoutMs)
{
  framesRead = 0;
  if (destination == nullptr && frameCount != 0)
  {
    return InputStatus::InvalidArgument;
  }

  if (!g_audioInputEnabled)
  {
    return InputStatus::NotReady;
  }

  if (frameCount == 0)
  {
    return InputStatus::Ready;
  }

  if (g_microphone == Microphone::Simulated)
  {
    framesRead = std::min(frameCount, g_simulatedMicrophoneFrames);
    g_simulatedMicrophoneFrames -= framesRead;
    for (std::size_t frame = 0; frame < framesRead; ++frame)
      destination[frame] = static_cast<std::int16_t>(static_cast<int>((g_simulatedMicrophonePhase++ % 100) * 300) - 15000);
    return InputStatus::Ready;
  }

  return ReadWaveInFrames(destination, frameCount, framesRead, timeoutMs);
}

}  // namespace Device::Audio

namespace device {

void SetSynthAudioRenderProvider(SynthAudioRenderProvider provider)
{
  std::lock_guard<std::mutex> lock(g_audioProviderMutex);
  g_audioProvider = provider;
}

SynthAudioPipelineBinding ConnectSynthAudioPipeline(std::size_t engineIndex) {
  SynthAudioPipelineBinding binding;
  binding.engineIndex = engineIndex;
  binding.backendName = EnsureWaveOutPumpStarted() ? "simulator-waveout" : "simulator-stub";
  binding.connected = binding.backendName == "simulator-waveout";
  binding.hasNativeOutput = binding.connected;
  return binding;
}

}  // namespace device

namespace CP0::Input {

void SetMicrophonePresent(bool present) { g_microphone = present ? Microphone::Simulated : Microphone::Absent; }
void FeedMicrophone(std::size_t frames) { g_simulatedMicrophoneFrames += frames; }

}  // namespace CP0::Input

// CP0 audio backend hook: the U8 style ALSA output thread (on the device) and the headless CP0Shot capture
// both pull interleaved int16 PCM from the same synth render provider here. Returns frames written.
namespace CP0 {
std::size_t RenderAudioBlock(std::int16_t* destination, std::size_t frameCount) {
  return RenderSynthFrames(destination, frameCount);
}
}  // namespace CP0
