#include "hexfile.hpp"
#include "memory.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexByte(const std::string &s, std::size_t pos, uint8_t &out) {
    if (pos + 1 >= s.size()) return false;
    const int hi = hexNib(s[pos]);
    const int lo = hexNib(s[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

} // namespace

namespace hexfile {

bool loadIntelHex(const std::string &path, Memory &memory, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    uint32_t upperBase = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() != ':') continue;
        uint8_t count = 0, addrHi = 0, addrLo = 0, type = 0, checksum = 0;
        if (!hexByte(line, 1, count) || !hexByte(line, 3, addrHi) || !hexByte(line, 5, addrLo) ||
            !hexByte(line, 7, type)) {
            error = "malformed record";
            return false;
        }
        const std::size_t dataStart = 9;
        uint8_t computed = static_cast<uint8_t>(count + addrHi + addrLo + type);
        for (int i = 0; i < count; ++i) {
            uint8_t byte = 0;
            if (!hexByte(line, dataStart + static_cast<std::size_t>(i) * 2, byte)) {
                error = "truncated record";
                return false;
            }
            computed = static_cast<uint8_t>(computed + byte);
        }
        if (!hexByte(line, dataStart + static_cast<std::size_t>(count) * 2, checksum)) {
            error = "missing checksum";
            return false;
        }
        if (static_cast<uint8_t>(computed + checksum) != 0) {
            error = "checksum mismatch";
            return false;
        }

        const uint16_t offset = static_cast<uint16_t>((addrHi << 8) | addrLo);
        switch (type) {
        case 0x00:
            for (int i = 0; i < count; ++i) {
                uint8_t byte = 0;
                hexByte(line, dataStart + static_cast<std::size_t>(i) * 2, byte);
                memory.write(static_cast<uint16_t>(upperBase + offset + i), byte);
            }
            break;
        case 0x01:
            error = "loaded";
            return true;
        case 0x02:
        case 0x04: {
            uint8_t hi = 0, lo = 0;
            hexByte(line, dataStart, hi);
            hexByte(line, dataStart + 2, lo);
            upperBase = type == 0x02 ? static_cast<uint32_t>(((hi << 8) | lo) << 4)
                                     : static_cast<uint32_t>(((hi << 8) | lo) << 16);
            break;
        }
        default:
            break;
        }
    }
    error = "loaded";
    return true;
}

bool saveIntelHex(const Memory &memory, uint16_t start, std::size_t count, const std::string &path,
                  std::string &error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    std::size_t remaining = std::min(count, static_cast<std::size_t>(0x10000 - start));
    uint16_t addr = start;
    char line[128];
    while (remaining > 0) {
        const std::size_t chunk = std::min<std::size_t>(remaining, 16);
        uint8_t checksum = static_cast<uint8_t>(chunk + ((addr >> 8) & 0xFF) + (addr & 0xFF));
        int written = std::snprintf(line, sizeof(line), ":%02zX%04X00", chunk, addr);
        for (std::size_t i = 0; i < chunk; ++i) {
            const uint8_t byte = memory.read(static_cast<uint16_t>(addr + i));
            checksum = static_cast<uint8_t>(checksum + byte);
            written +=
                std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X", byte);
        }
        std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X\n",
                      static_cast<uint8_t>(~checksum + 1));
        out << line;
        addr = static_cast<uint16_t>(addr + chunk);
        remaining -= chunk;
    }
    out << ":00000001FF\n";
    if (!out) {
        error = "write failed";
        return false;
    }
    error = "intel hex saved";
    return true;
}

bool saveSrec(const Memory &memory, uint16_t start, std::size_t count, const std::string &path,
              std::string &error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    std::size_t remaining = std::min(count, static_cast<std::size_t>(0x10000 - start));
    uint16_t addr = start;
    char line[128];
    out << "S00600004844521B\n";
    while (remaining > 0) {
        const std::size_t chunk = std::min<std::size_t>(remaining, 16);
        uint8_t checksum = static_cast<uint8_t>(3 + chunk + ((addr >> 8) & 0xFF) + (addr & 0xFF));
        int written = std::snprintf(line, sizeof(line), "S1%02zX%04X", chunk + 3, addr);
        for (std::size_t i = 0; i < chunk; ++i) {
            const uint8_t byte = memory.read(static_cast<uint16_t>(addr + i));
            checksum = static_cast<uint8_t>(checksum + byte);
            written +=
                std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X", byte);
        }
        std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X\n",
                      static_cast<uint8_t>(~checksum));
        out << line;
        addr = static_cast<uint16_t>(addr + chunk);
        remaining -= chunk;
    }
    uint8_t checksum = static_cast<uint8_t>(3 + ((start >> 8) & 0xFF) + (start & 0xFF));
    checksum = static_cast<uint8_t>(~(checksum + (count & 0xFF) + ((count >> 8) & 0xFF)));
    std::snprintf(line, sizeof(line), "S903%04X%02X\n", start, checksum);
    out << line;
    if (!out) {
        error = "write failed";
        return false;
    }
    error = "srecord saved";
    return true;
}

} // namespace hexfile
