// Assembler tests: exact encodings, forward references (PC consistency across
// passes), directives, error reporting, parseValue.
#include "assembler.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int g_asm_pass = 0;
int g_asm_fail = 0;

#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (expr) {                                                                          \
            ++g_asm_pass;                                                                    \
        } else {                                                                             \
            ++g_asm_fail;                                                                    \
            std::fprintf(stderr, "FAIL test_asm.cpp:%d %s\n", __LINE__, #expr);              \
        }                                                                                    \
    } while (0)

namespace {

Assembler asm_;

bool assembleStr(const std::string &src, AssembledProgram &p, std::vector<AssemblerError> &errs) {
    errs.clear();
    p = AssembledProgram{};
    return asm_.assemble(src, p, errs);
}

void dumpErrors(const std::vector<AssemblerError> &errs) {
    for (const auto &e : errs)
        std::fprintf(stderr, "  asm error line %d: %s\n", e.line, e.message.c_str());
}

bool sym(const AssembledProgram &p, const char *name, uint16_t &out) {
    const auto it = p.symbols.find(Assembler::normalizeSymbol(name));
    if (it == p.symbols.end()) return false;
    out = it->second;
    return true;
}

using Mode = Assembler::AddrMode;

struct OpRow {
    const char *mn;
    Mode mode;
    uint8_t op;
};

// Representative operand per addressing mode. Zero-page forms use $42 and
// absolute forms use $1234 so neither can silently collapse into the other.
const char *operandFor(Mode m) {
    switch (m) {
    case Mode::Implied:
        return "";
    case Mode::Accumulator:
        return "A";
    case Mode::Immediate:
        return "#$42";
    case Mode::ZeroPage:
        return "$42";
    case Mode::ZeroPageX:
        return "$42,X";
    case Mode::ZeroPageY:
        return "$42,Y";
    case Mode::Absolute:
        return "$1234";
    case Mode::AbsoluteX:
        return "$1234,X";
    case Mode::AbsoluteY:
        return "$1234,Y";
    case Mode::Indirect:
        return "($1234)";
    case Mode::IndexedIndirect:
        return "($42,X)";
    case Mode::IndirectIndexed:
        return "($42),Y";
    case Mode::Relative:
        return "*"; // branch to its own address: displacement $FE
    case Mode::IndirectAbsoluteX:
        return "($1234,X)";
    default:
        return "";
    }
}

// Instruction length straight from the standard 6502 chart; independent of
// Assembler::modeSize on purpose.
int chartLength(Mode m) {
    switch (m) {
    case Mode::Implied:
    case Mode::Accumulator:
        return 1;
    case Mode::Immediate:
    case Mode::ZeroPage:
    case Mode::ZeroPageX:
    case Mode::ZeroPageY:
    case Mode::IndexedIndirect:
    case Mode::IndirectIndexed:
    case Mode::Relative:
        return 2;
    default:
        return 3;
    }
}

// Canonical NMOS 6502 official opcode chart (151 opcodes), transcribed from the
// standard 6502 opcode table. Independent of Assembler::opcodeTable: any
// disagreement is reported as a failing check, never silently reconciled.
const OpRow kOfficial[] = {
    {"ADC", Mode::Immediate, 0x69},       {"ADC", Mode::ZeroPage, 0x65},
    {"ADC", Mode::ZeroPageX, 0x75},       {"ADC", Mode::Absolute, 0x6D},
    {"ADC", Mode::AbsoluteX, 0x7D},       {"ADC", Mode::AbsoluteY, 0x79},
    {"ADC", Mode::IndexedIndirect, 0x61}, {"ADC", Mode::IndirectIndexed, 0x71},
    {"AND", Mode::Immediate, 0x29},       {"AND", Mode::ZeroPage, 0x25},
    {"AND", Mode::ZeroPageX, 0x35},       {"AND", Mode::Absolute, 0x2D},
    {"AND", Mode::AbsoluteX, 0x3D},       {"AND", Mode::AbsoluteY, 0x39},
    {"AND", Mode::IndexedIndirect, 0x21}, {"AND", Mode::IndirectIndexed, 0x31},
    {"ASL", Mode::Accumulator, 0x0A},     {"ASL", Mode::ZeroPage, 0x06},
    {"ASL", Mode::ZeroPageX, 0x16},       {"ASL", Mode::Absolute, 0x0E},
    {"ASL", Mode::AbsoluteX, 0x1E},       {"BCC", Mode::Relative, 0x90},
    {"BCS", Mode::Relative, 0xB0},        {"BEQ", Mode::Relative, 0xF0},
    {"BIT", Mode::ZeroPage, 0x24},        {"BIT", Mode::Absolute, 0x2C},
    {"BMI", Mode::Relative, 0x30},        {"BNE", Mode::Relative, 0xD0},
    {"BPL", Mode::Relative, 0x10},        {"BRK", Mode::Implied, 0x00},
    {"BVC", Mode::Relative, 0x50},        {"BVS", Mode::Relative, 0x70},
    {"CLC", Mode::Implied, 0x18},         {"CLD", Mode::Implied, 0xD8},
    {"CLI", Mode::Implied, 0x58},         {"CLV", Mode::Implied, 0xB8},
    {"CMP", Mode::Immediate, 0xC9},       {"CMP", Mode::ZeroPage, 0xC5},
    {"CMP", Mode::ZeroPageX, 0xD5},       {"CMP", Mode::Absolute, 0xCD},
    {"CMP", Mode::AbsoluteX, 0xDD},       {"CMP", Mode::AbsoluteY, 0xD9},
    {"CMP", Mode::IndexedIndirect, 0xC1}, {"CMP", Mode::IndirectIndexed, 0xD1},
    {"CPX", Mode::Immediate, 0xE0},       {"CPX", Mode::ZeroPage, 0xE4},
    {"CPX", Mode::Absolute, 0xEC},        {"CPY", Mode::Immediate, 0xC0},
    {"CPY", Mode::ZeroPage, 0xC4},        {"CPY", Mode::Absolute, 0xCC},
    {"DEC", Mode::ZeroPage, 0xC6},        {"DEC", Mode::ZeroPageX, 0xD6},
    {"DEC", Mode::Absolute, 0xCE},        {"DEC", Mode::AbsoluteX, 0xDE},
    {"DEX", Mode::Implied, 0xCA},         {"DEY", Mode::Implied, 0x88},
    {"EOR", Mode::Immediate, 0x49},       {"EOR", Mode::ZeroPage, 0x45},
    {"EOR", Mode::ZeroPageX, 0x55},       {"EOR", Mode::Absolute, 0x4D},
    {"EOR", Mode::AbsoluteX, 0x5D},       {"EOR", Mode::AbsoluteY, 0x59},
    {"EOR", Mode::IndexedIndirect, 0x41}, {"EOR", Mode::IndirectIndexed, 0x51},
    {"INC", Mode::ZeroPage, 0xE6},        {"INC", Mode::ZeroPageX, 0xF6},
    {"INC", Mode::Absolute, 0xEE},        {"INC", Mode::AbsoluteX, 0xFE},
    {"INX", Mode::Implied, 0xE8},         {"INY", Mode::Implied, 0xC8},
    {"JMP", Mode::Absolute, 0x4C},        {"JMP", Mode::Indirect, 0x6C},
    {"JSR", Mode::Absolute, 0x20},        {"LDA", Mode::Immediate, 0xA9},
    {"LDA", Mode::ZeroPage, 0xA5},        {"LDA", Mode::ZeroPageX, 0xB5},
    {"LDA", Mode::Absolute, 0xAD},        {"LDA", Mode::AbsoluteX, 0xBD},
    {"LDA", Mode::AbsoluteY, 0xB9},       {"LDA", Mode::IndexedIndirect, 0xA1},
    {"LDA", Mode::IndirectIndexed, 0xB1}, {"LDX", Mode::Immediate, 0xA2},
    {"LDX", Mode::ZeroPage, 0xA6},        {"LDX", Mode::ZeroPageY, 0xB6},
    {"LDX", Mode::Absolute, 0xAE},        {"LDX", Mode::AbsoluteY, 0xBE},
    {"LDY", Mode::Immediate, 0xA0},       {"LDY", Mode::ZeroPage, 0xA4},
    {"LDY", Mode::ZeroPageX, 0xB4},       {"LDY", Mode::Absolute, 0xAC},
    {"LDY", Mode::AbsoluteX, 0xBC},       {"LSR", Mode::Accumulator, 0x4A},
    {"LSR", Mode::ZeroPage, 0x46},        {"LSR", Mode::ZeroPageX, 0x56},
    {"LSR", Mode::Absolute, 0x4E},        {"LSR", Mode::AbsoluteX, 0x5E},
    {"NOP", Mode::Implied, 0xEA},         {"ORA", Mode::Immediate, 0x09},
    {"ORA", Mode::ZeroPage, 0x05},        {"ORA", Mode::ZeroPageX, 0x15},
    {"ORA", Mode::Absolute, 0x0D},        {"ORA", Mode::AbsoluteX, 0x1D},
    {"ORA", Mode::AbsoluteY, 0x19},       {"ORA", Mode::IndexedIndirect, 0x01},
    {"ORA", Mode::IndirectIndexed, 0x11}, {"PHA", Mode::Implied, 0x48},
    {"PHP", Mode::Implied, 0x08},         {"PLA", Mode::Implied, 0x68},
    {"PLP", Mode::Implied, 0x28},         {"ROL", Mode::Accumulator, 0x2A},
    {"ROL", Mode::ZeroPage, 0x26},        {"ROL", Mode::ZeroPageX, 0x36},
    {"ROL", Mode::Absolute, 0x2E},        {"ROL", Mode::AbsoluteX, 0x3E},
    {"ROR", Mode::Accumulator, 0x6A},     {"ROR", Mode::ZeroPage, 0x66},
    {"ROR", Mode::ZeroPageX, 0x76},       {"ROR", Mode::Absolute, 0x6E},
    {"ROR", Mode::AbsoluteX, 0x7E},       {"RTI", Mode::Implied, 0x40},
    {"RTS", Mode::Implied, 0x60},         {"SBC", Mode::Immediate, 0xE9},
    {"SBC", Mode::ZeroPage, 0xE5},        {"SBC", Mode::ZeroPageX, 0xF5},
    {"SBC", Mode::Absolute, 0xED},        {"SBC", Mode::AbsoluteX, 0xFD},
    {"SBC", Mode::AbsoluteY, 0xF9},       {"SBC", Mode::IndexedIndirect, 0xE1},
    {"SBC", Mode::IndirectIndexed, 0xF1}, {"SEC", Mode::Implied, 0x38},
    {"SED", Mode::Implied, 0xF8},         {"SEI", Mode::Implied, 0x78},
    {"STA", Mode::ZeroPage, 0x85},        {"STA", Mode::ZeroPageX, 0x95},
    {"STA", Mode::Absolute, 0x8D},        {"STA", Mode::AbsoluteX, 0x9D},
    {"STA", Mode::AbsoluteY, 0x99},       {"STA", Mode::IndexedIndirect, 0x81},
    {"STA", Mode::IndirectIndexed, 0x91}, {"STX", Mode::ZeroPage, 0x86},
    {"STX", Mode::ZeroPageY, 0x96},       {"STX", Mode::Absolute, 0x8E},
    {"STY", Mode::ZeroPage, 0x84},        {"STY", Mode::ZeroPageX, 0x94},
    {"STY", Mode::Absolute, 0x8C},        {"TAX", Mode::Implied, 0xAA},
    {"TAY", Mode::Implied, 0xA8},         {"TSX", Mode::Implied, 0xBA},
    {"TXA", Mode::Implied, 0x8A},         {"TXS", Mode::Implied, 0x9A},
    {"TYA", Mode::Implied, 0x98},
};

// 65C02-only opcodes the assembler's table claims to support (gated at runtime
// on mnemonicExists so only what the table advertises is exercised).
const OpRow kCore65C02[] = {
    {"BRA", Mode::Relative, 0x80},          {"INA", Mode::Implied, 0x1A},
    {"DEA", Mode::Implied, 0x3A},           {"PHY", Mode::Implied, 0x5A},
    {"PLY", Mode::Implied, 0x7A},           {"PHX", Mode::Implied, 0xDA},
    {"PLX", Mode::Implied, 0xFA},           {"TSB", Mode::ZeroPage, 0x04},
    {"TSB", Mode::Absolute, 0x0C},          {"TRB", Mode::ZeroPage, 0x14},
    {"TRB", Mode::Absolute, 0x1C},          {"STZ", Mode::ZeroPage, 0x64},
    {"STZ", Mode::ZeroPageX, 0x74},         {"STZ", Mode::Absolute, 0x9C},
    {"STZ", Mode::AbsoluteX, 0x9E},         {"BIT", Mode::Immediate, 0x89},
    {"BIT", Mode::ZeroPageX, 0x34},         {"BIT", Mode::AbsoluteX, 0x3C},
    {"JMP", Mode::IndirectAbsoluteX, 0x7C}, {"WAI", Mode::Implied, 0xCB},
    {"STP", Mode::Implied, 0xDB},
};

} // namespace

int run_test_asm() {
    AssembledProgram p;
    std::vector<AssemblerError> errs;
    bool ok = false;

    // ---- exact bytes + entryPoint ----
    const std::string src1 =
        "        .org $8000\n"
        "start:  LDA #$05\n"
        "        STA $0200\n"
        "        JMP start\n";
    ok = assembleStr(src1, p, errs);
    CHECK(ok);
    if (!ok) dumpErrors(errs);
    if (ok) {
        const uint8_t want[8] = {0xA9, 0x05, 0x8D, 0x00, 0x02, 0x4C, 0x00, 0x80};
        bool bytesOk = true;
        for (int i = 0; i < 8; ++i) {
            if (!p.written[0x8000 + i] || p.memory[0x8000 + i] != want[i]) bytesOk = false;
        }
        CHECK(bytesOk);
        CHECK(p.entryPoint == 0x8000);
        CHECK(p.byteCount == 8);
        uint16_t start = 0;
        CHECK(sym(p, "start", start) && start == 0x8000);
        CHECK(!p.segments.empty() && p.segments[0].start == 0x8000 && p.segments[0].end == 0x8007);
    }

    // ---- CRITICAL: forward reference resolving to <= $FF must keep every
    // label/instruction address self-consistent (both passes agree on sizes). ----
    const std::string src2 =
        "        .org $0010\n"
        "begin:  LDA fwd\n"
        "        STA $0300\n"
        "        JMP done\n"
        "fwd:    .byte $77\n"
        "        NOP\n"
        "done:   .byte $AA\n";
    ok = assembleStr(src2, p, errs);
    CHECK(ok);
    if (!ok) dumpErrors(errs);
    if (ok) {
        uint16_t BEGIN = 0, FWD = 0, DONE = 0;
        const bool haveSyms = sym(p, "begin", BEGIN) && sym(p, "fwd", FWD) && sym(p, "done", DONE);
        CHECK(haveSyms);
        if (haveSyms) {
            CHECK(BEGIN == 0x0010);
            const uint8_t op = p.memory[0x0010];
            CHECK(op == 0xAD || op == 0xA5); // absolute or zero-page LDA
            const int ldaSize = (op == 0xAD) ? 3 : 2;
            CHECK(p.memory[0x0011] == (FWD & 0xFF));
            if (op == 0xAD) CHECK(p.memory[0x0012] == 0x00);
            const int sta = 0x0010 + ldaSize;
            CHECK(p.memory[sta] == 0x8D && p.memory[sta + 1] == 0x00 && p.memory[sta + 2] == 0x03);
            const int jmp = sta + 3;
            CHECK(p.memory[jmp] == 0x4C);
            CHECK(p.memory[jmp + 1] == (DONE & 0xFF));
            CHECK(p.memory[jmp + 2] == ((DONE >> 8) & 0xFF));
            // Later labels must land exactly where the emitted layout says.
            CHECK(FWD == jmp + 3);
            CHECK(DONE == FWD + 2);
            CHECK(p.memory[FWD] == 0x77);
            CHECK(p.memory[DONE] == 0xAA);
        }
    }

    // ---- STX/STY with a forward label must not error ----
    const std::string src3 =
        "        .org $0020\n"
        "        STX fwd2\n"
        "        STY fwd2\n"
        "fwd2:   .byte $55\n";
    ok = assembleStr(src3, p, errs);
    CHECK(ok);
    if (!ok) dumpErrors(errs);
    if (ok) {
        uint16_t FWD2 = 0;
        CHECK(sym(p, "fwd2", FWD2));
        int pos = 0x20;
        const uint8_t opx = p.memory[pos];
        CHECK(opx == 0x86 || opx == 0x8E);
        CHECK(p.memory[pos + 1] == (FWD2 & 0xFF));
        if (opx == 0x8E) CHECK(p.memory[pos + 2] == 0x00);
        pos += (opx == 0x86) ? 2 : 3;
        const uint8_t opy = p.memory[pos];
        CHECK(opy == 0x84 || opy == 0x8C);
        CHECK(p.memory[pos + 1] == (FWD2 & 0xFF));
        pos += (opy == 0x84) ? 2 : 3;
        CHECK(FWD2 == pos);
        CHECK(p.memory[FWD2] == 0x55);
    }

    // ---- forward label resolving above $00FF under a locked zero-page mode
    // (STX zp,Y is the only variant) must error, not silently truncate ----
    const std::string srcHigh =
        "        .org $0020\n"
        "        STX high,Y\n"
        "high:   .equ $FF10\n";
    ok = assembleStr(srcHigh, p, errs);
    CHECK(!ok);
    {
        bool rangeErr = false;
        for (const auto &e : errs)
            if (e.message.find("out of range") != std::string::npos) rangeErr = true;
        CHECK(rangeErr);
    }

    // ---- branch range error ----
    // Negative immediate still allowed: `LDA #-1` -> $A9 $FF (0xFFFF carve-out
    // survives for Immediate mode only).
    {
        const std::string srcNeg = "        .org $8000\n"
                                   "        LDA #-1\n";
        ok = assembleStr(srcNeg, p, errs);
        CHECK(ok);
        if (ok) CHECK(p.memory[0x8000] == 0xA9 && p.memory[0x8001] == 0xFF);
    }
    const std::string src4 =
        "        .org $8000\n"
        "        BNE faraway\n"
        "        .org $8200\n"
        "faraway: NOP\n";
    ok = assembleStr(src4, p, errs);
    CHECK(!ok);
    CHECK(!errs.empty());
    if (!errs.empty()) {
        CHECK(errs[0].message.find("range") != std::string::npos);
        CHECK(errs[0].line >= 1 && errs[0].line <= 4);
    }

    // ---- duplicate label error ----
    const std::string src5 =
        "        .org $8000\n"
        "dup:    NOP\n"
        "dup:    NOP\n";
    ok = assembleStr(src5, p, errs);
    CHECK(!ok);
    CHECK(!errs.empty());
    bool foundDup = false;
    bool linesInRange = !errs.empty();
    for (const auto &e : errs) {
        if (e.message.find("duplicate label") != std::string::npos) foundDup = true;
        if (e.line < 1 || e.line > 3) linesInRange = false;
    }
    CHECK(foundDup);
    CHECK(linesInRange);

    // ---- error line numbers within source line count ----
    const std::string src6 =
        "        .org $8000\n"
        "        LDA #$01\n"
        "        LDA #$1FF\n";
    ok = assembleStr(src6, p, errs);
    CHECK(!ok);
    CHECK(!errs.empty());
    bool linesOk = !errs.empty();
    for (const auto &e : errs) {
        if (e.line < 1 || e.line > 3) linesOk = false;
    }
    CHECK(linesOk);

    // ---- colon inside char literal must not produce a label error ----
    const std::string src7 =
        "        .org $8000\n"
        "        LDA #':'\n";
    ok = assembleStr(src7, p, errs);
    CHECK(ok);
    if (!ok) dumpErrors(errs);
    if (ok) {
        CHECK(p.memory[0x8000] == 0xA9);
        CHECK(p.memory[0x8001] == 0x3A);
        CHECK(errs.empty());
    }

    // ---- .byte / .word / .org / .equ round-trip ----
    const std::string src8 =
        "        .org $C000\n"
        "CONST   .equ $1234\n"
        "data:   .byte $01, $02, $FF\n"
        "words:  .word $ABCD, CONST\n";
    ok = assembleStr(src8, p, errs);
    CHECK(ok);
    if (!ok) dumpErrors(errs);
    if (ok) {
        CHECK(p.entryPoint == 0xC000);
        CHECK(p.memory[0xC000] == 0x01 && p.memory[0xC001] == 0x02 && p.memory[0xC002] == 0xFF);
        CHECK(p.memory[0xC003] == 0xCD && p.memory[0xC004] == 0xAB);
        CHECK(p.memory[0xC005] == 0x34 && p.memory[0xC006] == 0x12);
        uint16_t CONST = 0, DATA = 0, WORDS = 0;
        CHECK(sym(p, "const", CONST) && CONST == 0x1234);
        CHECK(sym(p, "data", DATA) && DATA == 0xC000);
        CHECK(sym(p, "words", WORDS) && WORDS == 0xC003);
    }

    // ---- parseValue: hex / bin / dec ----
    uint16_t v = 0;
    CHECK(Assembler::parseValue("$1F", v) && v == 0x001F);
    CHECK(Assembler::parseValue("$FFFF", v) && v == 0xFFFF);
    CHECK(Assembler::parseValue("%1010", v) && v == 10);
    CHECK(Assembler::parseValue("42", v) && v == 42);
    CHECK(!Assembler::parseValue("$", v));
    CHECK(!Assembler::parseValue("undefined_sym_xyz", v));

    // ---- defect 1: arithmetic core wraps in uint32 (no signed-overflow UB) ----
    {
        // 65535*65535 == $FFFE0001: defined wrap, but the result does not fit
        // 16 bits -> deterministic line-numbered error.
        ok = assembleStr(".org $8000\n.word 65535*65535\n", p, errs);
        CHECK(!ok);
        {
            bool found = false;
            for (const auto &e : errs)
                if (e.line == 2 && e.message.find("16-bit range") != std::string::npos) found = true;
            CHECK(found);
        }
        // Same expression consumed through <(...) and a mask stays deterministic.
        ok = assembleStr(".org $8000\nLDA #<(65535*65535)\n.word (65535*65535)&$FFFF\n", p, errs);
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) {
            CHECK(p.memory[0x8000] == 0xA9 && p.memory[0x8001] == 0x01);
            CHECK(p.memory[0x8002] == 0x01 && p.memory[0x8003] == 0x00);
            CHECK(p.byteCount == 4);
        }
        // INT32_MIN / -1 wraps (never UB) and is reported at the 16-bit boundary.
        uint16_t ev = 0;
        std::string evErr;
        CHECK(!Assembler::evalExpression("(-2147483647-1)/(-1)", ev, {}, evErr) && !evErr.empty());
        ok = assembleStr(".org $8000\n.word (-2147483647-1)/(-1)\n", p, errs);
        CHECK(!ok);
        {
            bool found = false;
            for (const auto &e : errs)
                if (e.line == 2 && !e.message.empty()) found = true;
            CHECK(found);
        }
        // INT32_MIN % -1 wraps deterministically to 0.
        evErr.clear();
        ev = 0;
        CHECK(Assembler::evalExpression("(-2147483647-1)%(-1)", ev, {}, evErr) && ev == 0);
        // Signed addition overflow wraps instead of invoking UB.
        evErr.clear();
        CHECK(!Assembler::evalExpression("2147483647+1", ev, {}, evErr) && !evErr.empty());
        // Ordinary expressions keep their meaning.
        evErr.clear();
        CHECK(Assembler::evalExpression("(1+2)*3", ev, {}, evErr) && ev == 9);
    }

    // ---- defect 2: stripComment honors backslash escapes inside strings ----
    {
        // ';' inside a string with an escaped quote is data, not a comment.
        ok = assembleStr(".org $8000\n.byte \"a\\\";b\"\n", p, errs); // .byte "a\";b"
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) {
            CHECK(p.byteCount == 4);
            CHECK(p.memory[0x8000] == 0x61 && p.memory[0x8001] == 0x22 && p.memory[0x8002] == 0x3B &&
                  p.memory[0x8003] == 0x62);
        }
        // A ';' after the closed string (escaped quote inside) is a comment again.
        ok = assembleStr(".org $8000\n.byte \"x\\\"y\" ; tail comment\n", p, errs); // .byte "x\"y" ; tail
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok)
            CHECK(p.byteCount == 3 && p.memory[0x8000] == 0x78 && p.memory[0x8001] == 0x22 &&
                  p.memory[0x8002] == 0x79);
        // Trailing ';' as the last char of the string survives too.
        ok = assembleStr(".org $8000\n.byte \"a\\\";\"\n", p, errs); // .byte "a\";"
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) CHECK(p.byteCount == 3 && p.memory[0x8002] == 0x3B);
        // Escaped quote immediately before ',' must not split the operand list.
        ok = assembleStr(".org $8000\n.byte \"a\\\"\",2\n", p, errs); // .byte "a\"",2
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok)
            CHECK(p.byteCount == 3 && p.memory[0x8000] == 0x61 && p.memory[0x8001] == 0x22 &&
                  p.memory[0x8002] == 0x02);
    }

    // ---- defect 3: .repeat / macro defaults evaluate against .equ constants ----
    {
        ok = assembleStr(".org $8000\nN .equ 3\n.repeat N\n        NOP\n.endr\n", p, errs);
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) {
            CHECK(p.byteCount == 3);
            CHECK(p.memory[0x8000] == 0xEA && p.memory[0x8001] == 0xEA && p.memory[0x8002] == 0xEA);
        }
        const std::string srcMacro =
            ".org $8000\n"
            "K .equ 2\n"
            ".macro emit val=K\n"
            "        .byte val\n"
            ".endm\n"
            "        emit\n"
            "        emit 7\n";
        ok = assembleStr(srcMacro, p, errs);
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) CHECK(p.byteCount == 2 && p.memory[0x8000] == 0x02 && p.memory[0x8001] == 0x07);
    }

    // ---- defect 4: numeric literals above INT32_MAX are rejected ----
    {
        ok = assembleStr(".org $8000\n.byte 4294967295\n", p, errs);
        CHECK(!ok);
        {
            bool found = false;
            for (const auto &e : errs)
                if (e.line == 2 && e.message.find("decimal constant out of range") != std::string::npos)
                    found = true;
            CHECK(found);
        }
        ok = assembleStr(".org $8000\n.byte $FFFFFFFF\n", p, errs);
        CHECK(!ok);
        {
            bool found = false;
            for (const auto &e : errs)
                if (e.line == 2 && e.message.find("hex constant out of range") != std::string::npos) found = true;
            CHECK(found);
        }
        // 16-bit literals are unaffected...
        ok = assembleStr(".org $8000\n.word $FFFF\n.byte 255\n", p, errs);
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) CHECK(p.memory[0x8000] == 0xFF && p.memory[0x8001] == 0xFF && p.memory[0x8002] == 0xFF);
        // ...and the -1 immediate carve-out (unary minus, not a literal) survives.
        ok = assembleStr(".org $8000\n.byte -1\n", p, errs);
        CHECK(ok);
        if (ok) CHECK(p.memory[0x8000] == 0xFF);
    }

    // ---- defect 5: entry precedence (.entry > START > RESET > first .org) ----
    {
        ok = assembleStr(".org $8000\nRESET:  NOP\n        .org $8100\nSTART:  NOP\n", p, errs);
        CHECK(ok);
        if (!ok) dumpErrors(errs);
        if (ok) CHECK(p.entryPoint == 0x8100); // START deliberately wins over RESET
        ok = assembleStr(".org $8000\nRESET:  NOP\n", p, errs);
        CHECK(ok);
        if (ok) CHECK(p.entryPoint == 0x8000);
        ok = assembleStr(".org $8000\nRESET:  NOP\n        .org $8100\nSTART:  NOP\n        .entry $8000\n", p,
                         errs);
        CHECK(ok);
        if (ok) CHECK(p.entryPoint == 0x8000);
    }

    // ---- defect 6: table-driven encoding matrix (canonical chart) ----
    static_assert(sizeof(kOfficial) / sizeof(kOfficial[0]) == 151,
                  "chart must cover every official NMOS opcode");
    CHECK(sizeof(kOfficial) / sizeof(kOfficial[0]) == 151);
    auto runRow = [&](const OpRow &r) {
        const char *opnd = operandFor(r.mode);
        std::string src = std::string(".org $8000\n        ") + r.mn;
        if (*opnd != '\0') src += std::string(" ") + opnd;
        src += "\n";
        const bool rowOk = assembleStr(src, p, errs);
        if (!rowOk) {
            std::fprintf(stderr, "FAIL matrix assemble %s [%s]:\n", r.mn, opnd);
            dumpErrors(errs);
        }
        CHECK(rowOk);
        if (rowOk) {
            const bool opOk = p.memory[0x8000] == r.op;
            if (!opOk)
                std::fprintf(stderr, "FAIL matrix opcode %s [%s]: got $%02X want $%02X\n", r.mn, opnd,
                             p.memory[0x8000], r.op);
            CHECK(opOk);
            const bool lenOk = p.byteCount == static_cast<std::size_t>(chartLength(r.mode));
            if (!lenOk)
                std::fprintf(stderr, "FAIL matrix length %s [%s]: got %zu want %d\n", r.mn, opnd, p.byteCount,
                             chartLength(r.mode));
            CHECK(lenOk);
        }
    };
    for (const OpRow &r : kOfficial) runRow(r);
    // Cross-check the chart against the implementation's opcodeTable. A mismatch
    // is printed and fails here; the chart above is never edited to fit.
    auto crossCheck = [&](const OpRow &r) {
        const Assembler::OpcodeInfo *info = Assembler::findOpcode(r.mn, r.mode);
        bool okRow = true;
        if (!info) {
            okRow = false;
            std::fprintf(stderr, "FAIL opcodeTable missing %s mode=%d (chart $%02X)\n", r.mn,
                         static_cast<int>(r.mode), r.op);
        } else if (info->opcode != r.op) {
            okRow = false;
            std::fprintf(stderr, "FAIL opcodeTable mismatch %s mode=%d: table $%02X vs chart $%02X\n", r.mn,
                         static_cast<int>(r.mode), info->opcode, r.op);
        }
        CHECK(okRow);
    };
    for (const OpRow &r : kOfficial) crossCheck(r);

    // ---- 65C02-only opcodes (no core gating in the assembler: always enabled) ----
    for (const OpRow &r : kCore65C02) {
        if (!Assembler::mnemonicExists(r.mn)) continue;
        runRow(r);
        crossCheck(r);
    }

    // ---- every relative displacement -128..+127 for all branch mnemonics ----
    {
        static const struct {
            const char *mn;
            uint8_t op;
        } kBr[] = {{"BCC", 0x90}, {"BCS", 0xB0}, {"BEQ", 0xF0}, {"BMI", 0x30}, {"BNE", 0xD0},
                   {"BPL", 0x10}, {"BVC", 0x50}, {"BVS", 0x70}, {"BRA", 0x80}};
        int branchFail = 0;
        for (const auto &br : kBr) {
            if (!Assembler::mnemonicExists(br.mn)) continue;
            for (int off = -128; off <= 127; ++off) {
                const unsigned target = (0x8002u + static_cast<unsigned>(off)) & 0xFFFFu;
                char srcBuf[128];
                // Target via forward ".equ" (not emitted code): offsets -2/-1 land
                // inside the instruction itself, so emitting there would self-overwrite.
                std::snprintf(srcBuf, sizeof(srcBuf), ".org $8000\n        %s tgt\ntgt .equ $%04X\n", br.mn, target);
                if (!assembleStr(srcBuf, p, errs)) {
                    ++branchFail;
                    std::fprintf(stderr, "FAIL branch %s off=%d assemble:\n", br.mn, off);
                    dumpErrors(errs);
                    continue;
                }
                const uint8_t want = static_cast<uint8_t>(off & 0xFF);
                if (p.byteCount != 2 || p.memory[0x8000] != br.op || p.memory[0x8001] != want) {
                    ++branchFail;
                    std::fprintf(stderr, "FAIL branch %s off=%d: count=%zu bytes %02X %02X want %02X %02X\n",
                                 br.mn, off, p.byteCount, p.memory[0x8000], p.memory[0x8001], br.op, want);
                }
            }
        }
        CHECK(branchFail == 0);
    }

    // ---- invalid syntax: in-range line numbers, non-empty messages ----
    {
        struct BadCase {
            const char *src;
            int lines;
            const char *what;
        };
        const BadCase kBad[] = {
            {".org $8000\n        NOPX\n", 2, "mnemonic typo"},
            {".org $8000\n        LDA\n", 2, "missing operand"},
            {".org $8000\n        CLC $12\n", 2, "extra operand"},
            {".org $8000\n        LDA ($12\n", 2, "unbalanced paren"},
        };
        for (const BadCase &b : kBad) {
            const bool assembled = assembleStr(b.src, p, errs);
            bool rowOk = !assembled && !errs.empty();
            for (const auto &e : errs) {
                if (e.line < 1 || e.line > b.lines) rowOk = false;
                if (e.message.empty()) rowOk = false;
            }
            if (!rowOk) {
                std::fprintf(stderr, "FAIL syntax %s: assembled=%d errs=%zu\n", b.what, assembled, errs.size());
                dumpErrors(errs);
            }
            CHECK(rowOk);
        }
    }

    return g_asm_fail;
}
