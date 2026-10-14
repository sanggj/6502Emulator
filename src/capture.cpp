#include "capture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace capture {

// ---------------------------------------------------------------- GifWriter

namespace {

struct BitPacker {
    std::vector<uint8_t> *out = nullptr;
    uint32_t acc = 0;
    int nbits = 0;
    void push(uint32_t code, int size) {
        acc |= code << nbits;
        nbits += size;
        while (nbits >= 8) {
            out->push_back(static_cast<uint8_t>(acc & 0xFF));
            acc >>= 8;
            nbits -= 8;
        }
    }
    void flush() {
        if (nbits > 0) out->push_back(static_cast<uint8_t>(acc & 0xFF));
        acc = 0;
        nbits = 0;
    }
};

} // namespace

GifWriter::~GifWriter() {
    std::string err;
    close(err);
}

void GifWriter::writeBytes(const uint8_t *data, std::size_t size) {
    if (file_) std::fwrite(data, 1, size, static_cast<FILE *>(file_));
}

void GifWriter::buildPalette(const uint32_t *argb, std::size_t count) {
    exactColors_.clear();
    exactColors_.reserve(256);
    paletteExact_ = true;
    for (std::size_t i = 0; i < count; ++i) {
        const uint32_t c = argb[i] & 0x00FFFFFFu;
        if (std::find(exactColors_.begin(), exactColors_.end(), c) == exactColors_.end()) {
            if (exactColors_.size() >= 256) {
                paletteExact_ = false;
                break;
            }
            exactColors_.push_back(c);
        }
    }
    palette_.assign(256 * 3, 0);
    auto put = [&](int idx, uint32_t c) {
        palette_[idx * 3 + 0] = static_cast<uint8_t>((c >> 16) & 0xFF);
        palette_[idx * 3 + 1] = static_cast<uint8_t>((c >> 8) & 0xFF);
        palette_[idx * 3 + 2] = static_cast<uint8_t>(c & 0xFF);
    };
    if (paletteExact_) {
        for (std::size_t i = 0; i < exactColors_.size(); ++i) put(static_cast<int>(i), exactColors_[i]);
        for (std::size_t i = exactColors_.size(); i < 256; ++i) put(static_cast<int>(i), 0);
    } else {
        // Fixed 2:3:3 cube fallback for high-color frames.
        for (int r = 0; r < 4; ++r)
            for (int g = 0; g < 8; ++g)
                for (int b = 0; b < 8; ++b) {
                    put((r << 6) | (g << 3) | b, (static_cast<uint32_t>(r * 85) << 16) |
                                                     (static_cast<uint32_t>(g * 36) << 8) |
                                                     static_cast<uint32_t>(b * 36));
                }
    }
}

bool GifWriter::open(const std::string &path, int width, int height, int centisecondsPerFrame,
                     std::string &error) {
    std::string closeErr;
    close(closeErr);
    if (width <= 0 || width > 2048 || height <= 0 || height > 2048) {
        error = "GIF size out of range";
        return false;
    }
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        error = "Could not create GIF file: " + path;
        return false;
    }
    file_ = f;
    width_ = width;
    height_ = height;
    delayCs_ = centisecondsPerFrame < 1 ? 1 : centisecondsPerFrame;
    frames_ = 0;
    error.clear();
    return true;
}

bool GifWriter::addFrame(const uint32_t *argb, int width, int height, std::string &error) {
    if (!file_) {
        error = "GIF not open";
        return false;
    }
    if (!argb || width != width_ || height != height_) {
        error = "frame size mismatch";
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);

    if (frames_ == 0) {
        buildPalette(argb, n);
        uint8_t header[13];
        std::memcpy(header, "GIF89a", 6);
        header[6] = static_cast<uint8_t>(width_ & 0xFF);
        header[7] = static_cast<uint8_t>((width_ >> 8) & 0xFF);
        header[8] = static_cast<uint8_t>(height_ & 0xFF);
        header[9] = static_cast<uint8_t>((height_ >> 8) & 0xFF);
        header[10] = 0xF7; // global color table flag + 256 entries
        header[11] = 0x00;
        header[12] = 0x00;
        writeBytes(header, 13);
        writeBytes(palette_.data(), palette_.size());
    }

    // Graphic control extension (delay in 1/100 s).
    const uint8_t gce[8] = {0x21,
                            0xF9,
                            0x04,
                            0x04,
                            static_cast<uint8_t>(delayCs_ & 0xFF),
                            static_cast<uint8_t>((delayCs_ >> 8) & 0xFF),
                            0x00,
                            0x00};
    writeBytes(gce, 8);

    // Image descriptor at (0,0), no local color table.
    const uint8_t desc[10] = {0x2C,
                              0,
                              0,
                              0,
                              0,
                              static_cast<uint8_t>(width_ & 0xFF),
                              static_cast<uint8_t>((width_ >> 8) & 0xFF),
                              static_cast<uint8_t>(height_ & 0xFF),
                              static_cast<uint8_t>((height_ >> 8) & 0xFF),
                              0x00};
    writeBytes(desc, 10);

    // Map pixels to palette indices.
    std::vector<uint8_t> pix(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (paletteExact_) {
            const uint32_t c = argb[i] & 0x00FFFFFFu;
            int found = 0;
            for (std::size_t k = 0; k < exactColors_.size(); ++k)
                if ((exactColors_[k] & 0x00FFFFFFu) == c) {
                    found = static_cast<int>(k);
                    break;
                }
            pix[i] = static_cast<uint8_t>(found);
        } else {
            const uint32_t c = argb[i];
            pix[i] = static_cast<uint8_t>(((c >> 14) & 0x1C0u) | ((c >> 9) & 0x38u) | ((c >> 5) & 0x07u));
        }
    }

    // Uncompressed LZW: min code size 8, 9-bit literal codes, a CLEAR before
    // the string table can overflow. Deterministic and simple by design.
    std::vector<uint8_t> block;
    block.push_back(0x08);
    BitPacker packer;
    packer.out = &block;
    int sinceClear = 0;
    packer.push(256, 9); // clear
    for (std::size_t i = 0; i < n; ++i) {
        packer.push(pix[i], 9);
        if (++sinceClear >= 254) {
            packer.push(256, 9);
            sinceClear = 0;
        }
    }
    packer.push(256, 9); // clear before EOI keeps decoders happy
    packer.push(257, 9); // end of information
    packer.flush();

    std::size_t off = 0;
    while (off < block.size()) {
        const std::size_t chunk = std::min<std::size_t>(255, block.size() - off);
        const uint8_t len = static_cast<uint8_t>(chunk);
        writeBytes(&len, 1);
        writeBytes(block.data() + off, chunk);
        off += chunk;
    }
    const uint8_t terminator = 0x00;
    writeBytes(&terminator, 1);

    ++frames_;
    error.clear();
    return true;
}

bool GifWriter::close(std::string &error) {
    if (!file_) {
        error.clear();
        return true;
    }
    const uint8_t trailer = 0x3B;
    writeBytes(&trailer, 1);
    const bool ok = std::fflush(static_cast<FILE *>(file_)) == 0;
    std::fclose(static_cast<FILE *>(file_));
    file_ = nullptr;
    error = ok ? std::string() : std::string("GIF write failed");
    return ok;
}

// ----------------------------------------------------------- InputRecording

bool InputRecording::startRecord(const std::string &path, std::string &error) {
    stop();
    path_ = path;
    frames_ = 0;
    mode_ = Mode::Record;
    error.clear();
    return true;
}

bool InputRecording::startReplay(const std::string &path, bool loop, std::string &error) {
    stop();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open recording: " + path;
        return false;
    }
    char magic[4];
    uint32_t count = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char *>(&count), 4);
    if (!in || std::memcmp(magic, "REC1", 4) != 0 || count == 0 || count > 100000000u) {
        error = "Not a valid input recording";
        return false;
    }
    replayData_.resize(count);
    in.read(reinterpret_cast<char *>(replayData_.data()), static_cast<std::streamsize>(count));
    if (!in) {
        error = "Recording truncated";
        return false;
    }
    path_ = path;
    loop_ = loop;
    frames_ = 0;
    mode_ = Mode::Replay;
    error.clear();
    return true;
}

void InputRecording::stop() {
    if (mode_ == Mode::Record && !path_.empty()) {
        std::ofstream out(path_, std::ios::binary);
        if (out) {
            const uint32_t count = frames_;
            out.write("REC1", 4);
            out.write(reinterpret_cast<const char *>(&count), 4);
            out.write(reinterpret_cast<const char *>(replayData_.data()),
                      static_cast<std::streamsize>(replayData_.size()));
        }
    }
    replayData_.clear();
    mode_ = Mode::None;
    loop_ = false;
    frames_ = 0;
    path_.clear();
}

bool InputRecording::frameTick(uint8_t liveButtons, uint8_t &outButtons) {
    switch (mode_) {
    case Mode::None:
        outButtons = liveButtons;
        return false;
    case Mode::Record:
        replayData_.push_back(liveButtons);
        ++frames_;
        outButtons = liveButtons;
        return false;
    case Mode::Replay: {
        if (replayData_.empty()) {
            outButtons = liveButtons;
            return false;
        }
        unsigned idx = frames_;
        if (idx >= replayData_.size()) {
            if (!loop_) {
                mode_ = Mode::None;
                outButtons = liveButtons;
                return false;
            }
            idx = 0;
        }
        outButtons = replayData_[idx];
        ++frames_;
        return true;
    }
    }
    outButtons = liveButtons;
    return false;
}

} // namespace capture
