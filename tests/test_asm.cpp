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

    return g_asm_fail;
}
