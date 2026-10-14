#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Memory;

namespace hexfile {

bool loadIntelHex(const std::string &path, Memory &memory, std::string &error);
bool saveIntelHex(const Memory &memory, uint16_t start, std::size_t count, const std::string &path,
                  std::string &error);
bool saveSrec(const Memory &memory, uint16_t start, std::size_t count, const std::string &path,
              std::string &error);

} // namespace hexfile
