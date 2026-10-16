#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Memory {
  public:
    struct Write {
        uint16_t address;
        uint8_t oldValue;
        uint8_t newValue;
    };
    static constexpr std::size_t SIZE = 0x10000;
    static bool isIoRange(uint16_t address) noexcept { return address >= 0x4000 && address <= 0x5FFF; }

    Memory();

    void reset(uint8_t fill = 0x00);
    uint8_t read(uint16_t address) const noexcept;
    void write(uint16_t address, uint8_t value) noexcept;
    // Raw poke: writes RAM directly, BYPASSING the I/O read/write hooks.
    // Fine for restore paths (stepBack undo) and main-loop state pushes that
    // intentionally skip device side effects - but a debugger/hex-view poke
    // through here on an I/O address ($4000-$5FFF, e.g. $4015) silently
    // misses the APU/PPU/devices. Use write() for pokes that must hit hooks.
    void writeRaw(uint16_t address, uint8_t value) noexcept {
        ram_[address] = value;
        ++epoch_;
        dirtyPage_[address >> 6] = epoch_;
    }

    // NMOS read-modify-write bus behavior: the CPU writes the ORIGINAL value
    // first and then the modified value. I/O devices therefore observe two
    // write strobes (e.g. INC $4016 pulses the controller strobe twice).
    void writeRmw(uint16_t address, uint8_t oldValue, uint8_t newValue) noexcept;

    // Per-byte access-frequency counters used by the hex-view heatmap.
    uint8_t heat(uint16_t address) const noexcept { return accessHeat_[address]; }
    void resetHeat() noexcept { accessHeat_.fill(0); }
    uint64_t pageEpoch(uint16_t address) const noexcept { return dirtyPage_[address >> 6]; }
    void beginWriteTrace() noexcept { lastWrites_.clear(); }
    const std::vector<Write> &lastWrites() const noexcept { return lastWrites_; }

    void setIoReadHook(std::function<uint8_t(uint16_t)> hook) { ioReadHook_ = std::move(hook); }
    void setIoWriteHook(std::function<void(uint16_t, uint8_t)> hook) { ioWriteHook_ = std::move(hook); }

    bool loadFile(const std::string &filename, uint16_t startAddress, std::string &error);
    bool loadBytes(const std::vector<uint8_t> &bytes, uint16_t startAddress, std::string &error);
    std::vector<uint8_t> copyRange(uint16_t startAddress, std::size_t count) const;
    void fill(uint16_t startAddress, std::size_t count, uint8_t value) noexcept;
    std::vector<uint16_t> search(const std::vector<uint8_t> &pattern, uint16_t startAddress = 0,
                                 std::size_t count = SIZE) const;
    static bool compare(const Memory &a, const Memory &b, std::vector<Write> &differences,
                        std::size_t limit = SIZE);
    uint32_t crc32(uint16_t startAddress = 0, std::size_t count = SIZE) const noexcept;
    bool saveRange(const std::string &filename, uint16_t startAddress, std::size_t count,
                   std::string &error) const;

    const uint8_t *data() const noexcept { return ram_.data(); }
    uint8_t *data() noexcept { return ram_.data(); }

    uint64_t epoch() const noexcept { return epoch_; }
    bool pageChangedSince(uint16_t address, std::size_t count, uint64_t sinceEpoch) const noexcept {
        if (count == 0) return false;
        // Wider math: the old ((address & 0xFFFF) + count - 1) computed in
        // 16/32-bit or wrapping size_t could put lastPage below firstPage
        // (loop no-ops). Compute the span without any wrap and clamp to the
        // last valid 64-byte page (1023).
        const std::size_t firstPage = static_cast<std::size_t>(address) >> 6;
        const std::size_t span = count - 1;
        const std::size_t maxSpan = 0xFFFFu - static_cast<std::size_t>(address);
        const std::size_t lastPage = span >= maxSpan ? 1023 : (static_cast<std::size_t>(address) + span) >> 6;
        for (std::size_t p = firstPage; p <= lastPage; ++p)
            if (dirtyPage_[p] > sinceEpoch) return true;
        return false;
    }
    void markAllDirty() noexcept {
        ++epoch_;
        for (auto &d : dirtyPage_) d = epoch_;
    }

  private:
    std::array<uint8_t, SIZE> ram_{};
    mutable std::array<uint8_t, SIZE> accessHeat_{};
    std::vector<Write> lastWrites_;
    std::array<uint64_t, 1024> dirtyPage_{};
    uint64_t epoch_ = 0;
    std::function<uint8_t(uint16_t)> ioReadHook_;
    std::function<void(uint16_t, uint8_t)> ioWriteHook_;
};
