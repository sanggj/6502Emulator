#include "memory.hpp"

#include <fstream>
#include <sstream>

Memory::Memory() {
    reset();
}

void Memory::reset(uint8_t fill) {
    ram_.fill(fill);
    lastWrites_.clear();
    accessHeat_.fill(0);
    markAllDirty();
}

uint8_t Memory::read(uint16_t address) const noexcept {
    uint8_t &h = accessHeat_[address];
    if (h < 0xFF) ++h;
    if (isIoRange(address) && ioReadHook_) return ioReadHook_(address);
    return ram_[address];
}

void Memory::write(uint16_t address, uint8_t value) noexcept {
    uint8_t &h = accessHeat_[address];
    if (h < 0xFE) h += 2;
    if (isIoRange(address) && ioWriteHook_) ioWriteHook_(address, value);
    const uint8_t old = ram_[address];
    if (old == value) return;
    ram_[address] = value;
    ++epoch_;
    dirtyPage_[address >> 6] = epoch_;
    if (lastWrites_.size() < 256) lastWrites_.push_back({address, old, value});
}

void Memory::writeRmw(uint16_t address, uint8_t oldValue, uint8_t newValue) noexcept {
    uint8_t &h = accessHeat_[address];
    if (h < 0xFD) h += 2;
    if (isIoRange(address) && ioWriteHook_) {
        ioWriteHook_(address, oldValue);
        ioWriteHook_(address, newValue);
    }
    ram_[address] = oldValue;
    ram_[address] = newValue;
    ++epoch_;
    dirtyPage_[address >> 6] = epoch_;
    if (lastWrites_.size() < 255) {
        lastWrites_.push_back({address, oldValue, oldValue});
        lastWrites_.push_back({address, oldValue, newValue});
    }
}

bool Memory::loadFile(const std::string &filename, uint16_t startAddress, std::string &error) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        error = "Could not open binary file: " + filename;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return loadBytes(bytes, startAddress, error);
}

bool Memory::loadBytes(const std::vector<uint8_t> &bytes, uint16_t startAddress, std::string &error) {
    if (bytes.size() > SIZE - static_cast<std::size_t>(startAddress)) {
        std::ostringstream out;
        out << "Image of " << bytes.size() << " bytes does not fit at $" << std::hex
            << static_cast<unsigned>(startAddress);
        error = out.str();
        return false;
    }
    if (!bytes.empty()) std::copy(bytes.begin(), bytes.end(), ram_.begin() + startAddress);
    markAllDirty();
    return true;
}

std::vector<uint8_t> Memory::copyRange(uint16_t startAddress, std::size_t count) const {
    const std::size_t safeCount = std::min(count, SIZE - static_cast<std::size_t>(startAddress));
    return std::vector<uint8_t>(ram_.begin() + startAddress, ram_.begin() + startAddress + safeCount);
}

void Memory::fill(uint16_t startAddress, std::size_t count, uint8_t value) noexcept {
    const std::size_t safeCount = std::min(count, SIZE - static_cast<std::size_t>(startAddress));
    for (std::size_t i = 0; i < safeCount; ++i)
        write(static_cast<uint16_t>(static_cast<std::size_t>(startAddress) + i), value);
}

std::vector<uint16_t> Memory::search(const std::vector<uint8_t> &pattern, uint16_t startAddress,
                                     std::size_t count) const {
    std::vector<uint16_t> out;
    if (pattern.empty()) return out;
    const std::size_t start = startAddress;
    const std::size_t safe = std::min(count, SIZE - start);
    if (pattern.size() > safe) return out;
    for (std::size_t i = 0; i + pattern.size() <= safe; ++i) {
        if (std::equal(pattern.begin(), pattern.end(),
                       ram_.begin() + static_cast<std::ptrdiff_t>(start + i))) {
            out.push_back(static_cast<uint16_t>(start + i));
        }
    }
    return out;
}

bool Memory::compare(const Memory &a, const Memory &b, std::vector<Write> &differences, std::size_t limit) {
    differences.clear();
    for (std::size_t i = 0; i < SIZE && differences.size() < limit; ++i) {
        if (a.ram_[i] != b.ram_[i]) differences.push_back({static_cast<uint16_t>(i), a.ram_[i], b.ram_[i]});
    }
    return true;
}

uint32_t Memory::crc32(uint16_t startAddress, std::size_t count) const noexcept {
    const std::size_t safe = std::min(count, SIZE - static_cast<std::size_t>(startAddress));
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < safe; ++i) {
        crc ^= ram_[static_cast<std::size_t>(startAddress) + i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

bool Memory::saveRange(const std::string &filename, uint16_t startAddress, std::size_t count,
                       std::string &error) const {
    std::ofstream f(filename, std::ios::binary);
    if (!f) {
        error = "Could not create binary file: " + filename;
        return false;
    }
    const auto bytes = copyRange(startAddress, count);
    if (!bytes.empty())
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        error = "Could not write binary file: " + filename;
        return false;
    }
    error = "Saved " + std::to_string(bytes.size()) + " bytes";
    return true;
}
