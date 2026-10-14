#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Apu {
  public:
    Apu();
    ~Apu();

    void init();
    void shutdown();

    void writeRegister(uint16_t address, uint8_t value);
    uint8_t readStatus(uint16_t address);

    void tick(uint64_t cpuCycles);
    void reset();

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    bool enabled() const noexcept { return enabled_; }
    void setVolume(float volume) noexcept;
    float volume() const noexcept;

    // Audio backend selection (item 59): "wasapi" or "waveout".
    // Overrides the 6502_AUDIO_BACKEND environment variable ("waveout" default).
    // Must be called before init(). Unrecognized names fall back to waveOut.
    static void setAudioBackend(const char *name);

    bool startWavRecord(const std::string &path);
    void stopWavRecord();
    bool recording() const noexcept { return recording_; }
    std::size_t recordedSamples() const noexcept { return recorded_.size(); }
    void setRecordGain(float gain) noexcept { recordGain_ = gain; }

    uint8_t registerValue(int index) const noexcept { return registers_[index & 0x1F]; }

    // Savestate serialisation: explicit little-endian blob of register,
    // channel and frame-sequencer state (audio buffers/backend excluded).
    void saveState(std::vector<uint8_t> &out) const;
    void loadState(const std::vector<uint8_t> &in);

  private:
    struct Square {
        bool enabled = false;
        uint8_t duty = 0;
        uint8_t dutyStep = 0;
        uint16_t period = 0;
        uint16_t timer = 0;
        uint8_t lengthCounter = 0;
        uint8_t envelopeVolume = 0;
        uint8_t envelopeDivider = 0;
        bool envelopeDecay = false;
        bool envelopeLoop = false;
        bool constantVolume = false;
        bool sweepEnabled = false;
        uint8_t sweepPeriod = 0;
        bool sweepNegate = false;
        uint8_t sweepShift = 0;
        uint8_t sweepDivider = 0;
        bool sweepReload = false;
    };

    struct Triangle {
        bool enabled = false;
        uint16_t period = 0;
        uint16_t timer = 0;
        uint8_t step = 0;
        uint8_t lengthCounter = 0;
        bool linearReload = false;
        uint8_t linearCounter = 0;
        uint8_t linearReloadValue = 0;
        bool controlFlag = false;
    };

    struct Noise {
        bool enabled = false;
        uint16_t timer = 0;
        uint16_t period = 4;
        uint8_t lengthCounter = 0;
        uint16_t shift = 1;
        bool mode93 = true;
        uint8_t envelopeVolume = 15;
        uint8_t envelopeDivider = 0;
        bool envelopeDecay = false;
        bool envelopeLoop = false;
        bool constantVolume = false;
    };

    void tickSquare(Square &ch, int channelIndex);
    void tickTriangle();
    void tickNoise();
    void tickFrameSequencer(uint64_t cycles);
    void clockEnvelope(Square &ch);
    void clockEnvelopeNoise();
    int16_t mix() const;

    uint8_t registers_[0x18]{};
    Square square_[2];
    Triangle triangle_;
    Noise noise_;
    uint64_t frameTimer_ = 0;
    uint8_t frameStep_ = 0;
    bool frameIrqInhibit_ = false;
    bool frameIrqPending_ = false;
    double sampleAccumulator_ = 0.0;

    bool enabled_ = false;
    float volumeLevel_ = 0.8f;
    float recordGain_ = 1.0f;
    bool recording_ = false;
    std::string recordPath_;
    std::vector<int16_t> recorded_;

    struct Impl;
    Impl *impl_ = nullptr;
};
