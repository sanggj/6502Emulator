#include "apu.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

// WASAPI headers are optional at compile time; the backend itself is always
// optional at runtime (any init failure falls back to waveOut silently).
#if defined(__has_include)
#if __has_include(<mmdeviceapi.h>) && __has_include(<audioclient.h>)
#define APU_HAVE_WASAPI 1
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif
#endif
#endif

namespace {

constexpr int kSampleRate = 44100;
constexpr double kCpuHz = 1789772.5;
constexpr std::size_t kRingCapacity = 16384;

const uint8_t kDutyWave[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

const uint16_t kNoisePeriods[16] = {4,   8,   16,  32,  64,  96,   128,  160,
                                    202, 254, 380, 508, 762, 1016, 2034, 4068};
const uint8_t kLengthTable[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                  12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

uint16_t periodFromRegisters(const uint8_t *regs, int channel) {
    const uint16_t low = regs[channel * 4 + 2];
    const uint16_t high = regs[channel * 4 + 3] & 0x07u;
    return static_cast<uint16_t>(low | (high << 8));
}

struct ApuWavHeader {
    uint32_t riff;
    uint32_t fileSize;
    uint32_t wave;
    uint32_t fmt;
    uint32_t fmtSize = 16;
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t sampleRate = kSampleRate;
    uint32_t byteRate;
    uint16_t blockAlign = 2;
    uint16_t bits = 16;
    uint32_t data;
    uint32_t dataSize;
};

#ifdef _WIN32

constexpr int kWaveOutBufferFrames = 2048;
constexpr int kMaxPushFrames = 8192;

std::string &backendOverride() {
    static std::string s;
    return s;
}

// Tiny internal audio backend interface (item 59). The Apu ring/thread push
// int16 mono 44100 Hz frames to whichever backend was selected.
struct AudioBackend {
    virtual ~AudioBackend() {}
    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual int writableFrames() = 0; // 0 -> nothing to do right now
    virtual bool submit(const int16_t *frames, int count) = 0;
    virtual void applyVolume(float v) = 0;
};

class WaveOutBackend final : public AudioBackend {
  public:
    bool init() override {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = kSampleRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = kSampleRate * 2;
        if (waveOutOpen(&waveOut_, WAVE_MAPPER, &wf, reinterpret_cast<DWORD_PTR>(nullptr), 0,
                        CALLBACK_NULL) != MMSYSERR_NOERROR)
            return false;
        for (int i = 0; i < 2; ++i) {
            std::memset(&headers_[i], 0, sizeof(WAVEHDR));
            headers_[i].lpData = reinterpret_cast<LPSTR>(buffers_[i]);
            headers_[i].dwBufferLength = sizeof(buffers_[i]);
            waveOutPrepareHeader(waveOut_, &headers_[i], sizeof(WAVEHDR));
            waveOutWrite(waveOut_, &headers_[i], sizeof(WAVEHDR));
        }
        prepared_[0] = prepared_[1] = true;
        return true;
    }

    void shutdown() override {
        if (waveOut_) {
            for (int i = 0; i < 2; ++i)
                if (prepared_[i]) waveOutUnprepareHeader(waveOut_, &headers_[i], sizeof(WAVEHDR));
            waveOutClose(waveOut_);
            waveOut_ = nullptr;
            prepared_[0] = prepared_[1] = false;
        }
    }

    int writableFrames() override {
        return (headers_[current_].dwFlags & WHDR_DONE) != 0 ? kWaveOutBufferFrames : 0;
    }

    bool submit(const int16_t *frames, int count) override {
        if (!waveOut_ || count <= 0) return false;
        WAVEHDR &header = headers_[current_];
        const int n = std::min(count, kWaveOutBufferFrames);
        std::memcpy(buffers_[current_], frames, static_cast<std::size_t>(n) * sizeof(int16_t));
        header.dwBufferLength = static_cast<DWORD>(n * sizeof(int16_t));
        if (prepared_[current_]) waveOutUnprepareHeader(waveOut_, &header, sizeof(WAVEHDR));
        waveOutPrepareHeader(waveOut_, &header, sizeof(WAVEHDR));
        prepared_[current_] = true;
        waveOutWrite(waveOut_, &header, sizeof(header));
        current_ = 1 - current_;
        return true;
    }

    void applyVolume(float v) override {
        if (!waveOut_) return;
        const DWORD vol = static_cast<DWORD>(v * 0xFFFFu);
        waveOutSetVolume(waveOut_, vol | (vol << 16));
    }

  private:
    HWAVEOUT waveOut_ = nullptr;
    int16_t buffers_[2][kWaveOutBufferFrames]{};
    WAVEHDR headers_[2]{};
    bool prepared_[2] = {false, false};
    int current_ = 0;
};

#ifdef APU_HAVE_WASAPI

// Locally-defined COM IIDs plus dynamically resolved ole32 entry points so
// neither ole32.lib nor any extra link dependency is required.
struct OleApi {
    bool attempted = false;
    HMODULE dll = nullptr;
    HRESULT(WINAPI *initializeEx)(LPVOID, DWORD) = nullptr;
    HRESULT(WINAPI *createInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *) = nullptr;
    void(WINAPI *uninitialize)(void) = nullptr;
};

OleApi &ole() {
    static OleApi api;
    if (!api.attempted) {
        api.attempted = true;
        api.dll = LoadLibraryA("ole32.dll");
        if (api.dll) {
            api.initializeEx =
                reinterpret_cast<HRESULT(WINAPI *)(LPVOID, DWORD)>(GetProcAddress(api.dll, "CoInitializeEx"));
            api.createInstance = reinterpret_cast<HRESULT(WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID,
                                                                    LPVOID *)>(
                GetProcAddress(api.dll, "CoCreateInstance"));
            api.uninitialize = reinterpret_cast<void(WINAPI *)(void)>(GetProcAddress(api.dll, "CoUninitialize"));
        }
    }
    return api;
}

const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                         {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
                                      {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483,
                                     {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

class WasapiBackend final : public AudioBackend {
  public:
    bool init() override {
        OleApi &o = ole();
        if (!o.initializeEx || !o.createInstance) return false;

        const HRESULT cohr = o.initializeEx(nullptr, COINIT_MULTITHREADED);
        comInited_ = SUCCEEDED(cohr) && cohr != S_FALSE;
        if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE && cohr != S_FALSE) return false;

        HRESULT hr = o.createInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      kIID_IMMDeviceEnumerator, reinterpret_cast<void **>(&enumerator_));
        if (FAILED(hr)) return cleanup(), false;
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr)) return cleanup(), false;
        hr = device_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client_));
        if (FAILED(hr)) return cleanup(), false;

        // Preferred: 44100 Hz mono 16-bit PCM (matches the internal stream).
        WAVEFORMATEX pcm{};
        pcm.wFormatTag = WAVE_FORMAT_PCM;
        pcm.nChannels = 1;
        pcm.nSamplesPerSec = kSampleRate;
        pcm.wBitsPerSample = 16;
        pcm.nBlockAlign = 2;
        pcm.nAvgBytesPerSec = kSampleRate * 2;
        // Fallback: shared-mode engines always resample float32.
        WAVEFORMATEX flt{};
        flt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        flt.nChannels = 1;
        flt.nSamplesPerSec = kSampleRate;
        flt.wBitsPerSample = 32;
        flt.nBlockAlign = 4;
        flt.nAvgBytesPerSec = kSampleRate * 4;

        format_ = nullptr;
        if (SUCCEEDED(client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &pcm, nullptr)))
            format_ = &pcmStorage_;
        else if (SUCCEEDED(client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &flt, nullptr)))
            format_ = &fltStorage_;
        else
            return cleanup(), false;

        pcmStorage_ = pcm;
        fltStorage_ = flt;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, /*eventless polling*/
                                 2000000,                     // 200 ms device buffer
                                 0, format_, nullptr);
        if (FAILED(hr)) return cleanup(), false;
        hr = client_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr)) return cleanup(), false;
        hr = client_->GetService(kIID_IAudioRenderClient, reinterpret_cast<void **>(&render_));
        if (FAILED(hr)) return cleanup(), false;

        BYTE *data = nullptr;
        if (FAILED(render_->GetBuffer(bufferFrames_, &data))) return cleanup(), false;
        std::memset(data, 0, static_cast<std::size_t>(bufferFrames_) * format_->nBlockAlign);
        render_->ReleaseBuffer(bufferFrames_, 0);

        if (FAILED(client_->Start())) return cleanup(), false;
        started_ = true;
        return true;
    }

    void shutdown() override {
        cleanup();
    }

    int writableFrames() override {
        if (!client_ || !format_) return 0;
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return 0;
        const int avail = static_cast<int>(bufferFrames_) - static_cast<int>(padding);
        return avail > 0 ? std::min(avail, static_cast<int>(bufferFrames_)) : 0;
    }

    bool submit(const int16_t *frames, int count) override {
        if (!client_ || !render_ || !format_ || count <= 0) return false;
        BYTE *dst = nullptr;
        if (FAILED(render_->GetBuffer(static_cast<UINT32>(count), &dst))) return false;
        convert(dst, frames, count);
        render_->ReleaseBuffer(static_cast<UINT32>(count), 0);
        return true;
    }

    void applyVolume(float v) override { volume_ = v; }

  private:
    void convert(BYTE *dst, const int16_t *src, int count) const {
        const UINT32 channels = format_->nChannels;
        if (format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            auto *out = reinterpret_cast<float *>(dst);
            for (int i = 0; i < count; ++i) {
                const float f = (static_cast<float>(src[i]) / 32768.0f) * volume_;
                for (UINT32 c = 0; c < channels; ++c) out[i * channels + c] = f;
            }
        } else {
            auto *out = reinterpret_cast<int16_t *>(dst);
            for (int i = 0; i < count; ++i) {
                const int16_t s = static_cast<int16_t>(
                    std::clamp(static_cast<int>(static_cast<float>(src[i]) * volume_), -32000, 32000));
                for (UINT32 c = 0; c < channels; ++c) out[i * channels + c] = s;
            }
        }
    }

    void cleanup() {
        if (client_ && started_) {
            client_->Stop();
            started_ = false;
        }
        if (render_) {
            render_->Release();
            render_ = nullptr;
        }
        if (client_) {
            client_->Release();
            client_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
        if (enumerator_) {
            enumerator_->Release();
            enumerator_ = nullptr;
        }
        if (comInited_) {
            OleApi &o = ole();
            if (o.uninitialize) o.uninitialize();
            comInited_ = false;
        }
        format_ = nullptr;
        bufferFrames_ = 0;
    }

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *device_ = nullptr;
    IAudioClient *client_ = nullptr;
    IAudioRenderClient *render_ = nullptr;
    WAVEFORMATEX pcmStorage_{};
    WAVEFORMATEX fltStorage_{};
    WAVEFORMATEX *format_ = nullptr;
    UINT32 bufferFrames_ = 0;
    float volume_ = 0.8f;
    bool started_ = false;
    bool comInited_ = false;
};

#else // !APU_HAVE_WASAPI

class WasapiBackend final : public AudioBackend {
  public:
    bool init() override { return false; }
    void shutdown() override {}
    int writableFrames() override { return 0; }
    bool submit(const int16_t *, int) override { return false; }
    void applyVolume(float) override {}
};

#endif // APU_HAVE_WASAPI

} // namespace

struct Apu::Impl {
    CRITICAL_SECTION lock{};
    HANDLE thread = nullptr;
    bool stopThread = false;
    std::unique_ptr<AudioBackend> backend;
    std::vector<int16_t> scratch;
    std::vector<int16_t> ring;

    static DWORD WINAPI audioThreadProc(LPVOID param) {
        auto *self = static_cast<Impl *>(param);
        while (!self->stopThread) {
            AudioBackend *backend = self->backend.get();
            if (!backend) break;
            const int want = backend->writableFrames();
            if (want <= 0) {
                Sleep(4);
                continue;
            }
            const int n = std::min(want, kMaxPushFrames);
            if (static_cast<int>(self->scratch.size()) < n) self->scratch.resize(static_cast<std::size_t>(n));
            EnterCriticalSection(&self->lock);
            int got = 0;
            while (got < n && !self->ring.empty()) {
                self->scratch[static_cast<std::size_t>(got++)] = self->ring.front();
                self->ring.erase(self->ring.begin());
            }
            LeaveCriticalSection(&self->lock);
            for (int i = got; i < n; ++i) self->scratch[static_cast<std::size_t>(i)] = 0; // underrun
            backend->submit(self->scratch.data(), n);
        }
        return 0;
    }
};
#else

struct Apu::Impl {
    std::vector<int16_t> ring;
};

#endif

Apu::Apu() : impl_(new Impl()) {}
Apu::~Apu() {
    shutdown();
    delete impl_;
}

void Apu::init() {
#ifdef _WIN32
    InitializeCriticalSection(&impl_->lock);
    if (impl_->thread) return;

    // Backend selection: explicit setter > "6502_AUDIO_BACKEND" env var >
    // default waveOut.
    std::string want = backendOverride();
    if (want.empty()) {
        char env[64] = {};
        const DWORD n = GetEnvironmentVariableA("6502_AUDIO_BACKEND", env, sizeof(env));
        if (n > 0 && n < sizeof(env)) want = env;
    }
    for (char &c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::unique_ptr<AudioBackend> chosen;
    if (want == "wasapi") {
        auto *wasapi = new WasapiBackend();
        if (wasapi->init())
            chosen.reset(wasapi);
        else {
            wasapi->shutdown();
            delete wasapi;
        } // silent fallback to waveOut
    }
    if (!chosen) {
        auto *waveout = new WaveOutBackend();
        if (waveout->init())
            chosen.reset(waveout);
        else {
            waveout->shutdown();
            delete waveout;
        }
    }
    if (!chosen) return;

    impl_->backend = std::move(chosen);
    impl_->backend->applyVolume(volumeLevel_);
    impl_->stopThread = false;
    impl_->thread = CreateThread(nullptr, 0, Impl::audioThreadProc, impl_, 0, nullptr);
    enabled_ = true;
#endif
}

void Apu::shutdown() {
#ifdef _WIN32
    enabled_ = false;
    if (impl_->thread) {
        impl_->stopThread = true;
        WaitForSingleObject(impl_->thread, 1000);
        CloseHandle(impl_->thread);
        impl_->thread = nullptr;
    }
    if (impl_->backend) {
        impl_->backend->shutdown();
        impl_->backend.reset();
    }
    DeleteCriticalSection(&impl_->lock);
#endif
}

void Apu::setVolume(float volume) noexcept {
    volumeLevel_ = std::clamp(volume, 0.0f, 1.0f);
#ifdef _WIN32
    if (impl_->backend) impl_->backend->applyVolume(volumeLevel_);
#endif
}

float Apu::volume() const noexcept {
    return volumeLevel_;
}

void Apu::setAudioBackend(const char *name) {
#ifdef _WIN32
    backendOverride() = name ? name : "";
#else
    (void)name;
#endif
}

void Apu::reset() {
    std::memset(registers_, 0, sizeof(registers_));
    square_[0] = Square{};
    square_[1] = Square{};
    triangle_ = Triangle{};
    noise_ = Noise{};
    frameTimer_ = 0;
    frameStep_ = 0;
    frameIrqInhibit_ = false;
    frameIrqPending_ = false;
}

namespace {
// Explicit little-endian blob helpers for savestate serialisation.
void put8(std::vector<uint8_t> &v, uint64_t x) { v.push_back(static_cast<uint8_t>(x)); }
void put16(std::vector<uint8_t> &v, uint64_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
}
void put64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFFu));
}
} // namespace

void Apu::saveState(std::vector<uint8_t> &out) const {
    for (int i = 0; i < 0x18; ++i) put8(out, registers_[i]);
    for (const Square &ch : square_) {
        put8(out, ch.enabled);
        put8(out, ch.duty);
        put8(out, ch.dutyStep);
        put16(out, ch.period);
        put16(out, ch.timer);
        put8(out, ch.lengthCounter);
        put8(out, ch.envelopeVolume);
        put8(out, ch.envelopeDivider);
        put8(out, ch.envelopeDecay);
        put8(out, ch.envelopeLoop);
        put8(out, ch.constantVolume);
        put8(out, ch.sweepEnabled);
        put8(out, ch.sweepPeriod);
        put8(out, ch.sweepNegate);
        put8(out, ch.sweepShift);
        put8(out, ch.sweepDivider);
        put8(out, ch.sweepReload);
    }
    put8(out, triangle_.enabled);
    put16(out, triangle_.period);
    put16(out, triangle_.timer);
    put8(out, triangle_.step);
    put8(out, triangle_.lengthCounter);
    put8(out, triangle_.linearReload);
    put8(out, triangle_.linearCounter);
    put8(out, triangle_.linearReloadValue);
    put8(out, triangle_.controlFlag);
    put8(out, noise_.enabled);
    put16(out, noise_.timer);
    put16(out, noise_.period);
    put8(out, noise_.lengthCounter);
    put16(out, noise_.shift);
    put8(out, noise_.mode93);
    put8(out, noise_.envelopeVolume);
    put8(out, noise_.envelopeDivider);
    put8(out, noise_.envelopeDecay);
    put8(out, noise_.envelopeLoop);
    put8(out, noise_.constantVolume);
    put64(out, frameTimer_);
    put8(out, frameStep_);
    put8(out, frameIrqInhibit_);
    put8(out, frameIrqPending_);
    put8(out, enabled_);
}

void Apu::loadState(const std::vector<uint8_t> &in) {
    std::vector<uint8_t> expected;
    saveState(expected);
    if (in.size() < expected.size()) return; // truncated/foreign blob: keep current state
    std::size_t pos = 0;
    auto g8 = [&]() -> uint8_t { return in[pos++]; };
    auto g16 = [&]() -> uint16_t {
        const uint16_t lo = in[pos++];
        const uint16_t hi = in[pos++];
        return static_cast<uint16_t>(lo | (hi << 8));
    };
    auto g64 = [&]() -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[pos++]) << (i * 8);
        return v;
    };
    for (int i = 0; i < 0x18; ++i) registers_[i] = g8();
    for (Square &ch : square_) {
        ch.enabled = g8() != 0;
        ch.duty = g8();
        ch.dutyStep = g8();
        ch.period = g16();
        ch.timer = g16();
        ch.lengthCounter = g8();
        ch.envelopeVolume = g8();
        ch.envelopeDivider = g8();
        ch.envelopeDecay = g8() != 0;
        ch.envelopeLoop = g8() != 0;
        ch.constantVolume = g8() != 0;
        ch.sweepEnabled = g8() != 0;
        ch.sweepPeriod = g8();
        ch.sweepNegate = g8() != 0;
        ch.sweepShift = g8();
        ch.sweepDivider = g8();
        ch.sweepReload = g8() != 0;
    }
    triangle_.enabled = g8() != 0;
    triangle_.period = g16();
    triangle_.timer = g16();
    triangle_.step = g8();
    triangle_.lengthCounter = g8();
    triangle_.linearReload = g8() != 0;
    triangle_.linearCounter = g8();
    triangle_.linearReloadValue = g8();
    triangle_.controlFlag = g8() != 0;
    noise_.enabled = g8() != 0;
    noise_.timer = g16();
    noise_.period = g16();
    noise_.lengthCounter = g8();
    noise_.shift = g16();
    noise_.mode93 = g8() != 0;
    noise_.envelopeVolume = g8();
    noise_.envelopeDivider = g8();
    noise_.envelopeDecay = g8() != 0;
    noise_.envelopeLoop = g8() != 0;
    noise_.constantVolume = g8() != 0;
    frameTimer_ = g64();
    frameStep_ = g8();
    frameIrqInhibit_ = g8() != 0;
    frameIrqPending_ = g8() != 0;
    enabled_ = g8() != 0;
}

void Apu::writeRegister(uint16_t address, uint8_t value) {
    if (address < 0x4000 || address > 0x4017) return;
    const int index = address - 0x4000;
    registers_[index] = value;

    if (index <= 0x07) {
        Square &ch = square_[index / 4];
        const int sub = index % 4;
        switch (sub) {
        case 0:
            ch.duty = (value >> 6) & 0x03u;
            ch.envelopeLoop = (value & 0x20u) != 0;
            ch.constantVolume = (value & 0x10u) != 0;
            ch.envelopeVolume = value & 0x0Fu;
            break;
        case 2:
            ch.period = periodFromRegisters(registers_, index / 4);
            break;
        case 3: {
            ch.period = periodFromRegisters(registers_, index / 4);
            if (ch.enabled) ch.lengthCounter = kLengthTable[(value >> 3) & 0x1Fu];
            ch.envelopeDecay = true;
            ch.dutyStep = 0;
            break;
        }
        default:
            break;
        }
    } else if (index >= 0x08 && index <= 0x0B) {
        switch (index % 4) {
        case 0:
            triangle_.controlFlag = (value & 0x80u) != 0;
            triangle_.linearReloadValue = value & 0x7Fu;
            break;
        case 2:
            triangle_.period = static_cast<uint16_t>((registers_[0x0A]) | ((value & 0x07u) << 8));
            break;
        case 3:
            triangle_.period = static_cast<uint16_t>(registers_[0x0A] | ((value & 0x07u) << 8));
            triangle_.linearReload = true;
            if (triangle_.enabled) triangle_.lengthCounter = kLengthTable[(value >> 3) & 0x1Fu];
            break;
        default:
            break;
        }
    } else if (index >= 0x0C && index <= 0x0F) {
        switch (index % 4) {
        case 0:
            noise_.constantVolume = (value & 0x10u) != 0;
            noise_.envelopeVolume = value & 0x0Fu;
            noise_.envelopeLoop = (value & 0x20u) != 0;
            break;
        case 2:
            noise_.period = kNoisePeriods[value & 0x0Fu];
            noise_.mode93 = (value & 0x80u) != 0;
            break;
        case 3:
            if (noise_.enabled) noise_.lengthCounter = kLengthTable[(value >> 3) & 0x1Fu];
            noise_.envelopeDecay = true;
            break;
        default:
            break;
        }
    }

    switch (index) {
    case 0x15: {
        square_[0].enabled = (value & 0x01u) != 0;
        square_[1].enabled = (value & 0x02u) != 0;
        triangle_.enabled = (value & 0x04u) != 0;
        noise_.enabled = (value & 0x08u) != 0;
        if (!square_[0].enabled) square_[0].lengthCounter = 0;
        if (!square_[1].enabled) square_[1].lengthCounter = 0;
        if (!triangle_.enabled) triangle_.lengthCounter = 0;
        if (!noise_.enabled) noise_.lengthCounter = 0;
        break;
    }
    case 0x17:
        frameIrqInhibit_ = (value & 0x40u) != 0;
        frameStep_ = 0;
        frameTimer_ = 0;
        break;
    default:
        break;
    }
}

uint8_t Apu::readStatus(uint16_t address) {
    if (address != 0x4015) return 0;
    uint8_t status = 0;
    if (square_[0].lengthCounter > 0) status |= 0x01;
    if (square_[1].lengthCounter > 0) status |= 0x02;
    if (triangle_.lengthCounter > 0) status |= 0x04;
    if (noise_.lengthCounter > 0) status |= 0x08;
    if (frameIrqPending_) status |= 0x40;
    frameIrqPending_ = false;
    return status;
}

void Apu::clockEnvelope(Square &ch) {
    if (ch.envelopeDecay) {
        ch.envelopeDivider = ch.constantVolume ? ch.envelopeVolume : 15;
        ch.envelopeDecay = false;
        return;
    }
    if (ch.envelopeDivider > 0) {
        --ch.envelopeDivider;
        return;
    }
    ch.envelopeDivider = ch.constantVolume ? ch.envelopeVolume : 15;
    if (!ch.constantVolume && ch.envelopeVolume > 0 && !ch.envelopeLoop) --ch.envelopeVolume;
}

void Apu::clockEnvelopeNoise() {
    if (noise_.envelopeDecay) {
        noise_.envelopeDivider = noise_.constantVolume ? noise_.envelopeVolume : 15;
        noise_.envelopeDecay = false;
        return;
    }
    if (noise_.envelopeDivider > 0) {
        --noise_.envelopeDivider;
        return;
    }
    noise_.envelopeDivider = noise_.constantVolume ? noise_.envelopeVolume : 15;
    if (!noise_.constantVolume && noise_.envelopeVolume > 0 && !noise_.envelopeLoop) --noise_.envelopeVolume;
}

void Apu::tickSquare(Square &ch, int channelIndex) {
    (void)channelIndex;
    if (!ch.enabled || ch.lengthCounter == 0 || ch.period < 8) return;
    if (ch.timer == 0) {
        ch.timer = ch.period;
        ch.dutyStep = (ch.dutyStep + 1) & 7u;
    } else {
        --ch.timer;
    }
}

void Apu::tickTriangle() {
    if (!triangle_.enabled || triangle_.lengthCounter == 0) return;
    if (triangle_.timer == 0) {
        triangle_.timer = triangle_.period ? triangle_.period : 1;
        if (triangle_.linearCounter > 0 || triangle_.controlFlag) triangle_.step = (triangle_.step + 1) & 31u;
    } else {
        --triangle_.timer;
    }
}

void Apu::tickNoise() {
    if (!noise_.enabled || noise_.lengthCounter == 0) return;
    if (--noise_.timer == 0) {
        noise_.timer = noise_.period;
        const uint16_t feedback =
            static_cast<uint16_t>((noise_.shift & 1u) ^ ((noise_.shift >> (noise_.mode93 ? 6 : 1)) & 1u));
        noise_.shift = static_cast<uint16_t>((noise_.shift >> 1) | (feedback << 14));
    }
}

void Apu::tickFrameSequencer(uint64_t cycles) {
    frameTimer_ += cycles;
    constexpr uint64_t kQuarterFrame = 7457;
    while (frameTimer_ >= kQuarterFrame) {
        frameTimer_ -= kQuarterFrame;
        ++frameStep_;
        if (frameStep_ % 2 == 1) {
            clockEnvelope(square_[0]);
            clockEnvelope(square_[1]);
            clockEnvelopeNoise();
        }
        if (frameStep_ >= 4) {
            frameStep_ = 0;
            auto decayLength = [](Square &ch) {
                if (ch.lengthCounter > 0 && !ch.envelopeLoop) --ch.lengthCounter;
            };
            decayLength(square_[0]);
            decayLength(square_[1]);
            if (triangle_.lengthCounter > 0 && !triangle_.controlFlag) --triangle_.lengthCounter;
            if (noise_.lengthCounter > 0 && !noise_.envelopeLoop) --noise_.lengthCounter;
            if (triangle_.linearReload)
                triangle_.linearCounter = triangle_.linearReloadValue;
            else if (triangle_.linearCounter > 0)
                --triangle_.linearCounter;
            if (!triangle_.controlFlag) triangle_.linearReload = false;
        }
    }
}

int16_t Apu::mix() const {
    int32_t sum = 0;
    for (int i = 0; i < 2; ++i) {
        const Square &ch = square_[i];
        if (ch.enabled && ch.lengthCounter > 0 && ch.period >= 8 && kDutyWave[ch.duty][ch.dutyStep]) {
            sum += ch.constantVolume ? ch.envelopeVolume : ch.envelopeVolume;
        }
    }
    if (triangle_.enabled && triangle_.lengthCounter > 0)
        sum += (triangle_.step < 16 ? triangle_.step : 31 - triangle_.step) / 4;
    if (noise_.enabled && noise_.lengthCounter > 0 && !(noise_.shift & 1u))
        sum += noise_.constantVolume ? noise_.envelopeVolume : noise_.envelopeVolume;
    return static_cast<int16_t>(std::clamp(sum * 500, -32000, 32000));
}

void Apu::tick(uint64_t cpuCycles) {
    if (!enabled_ && !recording_) return;

    for (uint64_t i = 0; i < cpuCycles; ++i) {
        tickSquare(square_[0], 0);
        tickSquare(square_[1], 1);
        tickTriangle();
        tickNoise();
    }
    tickFrameSequencer(cpuCycles);

    sampleAccumulator_ += cpuCycles;
    while (sampleAccumulator_ >= kCpuHz / kSampleRate) {
        sampleAccumulator_ -= kCpuHz / kSampleRate;
        const int16_t sample = static_cast<int16_t>(
            std::clamp(static_cast<int>(static_cast<float>(mix()) * recordGain_), -32000, 32000));
        if (recording_) recorded_.push_back(sample);
#ifdef _WIN32
        if (enabled_ && impl_->thread) {
            EnterCriticalSection(&impl_->lock);
            impl_->ring.push_back(sample);
            while (impl_->ring.size() > kRingCapacity) impl_->ring.erase(impl_->ring.begin());
            LeaveCriticalSection(&impl_->lock);
        }
#endif
    }
}

bool Apu::startWavRecord(const std::string &path) {
    stopWavRecord();
    recorded_.clear();
    recording_ = true;
    recordPath_ = path;
    return true;
}

void Apu::stopWavRecord() {
    if (!recording_) return;
    recording_ = false;
    if (recorded_.empty()) return;
    ApuWavHeader header{};
    header.riff = 0x46464952;
    header.wave = 0x45564157;
    header.fmt = 0x20746D66;
    header.data = 0x61746164;
    header.byteRate = kSampleRate * 2;
    header.dataSize = static_cast<uint32_t>(recorded_.size() * 2);
    header.fileSize = 36 + header.dataSize;
    std::ofstream out(recordPath_, std::ios::binary);
    if (!out) return;
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    out.write(reinterpret_cast<const char *>(recorded_.data()),
              static_cast<std::streamsize>(recorded_.size() * 2));
    recorded_.clear();
}
