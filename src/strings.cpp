#include "strings.hpp"

#include <string>
#include <unordered_map>

namespace {

const std::unordered_map<std::string, std::string> &table() {
    static const std::unordered_map<std::string, std::string> strings = {
        {"app.title", "6502 Emulator IDE"},
        {"app.about", "6502 Emulator IDE\nPure C++ / Win32\n\nCPU: NMOS 6502 + unofficial opcodes\nAPU: 2 "
                      "square + triangle + noise (waveOut)\nAssembler v2: macros, includes, conditionals"},
        {"status.ready", "Ready"},
        {"status.running", "Running"},
        {"status.paused", "Paused"},
        {"menu.file", "&File"},
        {"menu.edit", "&Edit"},
        {"menu.build", "&Build"},
        {"menu.debug", "&Debug"},
        {"menu.view", "&View"},
    };
    return strings;
}

} // namespace

const char *tr(const char *key) {
    const auto &t = table();
    const auto it = t.find(key);
    return it == t.end() ? key : it->second.c_str();
}
