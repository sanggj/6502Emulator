#include "assembler.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using Mode = Assembler::AddrMode;
using Op = Assembler::OpcodeInfo;

namespace AsmEvalImpl {
bool evalExprInt(const std::string &text, int32_t &value, const std::map<std::string, uint16_t> &symbols,
                 const uint16_t *pc, const std::string &scopePrefix, std::string &error);
}

// Thread-local state shared between the assembly driver and the expression parser:
// anonymous label context and the symbol cross-reference sink.
namespace AsmState {
struct AnonState {
    bool enabled = false;
    uint16_t pc = 0;
    const std::vector<uint16_t> *minus = nullptr;
    const std::vector<uint16_t> *plus = nullptr;
};
inline thread_local AnonState anon;

struct RefState {
    std::map<std::string, std::set<int>> *sink = nullptr;
    int line = 0;
    const std::map<std::string, int> *defs = nullptr;
};
inline thread_local RefState refs;

inline void reset() {
    anon = AnonState{};
    refs = RefState{};
}
} // namespace AsmState

#define O(name, mode, opcode, bytes, cycles, penalty) {name, Mode::mode, opcode, bytes, cycles, penalty}
static constexpr Op OPCODES[] = {
    O("ADC", Immediate, 0x69, 2, 2, false), O("ADC", ZeroPage, 0x65, 2, 3, false),
    O("ADC", ZeroPageX, 0x75, 2, 4, false), O("ADC", Absolute, 0x6D, 3, 4, false),
    O("ADC", AbsoluteX, 0x7D, 3, 4, true), O("ADC", AbsoluteY, 0x79, 3, 4, true),
    O("ADC", IndexedIndirect, 0x61, 2, 6, false), O("ADC", IndirectIndexed, 0x71, 2, 5, true),
    O("AND", Immediate, 0x29, 2, 2, false), O("AND", ZeroPage, 0x25, 2, 3, false),
    O("AND", ZeroPageX, 0x35, 2, 4, false), O("AND", Absolute, 0x2D, 3, 4, false),
    O("AND", AbsoluteX, 0x3D, 3, 4, true), O("AND", AbsoluteY, 0x39, 3, 4, true),
    O("AND", IndexedIndirect, 0x21, 2, 6, false), O("AND", IndirectIndexed, 0x31, 2, 5, true),
    O("ASL", Accumulator, 0x0A, 1, 2, false), O("ASL", ZeroPage, 0x06, 2, 5, false),
    O("ASL", ZeroPageX, 0x16, 2, 6, false), O("ASL", Absolute, 0x0E, 3, 6, false),
    O("ASL", AbsoluteX, 0x1E, 3, 7, false), O("BCC", Relative, 0x90, 2, 2, false),
    O("BCS", Relative, 0xB0, 2, 2, false), O("BEQ", Relative, 0xF0, 2, 2, false),
    O("BIT", ZeroPage, 0x24, 2, 3, false), O("BIT", Absolute, 0x2C, 3, 4, false),
    O("BMI", Relative, 0x30, 2, 2, false), O("BNE", Relative, 0xD0, 2, 2, false),
    O("BPL", Relative, 0x10, 2, 2, false), O("BRK", Implied, 0x00, 1, 7, false),
    O("BVC", Relative, 0x50, 2, 2, false), O("BVS", Relative, 0x70, 2, 2, false),
    O("CLC", Implied, 0x18, 1, 2, false), O("CLD", Implied, 0xD8, 1, 2, false),
    O("CLI", Implied, 0x58, 1, 2, false), O("CLV", Implied, 0xB8, 1, 2, false),
    O("CMP", Immediate, 0xC9, 2, 2, false), O("CMP", ZeroPage, 0xC5, 2, 3, false),
    O("CMP", ZeroPageX, 0xD5, 2, 4, false), O("CMP", Absolute, 0xCD, 3, 4, false),
    O("CMP", AbsoluteX, 0xDD, 3, 4, true), O("CMP", AbsoluteY, 0xD9, 3, 4, true),
    O("CMP", IndexedIndirect, 0xC1, 2, 6, false), O("CMP", IndirectIndexed, 0xD1, 2, 5, true),
    O("CPX", Immediate, 0xE0, 2, 2, false), O("CPX", ZeroPage, 0xE4, 2, 3, false),
    O("CPX", Absolute, 0xEC, 3, 4, false), O("CPY", Immediate, 0xC0, 2, 2, false),
    O("CPY", ZeroPage, 0xC4, 2, 3, false), O("CPY", Absolute, 0xCC, 3, 4, false),
    O("DEC", ZeroPage, 0xC6, 2, 5, false), O("DEC", ZeroPageX, 0xD6, 2, 6, false),
    O("DEC", Absolute, 0xCE, 3, 6, false), O("DEC", AbsoluteX, 0xDE, 3, 7, false),
    O("DEX", Implied, 0xCA, 1, 2, false), O("DEY", Implied, 0x88, 1, 2, false),
    O("EOR", Immediate, 0x49, 2, 2, false), O("EOR", ZeroPage, 0x45, 2, 3, false),
    O("EOR", ZeroPageX, 0x55, 2, 4, false), O("EOR", Absolute, 0x4D, 3, 4, false),
    O("EOR", AbsoluteX, 0x5D, 3, 4, true), O("EOR", AbsoluteY, 0x59, 3, 4, true),
    O("EOR", IndexedIndirect, 0x41, 2, 6, false), O("EOR", IndirectIndexed, 0x51, 2, 5, true),
    O("INC", ZeroPage, 0xE6, 2, 5, false), O("INC", ZeroPageX, 0xF6, 2, 6, false),
    O("INC", Absolute, 0xEE, 3, 6, false), O("INC", AbsoluteX, 0xFE, 3, 7, false),
    O("INX", Implied, 0xE8, 1, 2, false), O("INY", Implied, 0xC8, 1, 2, false),
    O("JMP", Absolute, 0x4C, 3, 3, false), O("JMP", Indirect, 0x6C, 3, 5, false),
    O("JSR", Absolute, 0x20, 3, 6, false), O("LDA", Immediate, 0xA9, 2, 2, false),
    O("LDA", ZeroPage, 0xA5, 2, 3, false), O("LDA", ZeroPageX, 0xB5, 2, 4, false),
    O("LDA", Absolute, 0xAD, 3, 4, false), O("LDA", AbsoluteX, 0xBD, 3, 4, true),
    O("LDA", AbsoluteY, 0xB9, 3, 4, true), O("LDA", IndexedIndirect, 0xA1, 2, 6, false),
    O("LDA", IndirectIndexed, 0xB1, 2, 5, true), O("LDX", Immediate, 0xA2, 2, 2, false),
    O("LDX", ZeroPage, 0xA6, 2, 3, false), O("LDX", ZeroPageY, 0xB6, 2, 4, false),
    O("LDX", Absolute, 0xAE, 3, 4, false), O("LDX", AbsoluteY, 0xBE, 3, 4, true),
    O("LDY", Immediate, 0xA0, 2, 2, false), O("LDY", ZeroPage, 0xA4, 2, 3, false),
    O("LDY", ZeroPageX, 0xB4, 2, 4, false), O("LDY", Absolute, 0xAC, 3, 4, false),
    O("LDY", AbsoluteX, 0xBC, 3, 4, true), O("LSR", Accumulator, 0x4A, 1, 2, false),
    O("LSR", ZeroPage, 0x46, 2, 5, false), O("LSR", ZeroPageX, 0x56, 2, 6, false),
    O("LSR", Absolute, 0x4E, 3, 6, false), O("LSR", AbsoluteX, 0x5E, 3, 7, false),
    O("NOP", Implied, 0xEA, 1, 2, false), O("ORA", Immediate, 0x09, 2, 2, false),
    O("ORA", ZeroPage, 0x05, 2, 3, false), O("ORA", ZeroPageX, 0x15, 2, 4, false),
    O("ORA", Absolute, 0x0D, 3, 4, false), O("ORA", AbsoluteX, 0x1D, 3, 4, true),
    O("ORA", AbsoluteY, 0x19, 3, 4, true), O("ORA", IndexedIndirect, 0x01, 2, 6, false),
    O("ORA", IndirectIndexed, 0x11, 2, 5, true), O("PHA", Implied, 0x48, 1, 3, false),
    O("PHP", Implied, 0x08, 1, 3, false), O("PLA", Implied, 0x68, 1, 4, false),
    O("PLP", Implied, 0x28, 1, 4, false), O("ROL", Accumulator, 0x2A, 1, 2, false),
    O("ROL", ZeroPage, 0x26, 2, 5, false), O("ROL", ZeroPageX, 0x36, 2, 6, false),
    O("ROL", Absolute, 0x2E, 3, 6, false), O("ROL", AbsoluteX, 0x3E, 3, 7, false),
    O("ROR", Accumulator, 0x6A, 1, 2, false), O("ROR", ZeroPage, 0x66, 2, 5, false),
    O("ROR", ZeroPageX, 0x76, 2, 6, false), O("ROR", Absolute, 0x6E, 3, 6, false),
    O("ROR", AbsoluteX, 0x7E, 3, 7, false), O("RTI", Implied, 0x40, 1, 6, false),
    O("RTS", Implied, 0x60, 1, 6, false), O("SBC", Immediate, 0xE9, 2, 2, false),
    O("SBC", ZeroPage, 0xE5, 2, 3, false), O("SBC", ZeroPageX, 0xF5, 2, 4, false),
    O("SBC", Absolute, 0xED, 3, 4, false), O("SBC", AbsoluteX, 0xFD, 3, 4, true),
    O("SBC", AbsoluteY, 0xF9, 3, 4, true), O("SBC", IndexedIndirect, 0xE1, 2, 6, false),
    O("SBC", IndirectIndexed, 0xF1, 2, 5, true), O("SEC", Implied, 0x38, 1, 2, false),
    O("SED", Implied, 0xF8, 1, 2, false), O("SEI", Implied, 0x78, 1, 2, false),
    O("STA", ZeroPage, 0x85, 2, 3, false), O("STA", ZeroPageX, 0x95, 2, 4, false),
    O("STA", Absolute, 0x8D, 3, 4, false), O("STA", AbsoluteX, 0x9D, 3, 5, false),
    O("STA", AbsoluteY, 0x99, 3, 5, false), O("STA", IndexedIndirect, 0x81, 2, 6, false),
    O("STA", IndirectIndexed, 0x91, 2, 6, false), O("STX", ZeroPage, 0x86, 2, 3, false),
    O("STX", ZeroPageY, 0x96, 2, 4, false), O("STX", Absolute, 0x8E, 3, 4, false),
    O("STY", ZeroPage, 0x84, 2, 3, false), O("STY", ZeroPageX, 0x94, 2, 4, false),
    O("STY", Absolute, 0x8C, 3, 4, false), O("TAX", Implied, 0xAA, 1, 2, false),
    O("TAY", Implied, 0xA8, 1, 2, false), O("TSX", Implied, 0xBA, 1, 2, false),
    O("TXA", Implied, 0x8A, 1, 2, false), O("TXS", Implied, 0x9A, 1, 2, false),
    O("TYA", Implied, 0x98, 1, 2, false), O("SLO", IndexedIndirect, 0x03, 2, 8, false),
    O("SLO", ZeroPage, 0x07, 2, 5, false), O("SLO", Absolute, 0x0F, 3, 6, false),
    O("SLO", IndirectIndexed, 0x13, 2, 8, false), O("SLO", ZeroPageX, 0x17, 2, 6, false),
    O("SLO", AbsoluteY, 0x1B, 3, 7, false), O("SLO", AbsoluteX, 0x1F, 3, 7, false),
    O("RLA", IndexedIndirect, 0x23, 2, 8, false), O("RLA", ZeroPage, 0x27, 2, 5, false),
    O("RLA", Absolute, 0x2F, 3, 6, false), O("RLA", IndirectIndexed, 0x33, 2, 8, false),
    O("RLA", ZeroPageX, 0x37, 2, 6, false), O("RLA", AbsoluteY, 0x3B, 3, 7, false),
    O("RLA", AbsoluteX, 0x3F, 3, 7, false), O("SRE", IndexedIndirect, 0x43, 2, 8, false),
    O("SRE", ZeroPage, 0x47, 2, 5, false), O("SRE", Absolute, 0x4F, 3, 6, false),
    O("SRE", IndirectIndexed, 0x53, 2, 8, false), O("SRE", ZeroPageX, 0x57, 2, 6, false),
    O("SRE", AbsoluteY, 0x5B, 3, 7, false), O("SRE", AbsoluteX, 0x5F, 3, 7, false),
    O("RRA", IndexedIndirect, 0x63, 2, 8, false), O("RRA", ZeroPage, 0x67, 2, 5, false),
    O("RRA", Absolute, 0x6F, 3, 6, false), O("RRA", IndirectIndexed, 0x73, 2, 8, false),
    O("RRA", ZeroPageX, 0x77, 2, 6, false), O("RRA", AbsoluteY, 0x7B, 3, 7, false),
    O("RRA", AbsoluteX, 0x7F, 3, 7, false), O("SAX", IndexedIndirect, 0x83, 2, 6, false),
    O("SAX", ZeroPage, 0x87, 2, 3, false), O("SAX", Absolute, 0x8F, 3, 4, false),
    O("SAX", ZeroPageY, 0x97, 2, 4, false), O("LAX", IndexedIndirect, 0xA3, 2, 6, false),
    O("LAX", ZeroPage, 0xA7, 2, 3, false), O("LAX", Immediate, 0xAB, 2, 2, false),
    O("LAX", Absolute, 0xAF, 3, 4, false), O("LAX", IndirectIndexed, 0xB3, 2, 5, true),
    O("LAX", ZeroPageY, 0xB7, 2, 4, false), O("LAX", AbsoluteY, 0xBF, 3, 4, true),
    O("DCP", IndexedIndirect, 0xC3, 2, 8, false), O("DCP", ZeroPage, 0xC7, 2, 5, false),
    O("DCP", Absolute, 0xCF, 3, 6, false), O("DCP", IndirectIndexed, 0xD3, 2, 8, false),
    O("DCP", ZeroPageX, 0xD7, 2, 6, false), O("DCP", AbsoluteY, 0xDB, 3, 7, false),
    O("DCP", AbsoluteX, 0xDF, 3, 7, false), O("ISC", IndexedIndirect, 0xE3, 2, 8, false),
    O("ISC", ZeroPage, 0xE7, 2, 5, false), O("ISC", Absolute, 0xEF, 3, 6, false),
    O("ISC", IndirectIndexed, 0xF3, 2, 8, false), O("ISC", ZeroPageX, 0xF7, 2, 6, false),
    O("ISC", AbsoluteY, 0xFB, 3, 7, false), O("ISC", AbsoluteX, 0xFF, 3, 7, false),
    O("ANC", Immediate, 0x0B, 2, 2, false), O("ALR", Immediate, 0x4B, 2, 2, false),
    O("ARR", Immediate, 0x6B, 2, 2, false), O("SBX", Immediate, 0xCB, 2, 2, false),
    O("XAA", Immediate, 0x8B, 2, 2, false), O("SHY", AbsoluteX, 0x9C, 3, 5, false),
    O("SHX", AbsoluteY, 0x9E, 3, 5, false), O("SHA", IndexedIndirect, 0x93, 2, 6, false),
    O("SHA", AbsoluteY, 0x9F, 3, 5, false), O("TAS", AbsoluteY, 0x9B, 3, 5, false),
    O("LAS", AbsoluteY, 0xBB, 3, 4, true), O("NOP", Immediate, 0x80, 2, 2, false),
    O("NOP", Immediate, 0x89, 2, 2, false), O("NOP", Immediate, 0xC2, 2, 2, false),
    O("NOP", Immediate, 0xE2, 2, 2, false), O("NOP", Immediate, 0x82, 2, 2, false),
    O("NOP", ZeroPage, 0x04, 2, 3, false), O("NOP", ZeroPage, 0x44, 2, 3, false),
    O("NOP", ZeroPage, 0x64, 2, 3, false), O("NOP", ZeroPageX, 0x14, 2, 4, false),
    O("NOP", ZeroPageX, 0x34, 2, 4, false), O("NOP", ZeroPageX, 0x54, 2, 4, false),
    O("NOP", ZeroPageX, 0x74, 2, 4, false), O("NOP", ZeroPageX, 0xD4, 2, 4, false),
    O("NOP", ZeroPageX, 0xF4, 2, 4, false), O("NOP", Absolute, 0x0C, 3, 4, false),
    O("NOP", AbsoluteX, 0x1C, 3, 4, true), O("NOP", AbsoluteX, 0x3C, 3, 4, true),
    O("NOP", AbsoluteX, 0x5C, 3, 4, true), O("NOP", AbsoluteX, 0x7C, 3, 4, true),
    O("NOP", AbsoluteX, 0xDC, 3, 4, true), O("NOP", AbsoluteX, 0xFC, 3, 4, true),
    O("JAM", Implied, 0x02, 1, 2, false),
    // 65C02 / extension opcodes appended at the END of the table on purpose: findOpcodeByByte
    // returns the FIRST match so NMOS disassembly keeps winning for duplicated encodings.
    O("BRA", Relative, 0x80, 2, 2, false), O("INA", Implied, 0x1A, 1, 2, false),
    O("DEA", Implied, 0x3A, 1, 2, false), O("PHY", Implied, 0x5A, 1, 3, false),
    O("PLY", Implied, 0x7A, 1, 4, false), O("PHX", Implied, 0xDA, 1, 3, false),
    O("PLX", Implied, 0xFA, 1, 4, false), O("TSB", ZeroPage, 0x04, 2, 5, false),
    O("TSB", Absolute, 0x0C, 3, 6, false), O("TRB", ZeroPage, 0x14, 2, 5, false),
    O("TRB", Absolute, 0x1C, 3, 6, false), O("STZ", ZeroPage, 0x64, 2, 3, false),
    O("STZ", ZeroPageX, 0x74, 2, 4, false), O("STZ", Absolute, 0x9C, 3, 4, false),
    O("STZ", AbsoluteX, 0x9E, 3, 5, false), O("BIT", Immediate, 0x89, 2, 2, false),
    O("BIT", ZeroPageX, 0x34, 2, 4, false), O("BIT", AbsoluteX, 0x3C, 3, 4, true),
    O("JMP", IndirectAbsoluteX, 0x7C, 3, 6, false), O("WAI", Implied, 0xCB, 1, 2, false),
    O("STP", Implied, 0xDB, 1, 2, false)};
#undef O

namespace {

enum DirTok : uint8_t { DT_NONE, DT_MACRO, DT_ENDM, DT_REPEAT, DT_ENDR, DT_INCLUDE, DT_BREAK };

DirTok classifyHead(const std::string &headUpper) {
    if (headUpper == ".MACRO") return DT_MACRO;
    if (headUpper == ".ENDM") return DT_ENDM;
    if (headUpper == ".REPEAT") return DT_REPEAT;
    if (headUpper == ".ENDR") return DT_ENDR;
    if (headUpper == ".INCLUDE") return DT_INCLUDE;
    if (headUpper == ".BREAK") return DT_BREAK;
    return DT_NONE;
}

std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool isIdentifier(const std::string &s) {
    if (s.empty()) return false;
    const char c0 = s[0];
    if (!(std::isalpha(static_cast<unsigned char>(c0)) || c0 == '_' || c0 == '@')) return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    });
}

int digitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "-" / "+" / "--" / "++" ... : anonymous label definition or reference.
bool isAnonToken(const std::string &s) {
    if (s.empty()) return false;
    const char c0 = s[0];
    if (c0 != '-' && c0 != '+') return false;
    return std::all_of(s.begin(), s.end(), [c0](char c) { return c == c0; });
}

std::string stripComment(const std::string &raw) {
    bool quote = false;
    char q = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        // Inside a quoted string the char after '\' is data, so an escaped quote
        // ("a\"") must not close the string and reveal a later ';' as a comment.
        if (quote && c == '\\' && i + 1 < raw.size()) {
            ++i;
            continue;
        }
        if ((c == '"' || c == '\'')) {
            if (quote && c == q) {
                quote = false;
                q = 0;
            } else if (!quote) {
                quote = true;
                q = c;
            }
        } else if (c == ';' && !quote) {
            return raw.substr(0, i);
        }
    }
    return raw;
}

std::string textAfterLabels(const std::string &stripped) {
    std::string s = trim(stripped);
    while (true) {
        const auto colon = s.find(':');
        if (colon == std::string::npos || colon == 0) return s;
        bool spaced = true;
        for (std::size_t i = 0; i < colon; ++i)
            if (std::isspace(static_cast<unsigned char>(s[i]))) {
                spaced = false;
                break;
            }
        if (!spaced) return s;
        s = trim(s.substr(colon + 1));
        if (s.empty()) return s;
    }
}

// 1-based column of the first non-blank character (label field start).
int labelColumnOf(const std::string &raw) {
    for (std::size_t i = 0; i < raw.size(); ++i)
        if (raw[i] != ' ' && raw[i] != '\t') return static_cast<int>(i) + 1;
    return 1;
}

// Best-effort 1-based column of a statement's operand area: skip leading blanks and
// "label:" prefixes, then the mnemonic token (plus a following directive token when
// present, e.g. "NAME .equ 3"). Used whenever an exact error position is impractical.
int operandColumnOf(const std::string &raw) {
    const std::string s = stripComment(raw);
    const std::size_t n = s.size();
    auto isWS = [&](char c) { return c == ' ' || c == '\t'; };
    auto wc = [&](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@' || c == '-' ||
               c == '+';
    };
    std::size_t i = 0;
    while (i < n && isWS(s[i])) ++i;
    bool progressed = true;
    while (progressed) {
        progressed = false;
        const std::size_t start = i;
        while (i < n && wc(s[i])) ++i;
        if (i > start && i < n && s[i] == ':') {
            ++i;
            while (i < n && isWS(s[i])) ++i;
            progressed = true;
        } else {
            i = start;
        }
    }
    // Skip mnemonic token.
    while (i < n && !isWS(s[i])) ++i;
    while (i < n && isWS(s[i])) ++i;
    const std::size_t operandStart = i;
    // For "NAME .equ value" style statements skip the directive token as well, but
    // only when more content follows (never swallow the whole operands area).
    if (i < n && s[i] == '.') {
        std::size_t d = i;
        while (d < n && !isWS(s[d])) ++d;
        while (d < n && isWS(s[d])) ++d;
        if (d < n) return static_cast<int>(d) + 1;
    }
    if (operandStart >= n) return 1;
    return static_cast<int>(operandStart) + 1;
}

std::string joinPathStr(const std::string &dir, const std::string &rel) {
    if (dir.empty()) return rel;
    try {
        return (std::filesystem::path(dir) / rel).string();
    } catch (...) {
        return rel;
    }
}

// Resolve ".include" targets: including file's directory first, then each configured
// search path in order, then the current working directory. First existing file wins.
bool resolveIncludePath(const std::string &rel, const std::string &primaryDir,
                        const std::vector<std::string> &searchPaths, std::string &chosen,
                        std::vector<std::string> &tried) {
    std::vector<std::string> candidates;
    if (!rel.empty() && std::filesystem::path(rel).is_absolute()) {
        candidates.push_back(rel);
    } else {
        candidates.push_back(joinPathStr(primaryDir, rel));
        for (const std::string &p : searchPaths) candidates.push_back(joinPathStr(p, rel));
        candidates.push_back(rel); // current working directory
    }
    for (const std::string &cand : candidates) {
        tried.push_back(cand);
        std::error_code ec;
        if (std::filesystem::is_regular_file(cand, ec)) {
            chosen = cand;
            return true;
        }
    }
    return false;
}

struct SourceLine {
    std::string text;
    int number = 0;        // sequential id inside the assembled stream
    int displayNumber = 0; // user-facing source line number
    std::string fileName;
    std::string dir; // directory used to resolve .include found on this line
};

struct MacroDef {
    std::vector<std::string> params;
    std::vector<bool> hasDefault;
    std::vector<std::string> defaults;
    std::vector<SourceLine> body;
};

bool readWholeFile(const std::string &path, std::string &out, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open include file: " + path;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string substituteWord(const std::string &text, const std::string &from, const std::string &to) {
    if (from.empty()) return text;
    auto wordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@';
    };
    std::string result;
    result.reserve(text.size() + to.size() * 2);
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.size() - i >= from.size() && _strnicmp(text.c_str() + i, from.c_str(), from.size()) == 0 &&
            (i == 0 || !wordChar(text[i - 1])) &&
            (i + from.size() >= text.size() || !wordChar(text[i + from.size()]))) {
            result += to;
            i += from.size();
            continue;
        }
        result += text[i++];
    }
    return result;
}

struct Preprocessor {
    std::vector<AssemblerError> *errors = nullptr;
    std::size_t budget = 200000;
    unsigned uniqueCounter = 0;
    std::vector<std::string> includePaths;
    // Sequential ids for lines coming from included files start above the top-level
    // range so error-attribution lookups never collide.
    int seqBase = 1000000;
    int seqExtra = 0;
    // Constants visible at expansion time: options.predefinedSymbols plus ".equ"/
    // ".define" values defined earlier in the flattened source (labels cannot be
    // known here; pass 1 owns those). ".repeat" counts, ".break if" and macro
    // parameter defaults evaluate against this table instead of an empty one.
    std::map<std::string, uint16_t> constSyms;

    // Preprocessor-side mirror of pass 1's conditional state (pass 1 owns the
    // real one; see condStackPass1) so ".equ"/".define" inside a provably
    // FALSE ".if" branch is not recorded below - a later ".repeat"/macro
    // default must not see a symbol the conditional logic never defines.
    // Conservative by design: conditions that cannot be evaluated here (label
    // references are pass-1 data, and ".ifdef" of a not-yet-known name) are
    // marked unknown and treated as ACTIVE, i.e. recorded exactly as before -
    // this mirror only ever skips recording it can PROVE unreachable, so it
    // can never hide a symbol a later ".repeat" legitimately needs.
    struct CondPre {
        bool parentActive;
        bool known; // false: condition not evaluable during preprocessing
        bool cond;
        bool inElse;
    };
    std::vector<CondPre> condStack;
    bool condActivePre() const {
        for (const auto &f : condStack) {
            const bool active =
                f.parentActive && (!f.known || (f.inElse ? !f.cond : f.cond));
            if (!active) return false;
        }
        return true;
    }

    bool fail(const SourceLine &at, const std::string &msg) {
        errors->push_back({at.number, msg, operandColumnOf(at.text), at.fileName});
        return false;
    }

    bool run(const std::vector<SourceLine> &raw, const std::string &dir, std::size_t maxDepth,
             std::vector<SourceLine> &out) {
        std::unordered_set<std::string> once;
        std::vector<SourceLine> flat;
        if (!expandIncludes(raw, dir, 0, maxDepth, once, flat)) return false;
        std::unordered_map<std::string, MacroDef> macros;
        bool broke = false;
        return processBlock(flat, 0, flat.size(), out, macros, 0, false, broke);
    }

    bool expandIncludes(const std::vector<SourceLine> &raw, const std::string &dir, int depth,
                        std::size_t maxDepth, std::unordered_set<std::string> &once,
                        std::vector<SourceLine> &flat) {
        if (static_cast<std::size_t>(depth) > maxDepth) {
            if (!raw.empty()) fail(raw.front(), "include depth limit exceeded");
            return false;
        }
        for (const SourceLine &src : raw) {
            const std::string t = trim(stripComment(src.text));
            std::istringstream tokens(textAfterLabels(t));
            std::string head;
            tokens >> head;
            if (classifyHead(upper(head)) == DT_INCLUDE) {
                std::string rest;
                std::getline(tokens, rest);
                rest = trim(rest);
                if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
                    const std::string rel = rest.substr(1, rest.size() - 2);
                    const std::string primaryDir = src.dir.empty() ? dir : src.dir;
                    std::string canonical;
                    try {
                        const auto p = std::filesystem::weakly_canonical(
                            rel.empty() || std::filesystem::path(rel).is_absolute()
                                ? std::filesystem::path(rel)
                                : std::filesystem::path(primaryDir) / rel);
                        canonical = p.string();
                    } catch (...) {
                    }
                    if (canonical.empty()) canonical = joinPathStr(primaryDir, rel);
                    if (once.count(canonical)) continue;
                    std::string chosen;
                    std::vector<std::string> tried;
                    if (!resolveIncludePath(rel, primaryDir, includePaths, chosen, tried)) {
                        std::string list;
                        for (std::size_t k = 0; k < tried.size(); ++k) {
                            if (k) list += ", ";
                            list += tried[k];
                        }
                        errors->push_back({src.number,
                                           ".include cannot find \"" + rel + "\" (tried: " + list + ")",
                                           operandColumnOf(src.text), src.fileName});
                        return false;
                    }
                    try {
                        const auto p = std::filesystem::weakly_canonical(chosen);
                        canonical = p.string();
                    } catch (...) {
                    }
                    once.insert(canonical);
                    std::string content;
                    std::string err;
                    if (!readWholeFile(canonical, content, err)) {
                        errors->push_back({src.number, err, operandColumnOf(src.text), src.fileName});
                        return false;
                    }
                    std::vector<SourceLine> included;
                    std::istringstream stream(content);
                    std::string lineText;
                    int number = 0;
                    const std::string childDir = std::filesystem::path(canonical).parent_path().string();
                    while (std::getline(stream, lineText)) {
                        ++number;
                        included.push_back({lineText, seqBase + (++seqExtra), number, canonical, childDir});
                    }
                    const SourceLine guard{"", seqBase + (++seqExtra), src.displayNumber, canonical,
                                           childDir};
                    if (included.empty()) included.push_back(guard);
                    if (!expandIncludes(included, childDir, depth + 1, maxDepth, once, flat)) return false;
                    continue;
                }
                return fail(src, ".include expects \"file\"");
            }
            flat.push_back(src);
        }
        return true;
    }

    bool processBlock(const std::vector<SourceLine> &in, std::size_t begin, std::size_t end,
                      std::vector<SourceLine> &out, std::unordered_map<std::string, MacroDef> &macros,
                      int depth, bool inRepeat, bool &broke) {
        if (depth > 24)
            return in.empty() || fail(in[begin < end ? begin : end - 1], "expansion nesting too deep");
        for (std::size_t i = begin; i < end;) {
            const SourceLine &src = in[i];
            const std::string strippedAll = trim(stripComment(src.text));
            if (strippedAll.empty()) {
                out.push_back(src);
                ++i;
                continue;
            }
            // Conditional bookkeeping FIRST: mirror pass 1's .if/.else/.endif
            // state (see CondPre above) so the ".equ"/".define" recording below
            // can be gated on it. The directive lines themselves still fall
            // through and are emitted unchanged for pass 1.
            const Assembler::ParsedLine pl = Assembler::parseLine(src.text);
            const bool isIfPre = (pl.mnemonic == ".IF" || pl.mnemonic == ".IFDEF" ||
                                  pl.mnemonic == ".IFNDEF");
            const bool isElseLikePre = (pl.mnemonic == ".ELSE" || pl.mnemonic == ".ELIF");
            const bool isEndifPre = (pl.mnemonic == ".ENDIF");
            if (isIfPre || isElseLikePre || isEndifPre) {
                if (isIfPre) {
                    const bool parentActive = condActivePre();
                    bool known = false;
                    bool truth = true;
                    if (pl.mnemonic == ".IF") {
                        int32_t cv = 0;
                        std::string cerr;
                        if (evaluateConst(pl.operands, cv, cerr)) {
                            known = true;
                            truth = (cv != 0);
                        } // else: unevaluable here (labels) -> unknown -> active
                    } else {
                        // .IFDEF/.IFNDEF: only a name already in the constant
                        // table is decidable now; absence may still be a
                        // pass-1 label, so that stays unknown -> active.
                        std::istringstream in(pl.operands);
                        std::string name;
                        in >> name;
                        if (!name.empty() && constSyms.count(normalizeSymbolName(name))) {
                            known = true;
                            truth = (pl.mnemonic == ".IFDEF");
                        }
                    }
                    condStack.push_back({parentActive, known, truth, false});
                } else if (isElseLikePre) {
                    if (!condStack.empty()) {
                        CondPre &f = condStack.back();
                        if (pl.mnemonic == ".ELSE") {
                            if (!f.inElse) f.inElse = true;
                            // unknown frames stay active in both branches
                        } else if (f.known && !f.inElse) {
                            // .ELIF: mirror pass 1 (cond/inElse update) only
                            // while the chain is still decidable here.
                            int32_t cv = 0;
                            std::string cerr;
                            if (evaluateConst(pl.operands, cv, cerr)) {
                                f.cond = f.cond ? false : (cv != 0);
                                f.inElse = f.cond;
                            } else {
                                f.known = false; // degrade to unknown -> active
                            }
                        }
                    }
                } else if (!condStack.empty()) {
                    condStack.pop_back();
                }
            }
            // Record ".equ"/".define" constants in source order so a later ".repeat"
            // count or macro default can use them. Failures are skipped silently:
            // pass 1 re-evaluates with the full symbol table and reports real errors.
            // Skipped while inside an inactive conditional branch: pass 1 would never
            // define the symbol there, so recording it would let a later ".repeat"
            // expand against a symbol that cannot exist (divergent expansion).
            if ((pl.mnemonic == ".EQU" || pl.mnemonic == ".DEFINE") && condActivePre()) {
                std::string nameKey = pl.label;
                std::string valueExpr = pl.operands;
                if (nameKey.empty()) {
                    std::istringstream parts(pl.operands);
                    std::string firstName;
                    parts >> firstName;
                    std::getline(parts, valueExpr);
                    valueExpr = trim(valueExpr);
                    nameKey = Assembler::normalizeSymbol(firstName);
                }
                int32_t cv = 0;
                std::string cerr;
                if (!nameKey.empty() && evaluateConst(valueExpr, cv, cerr))
                    constSyms[nameKey] = static_cast<uint16_t>(cv & 0xFFFF);
            }
            std::string labelPrefix;
            std::string bodyText = strippedAll;
            {
                const auto colon = strippedAll.find(':');
                if (colon != std::string::npos && colon > 0) {
                    const std::string maybeLabel = trim(strippedAll.substr(0, colon));
                    const std::string afterLabel = trim(strippedAll.substr(colon + 1));
                    bool spaced = !maybeLabel.empty();
                    for (char c : maybeLabel)
                        if (std::isspace(static_cast<unsigned char>(c))) {
                            spaced = false;
                            break;
                        }
                    if (spaced && !afterLabel.empty()) {
                        labelPrefix = maybeLabel + ":";
                        bodyText = afterLabel;
                    }
                }
            }
            std::istringstream tokens(bodyText);
            std::string head;
            tokens >> head;
            const std::string hu = upper(head);
            const DirTok tok = classifyHead(hu);
            if ((tok == DT_MACRO || tok == DT_REPEAT) && !labelPrefix.empty()) {
                out.push_back({labelPrefix, src.number, src.displayNumber, src.fileName, src.dir});
            }
            if (tok == DT_ENDM || tok == DT_ENDR) return fail(src, std::string(head) + " without opener");
            if (tok == DT_BREAK) {
                if (!inRepeat) return fail(src, ".break outside of .repeat");
                std::string rest;
                std::getline(tokens, rest);
                rest = trim(rest);
                bool doBreak = true;
                if (!rest.empty()) {
                    std::istringstream is(rest);
                    std::string kw;
                    is >> kw;
                    std::string exprText;
                    std::getline(is, exprText);
                    exprText = trim(exprText);
                    if (upper(kw) != "IF") return fail(src, ".break expects optional 'if <expression>'");
                    if (exprText.empty()) return fail(src, ".break if requires an expression");
                    int32_t cv = 0;
                    std::string cerr;
                    if (!evaluateConst(exprText, cv, cerr)) return fail(src, ".break: " + cerr);
                    doBreak = (cv != 0);
                }
                if (doBreak) {
                    broke = true;
                    return true; // stop this iteration; enclosing repeat stops looping
                }
                ++i; // condition false: keep processing the remainder of the block
                continue;
            }
            if (tok == DT_MACRO) {
                std::string rest;
                std::getline(tokens, rest);
                std::istringstream parts(trim(rest));
                std::string name;
                parts >> name;
                if (!isIdentifier(name)) return fail(src, ".macro requires a valid name");
                MacroDef def;
                // Parameter list: comma- and/or whitespace-separated tokens;
                // each token is "name" or "name=default" (defaults are constant
                // expressions without spaces).
                std::string param;
                while (std::getline(parts, param, ',')) {
                    std::istringstream cs(trim(param));
                    std::string tok;
                    while (cs >> tok) {
                        std::string pname = tok;
                        std::string pdefault;
                        bool hasDefault = false;
                        const auto eq = tok.find('=');
                        if (eq != std::string::npos) {
                            pname = trim(tok.substr(0, eq));
                            pdefault = trim(tok.substr(eq + 1));
                            hasDefault = true;
                            if (pdefault.empty())
                                return fail(src, ".macro parameter '" + pname + "' has empty default");
                        }
                        if (!isIdentifier(pname)) return fail(src, ".macro requires valid parameter names");
                        def.params.push_back(normalizeSymbolName(pname));
                        def.hasDefault.push_back(hasDefault);
                        def.defaults.push_back(pdefault);
                    }
                }
                ++i;
                const std::size_t bodyBegin = i;
                while (i < end) {
                    std::istringstream bt(textAfterLabels(trim(stripComment(in[i].text))));
                    std::string bh;
                    bt >> bh;
                    if (upper(bh) == ".ENDM") break;
                    ++i;
                }
                if (i >= end) return fail(src, ".macro missing .endm");
                def.body.assign(in.begin() + static_cast<std::ptrdiff_t>(bodyBegin),
                                in.begin() + static_cast<std::ptrdiff_t>(i));
                macros[upper(name)] = std::move(def);
                ++i;
                continue;
            }
            if (tok == DT_REPEAT) {
                std::string countText;
                std::getline(tokens, countText);
                int32_t n = 0;
                std::string err;
                if (!evaluateConst(countText, n, err)) return fail(src, ".repeat: " + err);
                if (n < 0 || static_cast<uint32_t>(n) > 65536u)
                    return fail(src, ".repeat count out of range");
                ++i;
                const std::size_t blockBegin = i;
                int nest = 1;
                while (i < end) {
                    std::istringstream bt(textAfterLabels(trim(stripComment(in[i].text))));
                    std::string bh;
                    bt >> bh;
                    const std::string bhu = upper(bh);
                    if (bhu == ".REPEAT")
                        ++nest;
                    else if (bhu == ".ENDR") {
                        if (--nest == 0) break;
                    }
                    ++i;
                }
                if (i >= end) return fail(src, ".repeat missing .endr");
                const std::size_t blockEnd = i;
                for (int32_t r = 0; r < n; ++r) {
                    if (out.size() > budget) return fail(src, ".repeat expansion exceeds budget");
                    bool innerBroke = false;
                    if (!processBlock(in, blockBegin, blockEnd, out, macros, depth + 1, true, innerBroke))
                        return false;
                    if (innerBroke) break; // innermost repeat consumes the break
                }
                ++i;
                continue;
            }
            const auto slash = bodyText.find_first_of(" \t");
            const std::string firstToken = slash == std::string::npos ? bodyText : bodyText.substr(0, slash);
            const auto it = macros.find(upper(firstToken));
            if (it == macros.end()) {
                out.push_back(src);
                ++i;
                continue;
            }
            const MacroDef def = it->second;
            std::string argText = slash == std::string::npos ? std::string{} : trim(bodyText.substr(slash));
            const auto args = Assembler::splitOperands(argText);
            if (args.size() > def.params.size())
                return fail(src, "too many arguments for macro " + firstToken);
            // Missing trailing arguments fall back to declared default values
            // (constant expressions evaluated lazily at expansion time).
            std::vector<std::string> effArgs = args;
            for (std::size_t p = args.size(); p < def.params.size(); ++p) {
                if (p < def.hasDefault.size() && def.hasDefault[p]) {
                    int32_t dv = 0;
                    std::string derr;
                    if (!evaluateConst(def.defaults[p], dv, derr))
                        return fail(src, "macro '" + firstToken + "' default for '" + def.params[p] +
                                             "': " + derr);
                    effArgs.push_back(std::to_string(dv));
                } else {
                    effArgs.push_back("0");
                }
            }
            ++uniqueCounter;
            const std::string uid = std::to_string(uniqueCounter);
            std::vector<SourceLine> expanded;
            for (const SourceLine &bodyLine : def.body) {
                std::string replaced = bodyLine.text;
                for (std::size_t p = 0; p < def.params.size(); ++p)
                    replaced = substituteWord(replaced, def.params[p], p < effArgs.size() ? effArgs[p] : "0");
                replaced = substituteWord(replaced, "\\@", uid);
                expanded.push_back({replaced, src.number, src.displayNumber, src.fileName, src.dir});
                if (expanded.size() > budget) return fail(src, "macro expansion exceeds budget");
            }
            if (!processBlock(expanded, 0, expanded.size(), out, macros, depth + 1, inRepeat, broke))
                return false;
            if (broke) return true; // .break fired inside the macro: propagate outward
            ++i;
        }
        return true;
    }

    static std::string normalizeSymbolName(const std::string &s) { return Assembler::normalizeSymbol(s); }

    bool evaluateConst(const std::string &text, int32_t &value, std::string &error) {
        return AsmEvalImpl::evalExprInt(text, value, constSyms, nullptr, "", error);
    }
};

} // namespace

namespace AsmEvalImpl {

constexpr double kPi = 3.14159265358979323846;

class Parser {
  public:
    Parser(const std::string &text, const std::map<std::string, uint16_t> &symbols, const uint16_t *pc,
           const std::string &scopePrefix, std::string &error)
        : s_(trim(text)), pos_(0), symbols_(symbols), pc_(pc), scope_(scopePrefix), error_(error) {}
    bool run(int32_t &out) {
        skip();
        if (!expr(0, out)) return false;
        skip();
        if (pos_ != s_.size()) {
            return fail("unexpected trailing characters in expression");
        }
        return true;
    }

  private:
    static int binPrec(char op, bool shift) {
        if (shift) return 6;
        switch (op) {
        case '|':
            return 1;
        case '^':
            return 2;
        case '&':
            return 3;
        case '=':
        case '!':
            return 4;
        case '<':
        case '>':
            return 5;
        case '+':
        case '-':
            return 7;
        case '*':
        case '/':
        case '%':
            return 8;
        default:
            return 0;
        }
    }

    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
    }

    bool fail(const std::string &message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool expr(int minPrec, int32_t &value) {
        if (!unary(value)) return false;
        while (true) {
            skip();
            if (pos_ >= s_.size()) return true;
            char op = s_[pos_];
            bool shift = false;
            int prec = 0;
            if ((op == '<' || op == '>') && pos_ + 1 < s_.size() && s_[pos_ + 1] == op) {
                shift = true;
                prec = binPrec(op, true);
            } else if (op == '=' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '=') {
                ++pos_;
                prec = binPrec('=', false);
            } else if (op == '!' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '=') {
                ++pos_;
                prec = binPrec('!', false);
            } else if ((op == '<' || op == '>') && pos_ + 1 < s_.size() && s_[pos_ + 1] == '=') {
                ++pos_;
                prec = binPrec(op, false);
            } else
                prec = binPrec(op, false);
            if (prec <= minPrec || prec == 0) return true;
            pos_ += shift ? 2 : 1;
            int32_t rhs = 0;
            if (!expr(prec, rhs)) return false;
            switch (shift ? static_cast<char>(std::tolower(static_cast<unsigned char>(op)) == '<' ? 'l' : 'r')
                          : op) {
            // All +,-,* and INT32_MIN edge cases go through uint32_t so wrapping is
            // defined (no signed-overflow UB); values re-enter as int32_t bit patterns
            // and the 16-bit range check at the eval boundary still reports errors.
            case '|':
                value |= rhs;
                break;
            case '^':
                value ^= rhs;
                break;
            case '&':
                value &= rhs;
                break;
            case '=':
                value = (value == rhs) ? 1 : 0;
                break;
            case '!':
                value = (value != rhs) ? 1 : 0;
                break;
            case '<':
                value = (value < rhs) ? 1 : 0;
                break;
            case '>':
                value = (value > rhs) ? 1 : 0;
                break;
            case 'l':
                value = static_cast<int32_t>(static_cast<uint32_t>(value) << (rhs & 31));
                break;
            case 'r':
                value = static_cast<int32_t>(static_cast<uint32_t>(value) >> (rhs & 31));
                break;
            case '+':
                value = static_cast<int32_t>(static_cast<uint32_t>(value) + static_cast<uint32_t>(rhs));
                break;
            case '-':
                value = static_cast<int32_t>(static_cast<uint32_t>(value) - static_cast<uint32_t>(rhs));
                break;
            case '*':
                value = static_cast<int32_t>(static_cast<uint32_t>(value) * static_cast<uint32_t>(rhs));
                break;
            case '/':
                if (rhs == 0) return fail("division by zero");
                // INT32_MIN / -1 would overflow: wrap to INT32_MIN (defined), the
                // boundary check turns it into a line-numbered range error.
                if (value == INT32_MIN && rhs == -1)
                    value = INT32_MIN;
                else
                    value /= rhs;
                break;
            case '%':
                if (rhs == 0) return fail("modulo by zero");
                if (value == INT32_MIN && rhs == -1)
                    value = 0;
                else
                    value %= rhs;
                break;
            default:
                return fail("bad operator");
            }
        }
    }

    bool unary(int32_t &value) {
        skip();
        if (pos_ >= s_.size()) return fail("unexpected end of expression");
        const char c = s_[pos_];
        // Anonymous label reference: the entire remaining expression is one or more
        // identical '+'/'-' characters ("-", "--", "+", "++"). Checked here because
        // these would otherwise be consumed as unary operators.
        if ((c == '-' || c == '+') && AsmState::anon.enabled) {
            std::size_t j = pos_;
            while (j < s_.size() && s_[j] == c) ++j;
            std::size_t k = j;
            while (k < s_.size() && (s_[k] == ' ' || s_[k] == '\t')) ++k;
            if (j > pos_ && k == s_.size()) {
                const int count = static_cast<int>(j - pos_);
                pos_ = j;
                int32_t v = 0;
                if (!resolveAnon(c, count, v)) return false;
                value = v;
                return true;
            }
        }
        switch (c) {
        case '-':
            ++pos_;
            {
                int32_t v;
                if (!unary(v)) return false;
                value = static_cast<int32_t>(0u - static_cast<uint32_t>(v)); // wraps, no INT32_MIN UB
                return true;
            }
        case '+':
            ++pos_;
            return unary(value);
        case '~':
            ++pos_;
            {
                int32_t v;
                if (!unary(v)) return false;
                value = ~v;
                return true;
            }
        case '!':
            ++pos_;
            {
                int32_t v;
                if (!unary(v)) return false;
                value = v ? 0 : 1;
                return true;
            }
        case '<':
            ++pos_;
            {
                int32_t v;
                if (!unary(v)) return false;
                value = v & 0xFF;
                return true;
            }
        case '>':
            ++pos_;
            {
                int32_t v;
                if (!unary(v)) return false;
                value = static_cast<int32_t>((static_cast<uint32_t>(v) >> 8) & 0xFFu);
                return true;
            }
        default:
            break;
        }
        return primary(value);
    }

    // Anonymous labels: "-" resolves to the nearest previous "-"-type definition,
    // counting multiplicity ("--" skips two); "+" to the nearest next "+"-type one.
    bool resolveAnon(char sign, int count, int32_t &value) {
        const AsmState::AnonState &st = AsmState::anon;
        const std::vector<uint16_t> *list = (sign == '-') ? st.minus : st.plus;
        const std::string tok(static_cast<std::size_t>(count), sign);
        if (!list) return fail("anonymous label '" + tok + "' not available here");
        std::vector<uint16_t> candidates;
        for (uint16_t a : *list) {
            if (sign == '-') {
                if (a < st.pc) candidates.push_back(a);
            } else {
                if (a > st.pc) candidates.push_back(a);
            }
        }
        if (sign == '-') {
            if (static_cast<int>(candidates.size()) < count)
                return fail("anonymous label '" + tok + "': not enough previous definitions");
            value = candidates[candidates.size() - static_cast<std::size_t>(count)];
        } else {
            if (static_cast<int>(candidates.size()) < count)
                return fail("anonymous label '" + tok + "': not enough forward definitions");
            value = candidates[static_cast<std::size_t>(count) - 1];
        }
        return true;
    }

    bool primary(int32_t &value) {
        skip();
        if (pos_ >= s_.size()) return fail("unexpected end of expression");
        const char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            if (!expr(0, value)) return false;
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ')') return fail("missing ')'");
            ++pos_;
            return true;
        }
        if (c == '$') {
            ++pos_;
            uint32_t v = 0;
            int digits = 0;
            while (pos_ < s_.size()) {
                const int d = digitValue(s_[pos_]);
                if (d < 0 || d > 15) break;
                v = v * 16u + static_cast<uint32_t>(d);
                ++digits;
                ++pos_;
                if (digits > 8) return fail("hex constant out of range");
            }
            if (digits == 0) return fail("invalid hex constant");
            // Values above INT32_MAX would flip sign on the int32 entry cast.
            if (v > 0x7FFFFFFFu) return fail("hex constant out of range");
            value = static_cast<int32_t>(v);
            return true;
        }
        if (c == '%') {
            ++pos_;
            uint32_t v = 0;
            int digits = 0;
            while (pos_ < s_.size() && (s_[pos_] == '0' || s_[pos_] == '1')) {
                v = (v << 1) | static_cast<uint32_t>(s_[pos_] - '0');
                ++digits;
                ++pos_;
                if (digits > 32) return fail("binary constant out of range");
            }
            if (digits == 0) return fail("invalid binary constant");
            if (v > 0x7FFFFFFFu) return fail("binary constant out of range");
            value = static_cast<int32_t>(v);
            return true;
        }
        if (c == '\'') return charLiteral(value);
        if (std::isdigit(static_cast<unsigned char>(c))) return number(value);
        if (c == '.') {
            if (builtin(value)) return true;
            if (!error_.empty()) return false;
            return symbol(value);
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@') return symbol(value);
        if (c == '*') {
            ++pos_;
            if (!pc_) return fail("current-address symbol '*' not available here");
            value = *pc_;
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    // Fixed-point math/string function-call atoms usable anywhere in an expression:
    //   .sin(deg)  sin of deg degrees scaled by 65536 (Q16 fixed point)
    //   .cos(deg)  cosine likewise (.cos(0) == 65536)
    //   .sqrt(n)   floor(sqrt(n) * 65536)
    //   .strlen("text")  byte length with escapes resolved like .ascii
    // Results are computed in double, rounded to int64 and clamped to int32 before
    // entering the evaluator. Negative results wrap mod 65536 exactly like other
    // negative intermediates produced by unary minus (e.g. .sin(270) == -65536 which
    // becomes $0000 when masked into a 16-bit operand field).
    bool builtin(int32_t &value) {
        const std::size_t save = pos_;
        ++pos_; // '.'
        std::string name;
        while (pos_ < s_.size() &&
               (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || s_[pos_] == '.'))
            name += s_[pos_++];
        const std::string fn = "." + upper(name);
        const bool known = (fn == ".SIN" || fn == ".COS" || fn == ".SQRT" || fn == ".STRLEN");
        skip();
        if (!known || pos_ >= s_.size() || s_[pos_] != '(') {
            pos_ = save;
            return false;
        }
        ++pos_; // '('
        if (fn == ".STRLEN") {
            skip();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail(".strlen expects a string literal");
            std::string lit;
            if (!scanQuoted(lit)) return false;
            std::vector<uint8_t> bytes;
            std::string perr;
            if (!Assembler::parseStringLiteral(lit, bytes, perr)) return fail(".strlen: " + perr);
            value = static_cast<int32_t>(bytes.size());
        } else {
            int32_t arg = 0;
            if (!expr(0, arg)) return false;
            double r = 0.0;
            if (fn == ".SIN") {
                r = std::sin(static_cast<double>(arg) * (kPi / 180.0)) * 65536.0;
            } else if (fn == ".COS") {
                r = std::cos(static_cast<double>(arg) * (kPi / 180.0)) * 65536.0;
            } else {
                if (arg < 0) return fail(".sqrt of negative value");
                r = std::floor(std::sqrt(static_cast<double>(arg)) * 65536.0);
            }
            long long q = std::llround(r);
            if (q > 2147483647LL) q = 2147483647LL;
            if (q < -2147483648LL) q = -2147483648LL;
            value = static_cast<int32_t>(q);
        }
        skip();
        if (pos_ >= s_.size() || s_[pos_] != ')') return fail("missing ')' after " + fn);
        ++pos_;
        return true;
    }

    // Scans a double-quoted string literal starting at pos_, honoring backslash
    // escapes, into lit (including the quotes).
    bool scanQuoted(std::string &lit) {
        const std::size_t begin = pos_;
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                pos_ += 2;
                continue;
            }
            if (s_[pos_] == '"') {
                ++pos_;
                lit = s_.substr(begin, pos_ - begin);
                return true;
            }
            ++pos_;
        }
        return fail("unterminated string literal");
    }

    bool charLiteral(int32_t &value) {
        ++pos_;
        if (pos_ >= s_.size()) return fail("unterminated character literal");
        if (s_[pos_] == '\\') {
            ++pos_;
            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case '0':
                value = 0;
                break;
            case '\\':
                value = '\\';
                break;
            case '"':
                value = '"';
                break;
            case '\'':
                value = '\'';
                break;
            case 'x': {
                int hv = 0, hd = 0;
                while (hd < 2 && pos_ < s_.size()) {
                    const int d = digitValue(s_[pos_]);
                    if (d < 0 || d > 15) break;
                    hv = hv * 16 + d;
                    ++hd;
                    ++pos_;
                }
                if (hd == 0) return fail("invalid \\x escape");
                value = hv;
                break;
            }
            default:
                return fail("unsupported character escape");
            }
        } else {
            value = static_cast<unsigned char>(s_[pos_]);
            ++pos_;
        }
        if (pos_ >= s_.size() || s_[pos_] != '\'') return fail("unterminated character literal");
        ++pos_;
        return true;
    }

    bool number(int32_t &value) {
        if (s_[pos_] == '0' && pos_ + 1 < s_.size() && (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X')) {
            pos_ += 2;
            uint32_t v = 0;
            int digits = 0;
            while (pos_ < s_.size()) {
                const int d = digitValue(s_[pos_]);
                if (d < 0 || d > 15) break;
                v = v * 16u + static_cast<uint32_t>(d);
                ++digits;
                ++pos_;
                if (digits > 8) return fail("hex constant out of range");
            }
            if (digits == 0) return fail("invalid hex constant");
            if (v > 0x7FFFFFFFu) return fail("hex constant out of range");
            value = static_cast<int32_t>(v);
            return true;
        }
        uint64_t v = 0;
        while (pos_ < s_.size()) {
            const char d = s_[pos_];
            if (d == '_') {
                ++pos_;
                continue;
            }
            if (!std::isdigit(static_cast<unsigned char>(d))) break;
            v = v * 10u + static_cast<uint64_t>(d - '0');
            ++pos_;
            // Literals must fit the signed 32-bit expression domain; values that only
            // fit uint32 (e.g. 4294967295) would alias -1 after the int32 cast.
            if (v > 0x7FFFFFFFull) return fail("decimal constant out of range");
        }
        value = static_cast<int32_t>(v);
        return true;
    }

    bool symbol(int32_t &value) {
        const std::size_t begin = pos_;
        while (pos_ < s_.size()) {
            const char d = s_[pos_];
            if (std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '.' || d == '@')
                ++pos_;
            else
                break;
        }
        std::string name = s_.substr(begin, pos_ - begin);
        if (!name.empty() && name[0] == '@') name = scope_ + name;
        const std::string key = Assembler::normalizeSymbol(name);
        const auto it = symbols_.find(key);
        if (it == symbols_.end()) return fail("unknown symbol: " + name);
        value = it->second;
        // Record cross-reference (operand/expression usage) for the xref listing.
        if (AsmState::refs.sink) {
            const int line = AsmState::refs.line;
            bool isDefLine = false;
            if (AsmState::refs.defs) {
                const auto dit = AsmState::refs.defs->find(key);
                if (dit != AsmState::refs.defs->end() && dit->second == line) isDefLine = true;
            }
            if (!isDefLine) (*AsmState::refs.sink)[key].insert(line);
        }
        return true;
    }

    std::string s_;
    std::size_t pos_;
    const std::map<std::string, uint16_t> &symbols_;
    const uint16_t *pc_;
    std::string scope_;
    std::string &error_;
};

bool evalExprInt(const std::string &text, int32_t &value, const std::map<std::string, uint16_t> &symbols,
                 const uint16_t *pc, const std::string &scopePrefix, std::string &error) {
    if (trim(text).empty()) {
        error = "empty expression";
        return false;
    }
    Parser p(text, symbols, pc, scopePrefix, error);
    return p.run(value);
}

} // namespace AsmEvalImpl

namespace {

thread_local std::string t_scopePrefix;

bool evalToU16(const std::string &text, uint16_t &out, const std::map<std::string, uint16_t> &symbols,
               const uint16_t *pc, const std::string &scopePrefix, std::string &error) {
    int32_t v = 0;
    if (!AsmEvalImpl::evalExprInt(text, v, symbols, pc, scopePrefix, error)) return false;
    if (v < -32768 || v > 65535) {
        error = "value out of 16-bit range";
        return false;
    }
    out = static_cast<uint16_t>(v & 0xFFFFu);
    return true;
}

bool evalInScope(const std::string &text, uint16_t &out, const std::map<std::string, uint16_t> &symbols,
                 const uint16_t *pc, std::string &error) {
    return evalToU16(text, out, symbols, pc, t_scopePrefix, error);
}

// Conservative numeric-literal test ($hex / %bin / 0xhex / decimal) used for the
// page-cross warning: only pure literals are considered, never general expressions.
bool isNumericLiteral(const std::string &t) {
    const std::string s = trim(t);
    std::size_t i = 0;
    int base = 10;
    if (!s.empty() && s[0] == '$') {
        base = 16;
        i = 1;
    } else if (!s.empty() && s[0] == '%') {
        base = 2;
        i = 1;
    } else if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '_') continue;
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16)
            d = digitValue(c);
        if (d < 0 || d >= base) return false;
    }
    return true;
}

// Base address part of an indexed operand ("$10FF,Y" -> "$10FF", "($20),Y" -> "$20").
std::string extractIndexedBase(const std::string &op, Mode mode) {
    std::string s = trim(op);
    if (mode == Mode::IndirectIndexed) {
        if (!s.empty() && s.front() == '(') {
            const auto close = s.find(')');
            if (close != std::string::npos) s = trim(s.substr(1, close - 1));
        }
        return s;
    }
    const auto comma = s.rfind(',');
    if (comma != std::string::npos) s = trim(s.substr(0, comma));
    return s;
}

const std::unordered_set<std::string> &illegalMnemonicSet() {
    static const std::unordered_set<std::string> set = {"SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP",
                                                        "ISC", "ANC", "ALR", "ARR", "SBX", "XAA", "SHY",
                                                        "SHX", "SHA", "TAS", "LAS", "JAM"};
    return set;
}

// Detects ".substr(" string-context calls and evaluates them into concrete bytes.
// Syntax: .substr("text", startExpr, lenExpr)
bool resolveStringOperand(const std::string &token,
                          const std::function<bool(const std::string &, uint16_t &, std::string &)> &evalU16,
                          std::vector<uint8_t> &out, std::string &error) {
    const std::string s = trim(token);
    if (s.size() < 9 || _strnicmp(s.c_str(), ".substr(", 8) != 0) return false;
    if (s.back() != ')') {
        error = ".substr expects ')'";
        return true;
    }
    const auto parts = Assembler::splitOperands(s.substr(8, s.size() - 9));
    if (parts.size() != 3) {
        error = ".substr expects (\"text\", start, length)";
        return true;
    }
    std::vector<uint8_t> str;
    if (!Assembler::parseStringLiteral(parts[0], str, error)) {
        error = ".substr: " + error;
        return true;
    }
    uint16_t start = 0, len = 0;
    if (!evalU16(parts[1], start, error)) {
        error = ".substr: " + error;
        return true;
    }
    if (!evalU16(parts[2], len, error)) {
        error = ".substr: " + error;
        return true;
    }
    if (start > str.size() || static_cast<std::size_t>(start) + len > str.size()) {
        error = ".substr: range out of bounds";
        return true;
    }
    out.assign(str.begin() + start, str.begin() + start + len);
    return true;
}

} // namespace

std::vector<uint8_t> AssembledProgram::flatImage() const {
    if (byteCount == 0 || lowestAddress == 0xFFFF) return {};
    return std::vector<uint8_t>(memory.begin() + lowestAddress, memory.begin() + highestAddress + 1);
}

std::string Assembler::normalizeSymbol(const std::string &name) {
    return upper(trim(name));
}

bool Assembler::parseValue(const std::string &text, uint16_t &value,
                           const std::map<std::string, uint16_t> &symbols) {
    std::string error;
    return evalToU16(text, value, symbols, nullptr, "", error);
}

const Assembler::OpcodeInfo *Assembler::opcodeTable() {
    return OPCODES;
}
std::size_t Assembler::opcodeTableSize() {
    return sizeof(OPCODES) / sizeof(OPCODES[0]);
}

const Assembler::OpcodeInfo *Assembler::findOpcodeByByte(uint8_t opcode) {
    for (std::size_t i = 0; i < opcodeTableSize(); ++i)
        if (OPCODES[i].opcode == opcode) return &OPCODES[i];
    return nullptr;
}

const Assembler::OpcodeInfo *Assembler::findOpcode(const std::string &mnemonic, AddrMode mode) {
    const std::string m = upper(mnemonic);
    for (std::size_t i = 0; i < opcodeTableSize(); ++i)
        if (m == OPCODES[i].name && mode == OPCODES[i].mode) return &OPCODES[i];
    return nullptr;
}

bool Assembler::mnemonicExists(const std::string &mnemonic) {
    const std::string m = upper(mnemonic);
    for (const auto &op : OPCODES)
        if (m == op.name) return true;
    return false;
}

Assembler::ParsedLine Assembler::parseLine(const std::string &raw) {
    ParsedLine line;
    std::string s = stripComment(raw);
    s = trim(s);
    if (s.empty()) return line;
    // Find the first ':' that is outside quotes so char literals (LDA #':')
    // are never mistaken for a label separator.
    std::size_t colon = std::string::npos;
    {
        bool inQuote = false;
        char q = 0;
        for (std::size_t ci = 0; ci < s.size(); ++ci) {
            const char c = s[ci];
            if (inQuote && c == '\\' && ci + 1 < s.size()) {
                ++ci; // escaped char stays inside the quote (never a label separator)
                continue;
            }
            if ((c == '"' || c == '\'') && (!inQuote || c == q)) {
                inQuote = !inQuote;
                q = inQuote ? c : 0;
                continue;
            }
            if (!inQuote && c == ':') {
                colon = ci;
                break;
            }
        }
    }
    if (colon != std::string::npos) {
        line.label = trim(s.substr(0, colon));
        line.hasLabel = true;
        s = trim(s.substr(colon + 1));
    } else if (!raw.empty() && !std::isspace(static_cast<unsigned char>(raw[0]))) {
        std::istringstream iss(s);
        std::string first, second;
        iss >> first >> second;
        const bool anonTok = isAnonToken(first);
        if (!first.empty() &&
            (anonTok || (!second.empty() && !Assembler::mnemonicExists(first) && first[0] != '.'))) {
            line.label = first;
            line.hasLabel = true;
            s = trim(s.substr(first.size()));
        }
    }
    if (!line.label.empty()) line.label = normalizeSymbol(line.label);
    if (s.empty()) return line;
    std::istringstream iss(s);
    iss >> line.mnemonic;
    const std::size_t pos = s.find_first_of(" \t");
    if (pos != std::string::npos) line.operands = trim(s.substr(pos));
    line.mnemonic = upper(line.mnemonic);
    return line;
}

std::vector<std::string> Assembler::splitOperands(const std::string &text) {
    std::vector<std::string> result;
    std::string current;
    int paren = 0;
    bool quote = false;
    char q = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote && c == '\\' && i + 1 < text.size()) {
            // Keep the escape pair verbatim: '"' inside a string is data, not a close quote.
            current.push_back(c);
            current.push_back(text[++i]);
            continue;
        }
        if ((c == '"' || c == '\'') && (!quote || c == q)) {
            quote = !quote;
            q = quote ? c : 0;
        }
        if (!quote && c == '(') ++paren;
        if (!quote && c == ')') --paren;
        if (!quote && paren == 0 && c == ',') {
            result.push_back(trim(current));
            current.clear();
        } else
            current.push_back(c);
    }
    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

bool Assembler::evalAtom(const std::string &raw, uint16_t &value,
                         const std::map<std::string, uint16_t> &symbols, std::string &error) {
    return evalToU16(raw, value, symbols, nullptr, "", error);
}

bool Assembler::evalExpression(const std::string &raw, uint16_t &value,
                               const std::map<std::string, uint16_t> &symbols, std::string &error) {
    return evalToU16(raw, value, symbols, nullptr, "", error);
}

Assembler::AddrMode Assembler::detectMode(const ParsedLine &line,
                                          const std::map<std::string, uint16_t> &symbols,
                                          std::string &error) {
    const std::string m = line.mnemonic;
    const std::string op = trim(line.operands);
    if (op.empty()) return Assembler::AddrMode::Implied;
    if (op == "A" || op == "a") return Assembler::AddrMode::Accumulator;
    if (op.front() == '#') return Assembler::AddrMode::Immediate;
    if (!m.empty() && m[0] == 'B' && m != "BIT") return Assembler::AddrMode::Relative;
    if (op.front() == '(') {
        const auto close = op.rfind(')');
        if (close == std::string::npos) {
            error = "missing ')' in operand";
            return Assembler::AddrMode::Invalid;
        }
        const std::string suffix = trim(op.substr(close + 1));
        if (suffix == ",Y" || suffix == ",y") return Assembler::AddrMode::IndirectIndexed;
        const std::string inside = trim(op.substr(1, close - 1));
        const bool endsWithX =
            inside.size() >= 2 && _strnicmp(inside.c_str() + inside.size() - 2, ",X", 2) == 0;
        if (endsWithX) {
            // JMP "($1234,X)" is the 65C02 absolute-indexed indirect jump.
            if (m == "JMP") return Assembler::AddrMode::IndirectAbsoluteX;
            return Assembler::AddrMode::IndexedIndirect;
        }
        return Assembler::AddrMode::Indirect;
    }
    std::string core = op;
    char index = 0;
    const auto comma = op.rfind(',');
    if (comma != std::string::npos) {
        const std::string idx = upper(trim(op.substr(comma + 1)));
        if (idx == "X" || idx == "Y") {
            index = idx[0];
            core = trim(op.substr(0, comma));
        }
    }
    uint16_t value = 0;
    std::string evalErr;
    const bool known = evalInScope(core, value, symbols, nullptr, evalErr);
    Assembler::AddrMode mode;
    if (index == 'X')
        mode = (known && value <= 0xFF) ? Assembler::AddrMode::ZeroPageX : Assembler::AddrMode::AbsoluteX;
    else if (index == 'Y')
        mode = (known && value <= 0xFF) ? Assembler::AddrMode::ZeroPageY : Assembler::AddrMode::AbsoluteY;
    else
        mode = (known && value <= 0xFF) ? Assembler::AddrMode::ZeroPage : Assembler::AddrMode::Absolute;
    // Unresolved (forward) symbol: pass 1 must size the instruction before the
    // value is known. Prefer the absolute form, but keep the only variant that
    // exists for mnemonics such as STX zp,Y / STY zp,X so both passes agree.
    if (!known && !findOpcode(m, mode)) {
        const Assembler::AddrMode alt = (index == 'X') ? Assembler::AddrMode::ZeroPageX
                                        : (index == 'Y') ? Assembler::AddrMode::ZeroPageY
                                                         : Assembler::AddrMode::ZeroPage;
        if (findOpcode(m, alt)) mode = alt;
    }
    return mode;
}

int Assembler::modeSize(AddrMode mode) {
    switch (mode) {
    case Assembler::AddrMode::Implied:
    case Assembler::AddrMode::Accumulator:
        return 1;
    case Assembler::AddrMode::Immediate:
    case Assembler::AddrMode::ZeroPage:
    case Assembler::AddrMode::ZeroPageX:
    case Assembler::AddrMode::ZeroPageY:
    case Assembler::AddrMode::IndexedIndirect:
    case Assembler::AddrMode::IndirectIndexed:
    case Assembler::AddrMode::Relative:
        return 2;
    case Assembler::AddrMode::Absolute:
    case Assembler::AddrMode::AbsoluteX:
    case Assembler::AddrMode::AbsoluteY:
    case Assembler::AddrMode::Indirect:
    case Assembler::AddrMode::IndirectAbsoluteX:
        return 3;
    default:
        return 0;
    }
}

bool Assembler::parseStringLiteral(const std::string &raw, std::vector<uint8_t> &out, std::string &error) {
    const std::string s = trim(raw);
    if (s.size() < 2 || (s.front() != '"' && s.front() != '\'') || s.back() != s.front()) {
        error = "invalid string literal";
        return false;
    }
    out.clear();
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\') {
            if (++i >= s.size() - 1) {
                error = "unterminated escape";
                return false;
            }
            switch (s[i]) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '0':
                out.push_back(0);
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            case '\'':
                out.push_back('\'');
                break;
            case 'x': {
                if (i + 2 >= s.size() - 1) {
                    error = "invalid \\x escape";
                    return false;
                }
                const int hi = digitValue(s[i + 1]);
                const int lo = digitValue(s[i + 2]);
                if (hi < 0 || lo < 0) {
                    error = "invalid \\x escape";
                    return false;
                }
                out.push_back(static_cast<uint8_t>((hi << 4) | lo));
                i += 2;
                break;
            }
            default:
                error = "unsupported string escape";
                return false;
            }
        } else {
            out.push_back(static_cast<uint8_t>(static_cast<unsigned char>(s[i])));
        }
    }
    return true;
}

bool Assembler::encodeInstruction(const ParsedLine &line, AddrMode mode, uint16_t address,
                                  const std::map<std::string, uint16_t> &symbols, std::vector<uint8_t> &bytes,
                                  std::string &error) {
    const Op *info = Assembler::findOpcode(line.mnemonic, mode);
    if (!info) {
        error = "unsupported addressing mode for " + line.mnemonic;
        return false;
    }
    bytes.push_back(info->opcode);
    if (mode == Assembler::AddrMode::Implied || mode == Assembler::AddrMode::Accumulator) return true;
    std::string op = trim(line.operands);
    if (mode == Assembler::AddrMode::Immediate) op = trim(op.substr(1));
    if (mode == Assembler::AddrMode::IndirectAbsoluteX) {
        // "($1234,X)": strip parentheses, then the ",X" index marker.
        if (!op.empty() && op.front() == '(' && op.back() == ')') op = trim(op.substr(1, op.size() - 2));
        const auto cx = op.rfind(',');
        if (cx != std::string::npos) op = trim(op.substr(0, cx));
    } else if (mode == Assembler::AddrMode::IndexedIndirect) {
        // "($42,X)": drop the '(' first, otherwise cutting at the comma leaves "($42".
        if (!op.empty() && op.front() == '(') op = trim(op.substr(1));
        const auto cx = op.rfind(',');
        if (cx != std::string::npos) op = trim(op.substr(0, cx));
    } else if (mode == Assembler::AddrMode::IndirectIndexed || mode == Assembler::AddrMode::AbsoluteX ||
               mode == Assembler::AddrMode::AbsoluteY || mode == Assembler::AddrMode::ZeroPageX ||
               mode == Assembler::AddrMode::ZeroPageY) {
        const auto comma = op.rfind(',');
        if (comma != std::string::npos) op = trim(op.substr(0, comma));
    }
    if (mode == Assembler::AddrMode::Indirect) op = trim(op.substr(1, op.size() - 2));
    uint16_t value = 0;
    if (!evalInScope(op, value, symbols, &address, error)) return false;
    if (mode == Assembler::AddrMode::Relative) {
        const int32_t offset = static_cast<int32_t>(value) - static_cast<int32_t>(address + 2);
        if (offset < -128 || offset > 127) {
            error = "branch target out of range";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(offset & 0xFF));
        return true;
    }
    const int width =
        (mode == Assembler::AddrMode::Immediate || mode == Assembler::AddrMode::ZeroPage ||
         mode == Assembler::AddrMode::ZeroPageX || mode == Assembler::AddrMode::ZeroPageY ||
         mode == Assembler::AddrMode::IndexedIndirect || mode == Assembler::AddrMode::IndirectIndexed)
            ? 1
            : 2;
    if (width == 1) {
        // No general $FF00+ carve-out: a zero-page mode with a >=$FF00 value
        // means a forward label resolved outside page zero - error instead of
        // truncating. Immediates keep the allowance so `LDA #-1` ($FFFF) -> $FF.
        if (value > 0xFF && !(mode == Assembler::AddrMode::Immediate && value >= 0xFF00)) {
            error = "8-bit operand out of range";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    } else {
        bytes.push_back(static_cast<uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    }
    return true;
}

bool Assembler::exportSymbolsJson(const AssembledProgram &program, const std::string &path,
                                  std::string &error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create symbol file: " + path;
        return false;
    }
    out << "{\n";
    bool first = true;
    for (const auto &sym : program.symbols) {
        if (!first) out << ",\n";
        first = false;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\"0x%04X\"", sym.second);
        out << "  \"" << sym.first << "\": " << buf;
    }
    out << "\n}\n";
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "exported " + std::to_string(program.symbols.size()) + " symbols";
    return true;
}

bool Assembler::writeListingFile(const AssembledProgram &program, const std::string &path,
                                 std::string &error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create listing file: " + path;
        return false;
    }
    char addr[8];
    for (const auto &entry : program.listing) {
        std::snprintf(addr, sizeof(addr), "%04X", entry.address);
        out << addr << "  ";
        char byte[4];
        std::size_t shown = 0;
        for (uint8_t b : entry.bytes) {
            if (shown++ == 4) {
                out << "  ";
                shown = 1;
            }
            std::snprintf(byte, sizeof(byte), "%02X ", b);
            out << byte;
        }
        for (std::size_t pad = shown; pad <= 4; ++pad) out << "   ";
        out << "  ";
        if (entry.sourceLine > 0) out << entry.sourceLine << ": ";
        out << entry.source << "\n";
    }
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "listing written";
    return true;
}

bool Assembler::writeMapFile(const AssembledProgram &program, const std::string &path, std::string &error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create map file: " + path;
        return false;
    }
    out << "; map file v1\n";
    std::vector<AssembledProgram::Segment> segs = program.segments;
    std::sort(segs.begin(), segs.end(),
              [](const AssembledProgram::Segment &a, const AssembledProgram::Segment &b) {
                  return a.start < b.start;
              });
    for (const auto &seg : segs) {
        char buf[96];
        const unsigned bytes =
            static_cast<unsigned>(static_cast<uint32_t>(seg.end) - static_cast<uint32_t>(seg.start) + 1u);
        std::snprintf(buf, sizeof(buf), "segment $%04X-$%04X  (%u bytes)", static_cast<unsigned>(seg.start),
                      static_cast<unsigned>(seg.end), bytes);
        out << buf << "\n";
    }
    std::vector<std::pair<uint16_t, std::string>> syms;
    syms.reserve(program.symbols.size());
    for (const auto &kv : program.symbols) syms.emplace_back(kv.second, kv.first);
    std::sort(syms.begin(), syms.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });
    for (const auto &sym : syms) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "symbol %-16s = $%04X", sym.second.c_str(),
                      static_cast<unsigned>(sym.first));
        out << buf << "\n";
    }
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "map file written";
    return true;
}

bool Assembler::writeXrefFile(const AssembledProgram &program, const std::string &path, std::string &error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create xref file: " + path;
        return false;
    }
    out << "; cross-reference\n";
    for (const auto &entry : program.symbolReferences) { // std::map iterates sorted by name
        int defLine = 0;
        const auto dit = program.symbolDefinitionLines.find(entry.first);
        if (dit != program.symbolDefinitionLines.end()) defLine = dit->second;
        out << entry.first << "  def line " << defLine << "  refs:";
        if (entry.second.empty()) {
            out << " -";
        } else {
            for (int ln : entry.second) out << " " << ln;
        }
        out << "\n";
    }
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "cross-reference written";
    return true;
}

bool Assembler::exportPrg(const AssembledProgram &program, const std::string &path, std::string &error) {
    const std::vector<uint8_t> image = program.flatImage();
    if (image.empty()) {
        error = "no assembled bytes to export";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create prg file: " + path;
        return false;
    }
    out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "prg written";
    return true;
}

bool Assembler::exportNes(const AssembledProgram &program, const std::string &path, std::string &error,
                          int prgBanks16) {
    if (prgBanks16 <= 0 || prgBanks16 > 64) {
        error = "invalid PRG bank count";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create nes file: " + path;
        return false;
    }
    uint8_t header[16] = {'N', 'E', 'S', 0x1A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    header[4] = static_cast<uint8_t>(prgBanks16);
    header[5] = 0; // CHR RAM/none: zero CHR banks, mapper 0, all flags zero
    header[6] = 0;
    header[7] = 0;
    out.write(reinterpret_cast<const char *>(header), sizeof(header));

    std::vector<uint8_t> prg(static_cast<std::size_t>(prgBanks16) * 16384u, 0xFF);
    const std::size_t prgSize = prg.size();
    if (program.byteCount != 0 && program.lowestAddress != 0xFFFF) {
        const uint16_t top =
            static_cast<uint16_t>(program.highestAddress < 0xFFFF ? program.highestAddress : 0xFFFF);
        // Copy the assembled image starting at $8000; addresses beyond the ROM's
        // capacity wrap (mirrored), matching iNES banking behavior.
        for (uint32_t a = 0x8000u; a <= top; ++a) {
            if (program.written[a]) prg[(a - 0x8000u) % prgSize] = program.memory[a];
        }
    }
    out.write(reinterpret_cast<const char *>(prg.data()), static_cast<std::streamsize>(prg.size()));

    const std::vector<uint8_t> chr(8192, 0x00);
    out.write(reinterpret_cast<const char *>(chr.data()), static_cast<std::streamsize>(chr.size()));
    if (!out) {
        error = "write failed: " + path;
        return false;
    }
    error = "nes rom written";
    return true;
}

std::map<std::string, int> Assembler::splitDefines(const std::string &csv) {
    std::map<std::string, int> out;
    std::istringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        const auto eq = item.find('=');
        const std::string name = trim(eq == std::string::npos ? item : item.substr(0, eq));
        const std::string val = eq == std::string::npos ? std::string() : trim(item.substr(eq + 1));
        if (name.empty()) continue;
        int v = 1;
        if (!val.empty()) {
            char *end = nullptr;
            const long parsed = std::strtol(val.c_str(), &end, 0);
            if (end && *end == '\0') v = static_cast<int>(parsed);
        }
        out[normalizeSymbol(name)] = v;
    }
    return out;
}

namespace {

struct Line {
    Assembler::ParsedLine parsed;
    std::string source; // listing source text ("file:line: text" prefix in project mode)
    std::string file;
    int number = 0;      // sequential id
    int displayLine = 0; // user-facing source line
    int column = 1;
    int labelColumn = 1;
    bool active = true;
    // Addressing mode chosen in pass 1; pass 2 must reuse it so both passes
    // agree on instruction size even with forward-referenced symbols.
    Assembler::AddrMode pass1Mode = Assembler::AddrMode::Invalid;
};

// RAII guard so thread-local parser state can never leak across assemble() calls.
struct ThreadStateGuard {
    ~ThreadStateGuard() { AsmState::reset(); }
};

bool assembleCore(const std::vector<SourceLine> &raw, std::vector<SourceLine> &proc,
                  const AssembleOptions &options, AssembledProgram &program,
                  std::vector<AssemblerError> &errors, bool projectMode) {
    ThreadStateGuard guard;
    program = AssembledProgram{};
    errors.clear();

    Preprocessor pre;
    pre.errors = &errors;
    pre.budget = options.maxExpansionLines;
    pre.includePaths = options.includePaths;
    for (const auto &kv : options.predefinedSymbols)
        if (!kv.first.empty())
            pre.constSyms[Assembler::normalizeSymbol(kv.first)] = static_cast<uint16_t>(kv.second);
    if (!pre.run(raw, options.basePath.empty() ? "." : options.basePath, options.maxIncludeDepth, proc))
        return false;
    if (std::getenv("ASM_DEBUG") != nullptr) {
        std::fprintf(stderr, "--- processed %zu lines ---\n", proc.size());
        for (const auto &sl : proc) std::fprintf(stderr, "%4d | %s\n", sl.number, sl.text.c_str());
    }
    std::vector<Line> lines;
    lines.reserve(proc.size());
    for (const SourceLine &sl : proc) {
        Line l;
        l.parsed = Assembler::parseLine(sl.text);
        l.parsed.number = sl.displayNumber > 0 ? sl.displayNumber : sl.number;
        l.file = sl.fileName;
        l.number = sl.number;
        l.displayLine = sl.displayNumber > 0 ? sl.displayNumber : sl.number;
        l.column = operandColumnOf(sl.text);
        l.labelColumn = labelColumnOf(sl.text);
        l.source = (projectMode && !sl.fileName.empty())
                       ? sl.fileName + ":" + std::to_string(l.displayLine) + ": " + sl.text
                       : sl.text;
        lines.push_back(std::move(l));
    }

    std::map<std::string, uint16_t> symbols;
    for (const auto &kv : options.predefinedSymbols) {
        if (kv.first.empty()) continue;
        symbols[Assembler::normalizeSymbol(kv.first)] = static_cast<uint16_t>(kv.second);
    }
    uint16_t entryDirective = 0;
    bool hasEntryDirective = false;
    uint16_t firstOrg = 0x8000;
    bool sawOrg = false;
    std::string scopePrefix;
    t_scopePrefix.clear();

    std::vector<uint16_t> anonMinus;
    std::vector<uint16_t> anonPlus;
    std::map<std::string, std::string> symbolFirstSite;
    std::map<std::string, int> symDefLines;
    std::map<std::string, std::set<int>> refMap;
    AsmState::anon.enabled = true;
    AsmState::anon.minus = &anonMinus;
    AsmState::anon.plus = &anonPlus;
    AsmState::anon.pc = 0x8000;
    AsmState::refs.sink = &refMap;
    AsmState::refs.defs = &symDefLines;
    bool collectAnon = true;

    auto defSiteString = [&](const Line &l) -> std::string {
        if (!l.file.empty()) return l.file + ":" + std::to_string(l.displayLine);
        return "line " + std::to_string(l.displayLine);
    };
    auto recordDef = [&](const std::string &key, const Line &l) {
        if (symbols.count(key)) return;
        symbolFirstSite[key] = defSiteString(l);
        symDefLines[key] = l.displayLine;
    };

    auto defineLabel = [&](Line &l, uint16_t pcVal, bool allowExisting) -> bool {
        if (!l.parsed.hasLabel || l.parsed.label.empty()) return true;
        std::string key = l.parsed.label;
        if (isAnonToken(key)) {
            if (collectAnon) (key[0] == '-' ? anonMinus : anonPlus).push_back(pcVal);
            return true;
        }
        if (!key.empty() && key[0] == '@') {
            if (scopePrefix.empty()) {
                errors.push_back({l.number, "local label '" + key + "' without a preceding global label",
                                  l.labelColumn, l.file});
                return false;
            }
            key = scopePrefix + key;
        } else {
            scopePrefix = key + ".";
        }
        t_scopePrefix = scopePrefix;
        if (symbols.count(key) && !allowExisting) {
            std::string where;
            const auto f = symbolFirstSite.find(key);
            if (f != symbolFirstSite.end()) where = " (defined at " + f->second + ")";
            errors.push_back({l.number, "duplicate label: " + key + where, l.labelColumn, l.file});
            return false;
        }
        recordDef(key, l);
        symbols[key] = pcVal;
        return true;
    };

    auto evalLine = [&](Line &l, const std::string &text, uint16_t pcVal, uint16_t &out,
                        std::string &what) -> bool {
        AsmState::anon.pc = pcVal;
        AsmState::refs.line = l.displayLine;
        return evalToU16(text, out, symbols, &pcVal, scopePrefix, what);
    };

    struct CondFrame {
        bool parentActive;
        bool cond;
        bool inElse;
    };
    std::vector<CondFrame> condStackPass1;
    auto condFrameActive = [](const CondFrame &f) { return f.parentActive && (f.inElse ? !f.cond : f.cond); };
    auto condActivePass1 = [&]() {
        for (const auto &f : condStackPass1)
            if (!condFrameActive(f)) return false;
        return true;
    };
    std::vector<bool> activeMask(lines.size(), true);

    uint16_t pc = 0x8000;

    // ---------------- pass 1: layout ----------------
    for (std::size_t i = 0; i < lines.size(); ++i) {
        Line &l = lines[i];
        Assembler::ParsedLine &p = l.parsed;

        AsmState::anon.pc = pc;
        AsmState::refs.line = l.displayLine;

        const bool isIf = (p.mnemonic == ".IF" || p.mnemonic == ".IFDEF" || p.mnemonic == ".IFNDEF");
        const bool isElseLike = (p.mnemonic == ".ELSE" || p.mnemonic == ".ELIF");
        const bool isEndif = (p.mnemonic == ".ENDIF");

        if (isIf || isElseLike || isEndif) {
            const bool parentActive = condActivePass1();
            if (isIf) {
                bool truth = false;
                std::string what;
                if (p.mnemonic == ".IF") {
                    uint16_t v = 0;
                    truth = evalLine(l, p.operands, pc, v, what) && v != 0;
                    if (!what.empty()) {
                        errors.push_back({l.number, ".if: " + what, l.column, l.file});
                        return false;
                    }
                } else {
                    std::istringstream in(p.operands);
                    std::string name;
                    in >> name;
                    const bool present = symbols.count(Assembler::normalizeSymbol(name)) > 0;
                    truth = (p.mnemonic == ".IFDEF") ? present : !present;
                }
                condStackPass1.push_back({parentActive, truth, false});
            } else if (isElseLike) {
                if (condStackPass1.empty()) {
                    errors.push_back({l.number, p.mnemonic + " without .if", l.column, l.file});
                    return false;
                }
                CondFrame &frame = condStackPass1.back();
                if (frame.inElse) {
                    errors.push_back({l.number, "duplicate .else", l.column, l.file});
                    return false;
                }
                if (p.mnemonic == ".ELSE") {
                    frame.inElse = true;
                } else {
                    uint16_t v = 0;
                    std::string what;
                    const bool truth = evalLine(l, p.operands, pc, v, what) && v != 0;
                    if (!what.empty()) {
                        errors.push_back({l.number, ".elif: " + what, l.column, l.file});
                        return false;
                    }
                    frame.cond = frame.cond ? false : truth;
                    frame.inElse = frame.cond;
                }
            } else {
                if (condStackPass1.empty()) {
                    errors.push_back({l.number, ".endif without .if", l.column, l.file});
                    return false;
                }
                condStackPass1.pop_back();
            }
            continue;
        }

        const bool active = condActivePass1();
        activeMask[i] = active;
        if (!active) continue;
        const bool isEquLike = (p.mnemonic == ".EQU" || p.mnemonic == ".DEFINE");
        if (p.hasLabel && !isEquLike && !defineLabel(l, pc, false)) return false;
        if (p.mnemonic.empty()) continue;

        if (p.mnemonic == ".ORG") {
            uint16_t v = 0;
            std::string what;
            if (!evalLine(l, p.operands, pc, v, what)) {
                errors.push_back({l.number, ".org: " + what, l.column, l.file});
                continue;
            }
            pc = v;
            if (!sawOrg) {
                firstOrg = v;
                sawOrg = true;
            }
            continue;
        }
        if (p.mnemonic == ".EQU" || p.mnemonic == ".DEFINE") {
            std::string nameKey;
            std::string valueExpr = p.operands;
            if (p.hasLabel) {
                nameKey = p.label;
            } else {
                std::istringstream parts(p.operands);
                std::string first;
                parts >> first;
                if (first.empty()) {
                    errors.push_back({l.number, p.mnemonic + " requires NAME and VALUE", l.column, l.file});
                    continue;
                }
                nameKey = Assembler::normalizeSymbol(first);
                std::getline(parts, valueExpr);
                valueExpr = trim(valueExpr);
                if (valueExpr.empty()) {
                    errors.push_back({l.number, p.mnemonic + " requires a VALUE", l.column, l.file});
                    continue;
                }
            }
            uint16_t v = 0;
            std::string what;
            if (!evalLine(l, valueExpr, pc, v, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            if (!nameKey.empty() && nameKey[0] == '@') nameKey = scopePrefix + nameKey;
            recordDef(nameKey, l);
            symbols[nameKey] = v;
            continue;
        }
        if (p.mnemonic == ".ENTRY") {
            uint16_t v = 0;
            std::string what;
            if (!evalLine(l, p.operands, pc, v, what)) {
                errors.push_back({l.number, ".entry: " + what, l.column, l.file});
                continue;
            }
            entryDirective = v;
            hasEntryDirective = true;
            continue;
        }
        if (p.mnemonic == ".ASSERT") {
            const auto parts = Assembler::splitOperands(p.operands);
            if (parts.empty()) {
                errors.push_back({l.number, ".assert requires an expression", l.column, l.file});
                continue;
            }
            uint16_t v = 0;
            std::string what;
            if (!evalLine(l, parts[0], pc, v, what)) {
                errors.push_back({l.number, ".assert: " + what, l.column, l.file});
                continue;
            }
            if (v == 0) {
                std::string msg = "assertion failed";
                if (parts.size() > 1) msg += ": " + parts[1];
                errors.push_back(
                    {l.number, "line " + std::to_string(l.displayLine) + ": " + msg, l.column, l.file});
            }
            continue;
        }
        if (p.mnemonic == ".ERROR") {
            errors.push_back(
                {l.number, "line " + std::to_string(l.displayLine) + ": " + p.operands, l.column, l.file});
            continue;
        }
        if (p.mnemonic == ".WARN" || p.mnemonic == ".SEGMENT" || p.mnemonic == ".HEX") {
            if (p.mnemonic == ".HEX") {
                std::string hexOnly;
                for (char c : p.operands)
                    if (std::isxdigit(static_cast<unsigned char>(c))) hexOnly += c;
                if (hexOnly.size() % 2 != 0) {
                    errors.push_back({l.number, ".hex expects pairs of hex digits", l.column, l.file});
                    continue;
                }
                pc = static_cast<uint16_t>(pc + hexOnly.size() / 2);
            }
            continue;
        }
        if (p.mnemonic == ".ALIGN") {
            const auto parts = Assembler::splitOperands(p.operands);
            if (parts.empty()) {
                errors.push_back({l.number, ".align requires an alignment", l.column, l.file});
                continue;
            }
            uint16_t align = 0;
            std::string what;
            if (!evalLine(l, parts[0], pc, align, what) || align == 0) {
                errors.push_back(
                    {l.number, ".align: " + (what.empty() ? "invalid alignment" : what), l.column, l.file});
                continue;
            }
            const uint16_t rem = static_cast<uint16_t>(pc % align);
            if (rem != 0) pc = static_cast<uint16_t>(pc + (align - rem));
            continue;
        }
        if (p.mnemonic == ".BYTE" || p.mnemonic == ".DB") {
            for (const auto &token : Assembler::splitOperands(p.operands)) {
                if (!token.empty() && (token.front() == '"' || token.front() == '\'')) {
                    std::vector<uint8_t> str;
                    std::string what;
                    if (!Assembler::parseStringLiteral(token, str, what)) {
                        errors.push_back({l.number, ".byte: " + what, l.column, l.file});
                        continue;
                    }
                    pc = static_cast<uint16_t>(pc + str.size());
                } else {
                    uint16_t v = 0;
                    std::string what;
                    if (!evalLine(l, token, pc, v, what))
                        errors.push_back({l.number, ".byte: " + what, l.column, l.file});
                    else
                        pc = static_cast<uint16_t>(pc + 1);
                }
            }
            continue;
        }
        if (p.mnemonic == ".WORD" || p.mnemonic == ".DW") {
            for (const auto &token : Assembler::splitOperands(p.operands)) {
                uint16_t v = 0;
                std::string what;
                if (!evalLine(l, token, pc, v, what))
                    errors.push_back({l.number, ".word: " + what, l.column, l.file});
                else
                    pc = static_cast<uint16_t>(pc + 2);
            }
            continue;
        }
        if (p.mnemonic == ".ASCII" || p.mnemonic == ".ASCIZ") {
            std::vector<uint8_t> str;
            std::string what;
            auto evalFn = [&](const std::string &txt, uint16_t &o, std::string &e) {
                return evalLine(l, txt, pc, o, e);
            };
            if (resolveStringOperand(p.operands, evalFn, str, what)) {
                if (!what.empty()) {
                    errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                    continue;
                }
            } else if (!Assembler::parseStringLiteral(p.operands, str, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            pc = static_cast<uint16_t>(pc + str.size() + (p.mnemonic == ".ASCIZ" ? 1 : 0));
            continue;
        }
        if (p.mnemonic == ".RES" || p.mnemonic == ".FILL") {
            const auto ops = Assembler::splitOperands(p.operands);
            if (ops.empty()) {
                errors.push_back({l.number, p.mnemonic + " requires a size", l.column, l.file});
                continue;
            }
            uint16_t n = 0;
            std::string what;
            if (!evalLine(l, ops[0], pc, n, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            pc = static_cast<uint16_t>(pc + n);
            continue;
        }
        if (p.mnemonic == ".END") break;
        if (!p.mnemonic.empty() && p.mnemonic.front() == '.') {
            errors.push_back({l.number, "unknown directive: " + p.mnemonic, l.column, l.file});
            continue;
        }
        if (!Assembler::mnemonicExists(p.mnemonic)) {
            errors.push_back({l.number, "unknown mnemonic: " + p.mnemonic, l.column, l.file});
            continue;
        }
        std::string what;
        const Assembler::AddrMode mode = Assembler::detectMode(p, symbols, what);
        if (mode == Assembler::AddrMode::Invalid) {
            errors.push_back({l.number, what, l.column, l.file});
            continue;
        }
        if (!Assembler::findOpcode(p.mnemonic, mode)) {
            errors.push_back({l.number, "unsupported addressing mode for " + p.mnemonic, l.column, l.file});
            continue;
        }
        l.pass1Mode = mode;
        pc = static_cast<uint16_t>(pc + Assembler::modeSize(mode));
    }

    if (std::getenv("ASM_DEBUG") != nullptr) {
        std::fprintf(stderr, "--- pass1 symbols ---\n");
        for (const auto &kv : symbols) std::fprintf(stderr, "%s = %04X\n", kv.first.c_str(), kv.second);
    }

    uint16_t entry = sawOrg ? firstOrg : 0x8000;
    const auto resetSym = symbols.find("RESET");
    if (resetSym != symbols.end()) entry = resetSym->second;
    // Precedence (unchanged, see defect register item 5): .entry > START > RESET >
    // first .org. START intentionally wins over RESET: every example defines only
    // "start:" and writes the reset vector itself (".org $FFFC" / ".word start"),
    // so nothing in-tree indicates RESET must take priority. Flipping this would
    // change --run entry for user sources that define both labels.
    const auto startSym = symbols.find("START");
    if (startSym != symbols.end()) entry = startSym->second;
    if (hasEntryDirective) entry = entryDirective;

    pc = sawOrg ? firstOrg : 0x8000;
    scopePrefix.clear();
    collectAnon = false;
    std::vector<uint8_t> lineBytes;
    AssemblerListingLine listingEntry;
    std::vector<AssembledProgram::Segment> segs;
    int segStart = -1;
    uint16_t lastWritten = 0;
    auto writeByte = [&](uint8_t b) {
        if (segStart < 0) {
            segStart = pc;
        } else if (pc != static_cast<uint16_t>(lastWritten + 1)) {
            segs.push_back({static_cast<uint16_t>(segStart), lastWritten});
            segStart = pc;
        }
        program.memory[pc] = b;
        program.written[pc] = true;
        program.byteCount++;
        program.lowestAddress = std::min(program.lowestAddress, pc);
        program.highestAddress = std::max(program.highestAddress, pc);
        lastWritten = pc;
        pc = static_cast<uint16_t>(pc + 1);
        lineBytes.push_back(b);
    };
    auto pushListing = [&]() {
        AssemblerListingLine entryCopy = listingEntry;
        entryCopy.bytes = lineBytes;
        program.listing.push_back(std::move(entryCopy));
    };

    // ---------------- pass 2: emission ----------------
    for (std::size_t i = 0; i < lines.size(); ++i) {
        Line &l = lines[i];
        Assembler::ParsedLine &p = l.parsed;
        if (!activeMask[i]) continue;
        AsmState::anon.pc = pc;
        AsmState::refs.line = l.displayLine;
        const bool isEquLike2 = (p.mnemonic == ".EQU" || p.mnemonic == ".DEFINE");
        if (p.hasLabel && !isEquLike2 && !defineLabel(l, pc, true)) return false;
        program.lineAddresses[l.displayLine] = pc;

        listingEntry = AssemblerListingLine{};
        listingEntry.address = pc;
        listingEntry.sourceLine = l.displayLine;
        listingEntry.source = l.source;
        lineBytes.clear();

        if (p.mnemonic.empty()) {
            if (p.hasLabel) pushListing();
            continue;
        }

        if (p.mnemonic == ".ORG") {
            uint16_t v = 0;
            std::string what;
            if (evalLine(l, p.operands, pc, v, what)) {
                pc = v;
                listingEntry.address = pc;
            }
            pushListing();
            continue;
        }
        if (p.mnemonic == ".EQU" || p.mnemonic == ".DEFINE" || p.mnemonic == ".ENTRY" ||
            p.mnemonic == ".ASSERT" || p.mnemonic == ".ERROR" || p.mnemonic == ".WARN" ||
            p.mnemonic == ".SEGMENT" || p.mnemonic == ".IF" || p.mnemonic == ".IFDEF" ||
            p.mnemonic == ".IFNDEF" || p.mnemonic == ".ELSE" || p.mnemonic == ".ELIF" ||
            p.mnemonic == ".ENDIF") {
            continue;
        }
        if (p.mnemonic == ".HEX") {
            std::string hexOnly;
            for (char c : p.operands)
                if (std::isxdigit(static_cast<unsigned char>(c))) hexOnly += c;
            for (std::size_t k = 0; k + 1 < hexOnly.size(); k += 2) {
                const int hi = digitValue(hexOnly[k]);
                const int lo = digitValue(hexOnly[k + 1]);
                if (hi >= 0 && lo >= 0) writeByte(static_cast<uint8_t>((hi << 4) | lo));
            }
            pushListing();
            continue;
        }
        if (p.mnemonic == ".ALIGN") {
            const auto parts = Assembler::splitOperands(p.operands);
            uint16_t align = 0;
            std::string what;
            if (!parts.empty() && evalLine(l, parts[0], pc, align, what) && align != 0) {
                const uint16_t rem = static_cast<uint16_t>(pc % align);
                if (rem != 0) {
                    uint16_t fill = 0x00;
                    if (parts.size() > 1) {
                        uint16_t fv = 0;
                        if (evalLine(l, parts[1], pc, fv, what)) fill = static_cast<uint8_t>(fv);
                    }
                    const uint16_t pad = static_cast<uint16_t>(align - rem);
                    for (uint16_t k = 0; k < pad; ++k) writeByte(static_cast<uint8_t>(fill));
                }
            }
            pushListing();
            continue;
        }
        if (p.mnemonic == ".BYTE" || p.mnemonic == ".DB") {
            for (const auto &token : Assembler::splitOperands(p.operands)) {
                if (!token.empty() && (token.front() == '"' || token.front() == '\'')) {
                    std::vector<uint8_t> str;
                    std::string what;
                    if (!Assembler::parseStringLiteral(token, str, what)) {
                        errors.push_back({l.number, ".byte: " + what, l.column, l.file});
                        continue;
                    }
                    for (uint8_t b : str) writeByte(b);
                } else {
                    uint16_t v = 0;
                    std::string what;
                    if (!evalLine(l, token, pc, v, what)) {
                        errors.push_back({l.number, ".byte: " + what, l.column, l.file});
                        continue;
                    }
                    if (v > 0xFF && !(v >= 0xFF00)) {
                        errors.push_back({l.number, ".byte: value out of range", l.column, l.file});
                        continue;
                    }
                    writeByte(static_cast<uint8_t>(v & 0xFF));
                }
            }
            pushListing();
            continue;
        }
        if (p.mnemonic == ".WORD" || p.mnemonic == ".DW") {
            for (const auto &token : Assembler::splitOperands(p.operands)) {
                uint16_t v = 0;
                std::string what;
                if (!evalLine(l, token, pc, v, what)) {
                    errors.push_back({l.number, ".word: " + what, l.column, l.file});
                    continue;
                }
                writeByte(static_cast<uint8_t>(v & 0xFF));
                writeByte(static_cast<uint8_t>((v >> 8) & 0xFF));
            }
            pushListing();
            continue;
        }
        if (p.mnemonic == ".ASCII" || p.mnemonic == ".ASCIZ") {
            std::vector<uint8_t> str;
            std::string what;
            auto evalFn = [&](const std::string &txt, uint16_t &o, std::string &e) {
                return evalLine(l, txt, pc, o, e);
            };
            if (resolveStringOperand(p.operands, evalFn, str, what)) {
                if (!what.empty()) {
                    errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                    continue;
                }
            } else if (!Assembler::parseStringLiteral(p.operands, str, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            for (uint8_t b : str) writeByte(b);
            if (p.mnemonic == ".ASCIZ") writeByte(0);
            pushListing();
            continue;
        }
        if (p.mnemonic == ".RES" || p.mnemonic == ".FILL") {
            const auto ops = Assembler::splitOperands(p.operands);
            if (ops.empty()) {
                errors.push_back({l.number, p.mnemonic + " requires a size", l.column, l.file});
                continue;
            }
            uint16_t n = 0, fill = 0;
            std::string what;
            if (!evalLine(l, ops[0], pc, n, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            if (ops.size() > 1 && !evalLine(l, ops[1], pc, fill, what)) {
                errors.push_back({l.number, p.mnemonic + ": " + what, l.column, l.file});
                continue;
            }
            for (uint16_t k = 0; k < n; ++k) writeByte(static_cast<uint8_t>(fill));
            pushListing();
            continue;
        }
        if (p.mnemonic == ".END") break;
        if (!p.mnemonic.empty() && p.mnemonic.front() == '.') continue;

        std::string what;
        // Reuse pass 1's mode: pass 1 sized the instruction with a partial
        // symbol table, so re-detecting here could pick a different variant
        // (e.g. absolute vs zero page) and desync the PC.
        Assembler::AddrMode mode = l.pass1Mode;
        if (mode == Assembler::AddrMode::Invalid) mode = Assembler::detectMode(p, symbols, what);
        if (mode == Assembler::AddrMode::Invalid) {
            errors.push_back({l.number, what, l.column, l.file});
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!Assembler::encodeInstruction(p, mode, pc, symbols, bytes, what)) {
            errors.push_back({l.number, what, l.column, l.file});
            continue;
        }
        for (uint8_t b : bytes) writeByte(b);
        pushListing();

        // Warnings (never fatal).
        const Op *wi = Assembler::findOpcode(p.mnemonic, mode);
        if (wi) {
            const bool unofficial = illegalMnemonicSet().count(p.mnemonic) > 0;
            if (unofficial) {
                if (options.warnLevel & WARN_UNOFFICIAL) {
                    program.warnings.push_back({l.displayLine, l.column,
                                                "unofficial opcode '" + p.mnemonic + "' used",
                                                WARN_UNOFFICIAL});
                }
            } else if (options.warnLevel & WARN_PAGE_CROSS &&
                       (mode == Assembler::AddrMode::AbsoluteX || mode == Assembler::AddrMode::AbsoluteY ||
                        mode == Assembler::AddrMode::IndirectIndexed)) {
                const std::string base = extractIndexedBase(p.operands, mode);
                uint16_t bv = 0;
                std::string berr;
                std::map<std::string, uint16_t> none;
                if (isNumericLiteral(base) && evalToU16(base, bv, none, nullptr, "", berr) &&
                    (bv & 0xFF) == 0xFFu) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "$%04X", static_cast<unsigned>(bv));
                    program.warnings.push_back(
                        {l.displayLine, l.column,
                         std::string("indexed access from ") + buf + " likely crosses a page boundary",
                         WARN_PAGE_CROSS});
                }
            }
        }
    }

    // Finalize segments: flush trailing run, sort and merge adjacent/overlapping runs.
    if (segStart >= 0) segs.push_back({static_cast<uint16_t>(segStart), lastWritten});
    std::sort(segs.begin(), segs.end(),
              [](const AssembledProgram::Segment &a, const AssembledProgram::Segment &b) {
                  return a.start < b.start;
              });
    for (const auto &seg : segs) {
        if (!program.segments.empty()) {
            auto &backSeg = program.segments.back();
            if (static_cast<uint32_t>(seg.start) <= static_cast<uint32_t>(backSeg.end) + 1u) {
                backSeg.end = std::max(backSeg.end, seg.end);
                continue;
            }
        }
        program.segments.push_back(seg);
    }
    for (auto &kv : refMap) {
        std::vector<int> refs(kv.second.begin(), kv.second.end());
        std::sort(refs.begin(), refs.end());
        program.symbolReferences[kv.first] = std::move(refs);
    }
    program.symbolDefinitionLines = symDefLines;

    if (!errors.empty()) return false;
    program.symbols = symbols;
    program.entryPoint = entry;
    return true;
}

// Wrapper: remap sequential line ids in errors to per-file display lines and
// attach file names (unless already attributed). Needed for plain assemble()
// too, where .include lines carry ids >= 1000001.
bool assembleInternal(const std::vector<SourceLine> &raw, const AssembleOptions &options,
                      AssembledProgram &program, std::vector<AssemblerError> &errors, bool projectMode) {
    std::vector<SourceLine> proc;
    const bool ok = assembleCore(raw, proc, options, program, errors, projectMode);
    std::unordered_map<int, const SourceLine *> bySeq;
    bySeq.reserve(proc.size());
    for (const auto &sl : proc) bySeq.emplace(sl.number, &sl);
    for (auto &e : errors) {
        const auto it = bySeq.find(e.line);
        if (it == bySeq.end()) continue;
        if (e.fileName.empty()) e.fileName = it->second->fileName;
        if (it->second->displayNumber > 0) e.line = it->second->displayNumber;
    }
    return ok;
}

} // namespace

bool Assembler::assemble(const std::string &source, AssembledProgram &program,
                         std::vector<AssemblerError> &errors, const AssembleOptions &options) const {
    std::vector<SourceLine> raw;
    {
        std::istringstream stream(source);
        std::string text;
        int number = 0;
        while (std::getline(stream, text)) raw.push_back({text, ++number, number, "", ""});
        if (raw.empty()) raw.push_back({"", 1, 1, "", ""});
    }
    return assembleInternal(raw, options, program, errors, false);
}

bool Assembler::assembleProject(const std::vector<std::pair<std::string, std::string>> &files,
                                AssembledProgram &program, std::vector<AssemblerError> &errors,
                                const AssembleOptions &options) const {
    std::vector<SourceLine> raw;
    int global = 0;
    for (const auto &f : files) {
        std::string dir;
        if (!f.first.empty()) {
            try {
                dir = std::filesystem::path(f.first).parent_path().string();
            } catch (...) {
            }
        }
        if (dir.empty()) dir = options.basePath.empty() ? "." : options.basePath;
        std::istringstream stream(f.second);
        std::string text;
        int local = 0;
        bool any = false;
        while (std::getline(stream, text)) {
            ++local;
            raw.push_back({text, ++global, local, f.first, dir});
            any = true;
        }
        if (!any) raw.push_back({"", ++global, 1, f.first, dir});
    }
    if (raw.empty()) raw.push_back({"", ++global, 1, "", ""});
    return assembleInternal(raw, options, program, errors, true);
}
