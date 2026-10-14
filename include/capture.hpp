#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Animated-GIF writer used by the frame capture feature. Frames are stored
// with the classic "uncompressed LZW" scheme (clear-code streaming), so the
// encoder is tiny and safe, at the cost of larger files than optimized GIFs.
namespace capture {

class GifWriter {
  public:
    GifWriter() = default;
    ~GifWriter();
    GifWriter(const GifWriter &) = delete;
    GifWriter &operator=(const GifWriter &) = delete;

    bool open(const std::string &path, int width, int height, int centisecondsPerFrame, std::string &error);
    bool addFrame(const uint32_t *argb, int width, int height, std::string &error);
    bool close(std::string &error);
    bool isOpen() const noexcept { return file_ != nullptr; }
    unsigned frames() const noexcept { return frames_; }

  private:
    void writeBytes(const uint8_t *data, std::size_t size);
    void buildPalette(const uint32_t *argb, std::size_t count);
    void *file_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int delayCs_ = 4;
    unsigned frames_ = 0;
    bool paletteExact_ = false;
    std::vector<uint32_t> exactColors_;
    std::vector<uint8_t> palette_; // 256 * 3 RGB bytes
};

// Controller input recording/replay. The recording stores one controller
// byte ($12E0 layout) per emulation frame; replay substitutes recorded
// bytes for live input until stopped.
class InputRecording {
  public:
    bool startRecord(const std::string &path, std::string &error);
    bool startReplay(const std::string &path, bool loop, std::string &error);
    void stop();
    bool active() const noexcept { return mode_ != Mode::None; }
    bool recording() const noexcept { return mode_ == Mode::Record; }
    bool replaying() const noexcept { return mode_ == Mode::Replay; }
    const std::string &path() const noexcept { return path_; }
    unsigned frames() const noexcept { return frames_; }

    // Called once per emulated frame with live buttons; returns the effective
    // byte to inject into $12E0 and reports whether replay supplied it.
    bool frameTick(uint8_t liveButtons, uint8_t &outButtons);

  private:
    enum class Mode { None, Record, Replay };
    Mode mode_ = Mode::None;
    bool loop_ = false;
    std::string path_;
    std::vector<uint8_t> replayData_;
    unsigned frames_ = 0;
};

} // namespace capture
