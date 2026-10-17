#include "headless.hpp"
#include "apu.hpp"
#include "assembler.hpp"
#include "debugger.hpp"
#include "emulator.hpp"
#include "hexfile.hpp"
#include "ines.hpp"
#include "remote.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace remote {
std::mutex &emulatorMutex();
}

namespace {

// Portable console output: plain stdio works on every OS and in GUI-subsystem
// builds alike (when no console is attached the output goes to a log-less
// void, matching the previous WriteFile fallback behaviour).
void hprint(const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n <= 0) return;
    std::fwrite(buffer, 1, static_cast<std::size_t>(n), stdout);
    std::fflush(stdout);
}

// Case-insensitive suffix compare (replaces _stricmp, MSVC-only).
bool iendsWith(const std::string &text, const char *suffix) {
    const std::size_t slen = std::strlen(suffix);
    if (text.size() < slen) return false;
    for (std::size_t i = 0; i < slen; ++i) {
        const char a = text[text.size() - slen + i];
        const char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)))
            return false;
    }
    return true;
}

// Escape a message for embedding inside a JSON string literal.
std::string jsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            continue;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        } else
            out += c;
    }
    return out;
}

std::vector<std::string> splitArgs(const std::string &cmd) {
    std::vector<std::string> out;
    std::string current;
    bool quote = false;
    for (char c : cmd) {
        if (c == '"')
            quote = !quote;
        else if (c == ' ' && !quote) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else
            current += c;
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

bool writeBmp24(const std::string &path, int w, int h, const std::vector<uint8_t> &rgb, std::string &error) {
#pragma pack(push, 1)
    struct Bfh {
        uint16_t type;
        uint32_t size;
        uint16_t r1, r2;
        uint32_t off;
    };
    struct Bih {
        uint32_t size;
        int32_t w, h;
        uint16_t planes, bits;
        uint32_t comp, img;
        int32_t xppm, yppm;
        uint32_t used, important;
    };
#pragma pack(pop)
    const std::size_t rowSize = (static_cast<std::size_t>(w) * 3 + 3) & ~std::size_t{3};
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    Bfh fh{0x4D42, 0, 0, 0, sizeof(Bfh) + sizeof(Bih)};
    Bih ih{};
    ih.size = sizeof(Bih);
    ih.w = w;
    ih.h = h;
    ih.planes = 1;
    ih.bits = 24;
    ih.img = static_cast<uint32_t>(rowSize * h);
    fh.size = fh.off + ih.img;
    out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
    for (int y = 0; y < h; ++y)
        out.write(reinterpret_cast<const char *>(&rgb[static_cast<std::size_t>((h - 1 - y)) * w * 3]),
                  static_cast<std::streamsize>(w * 3));
    if (!out) {
        error = "bmp write failed";
        return false;
    }
    return true;
}

} // namespace

int headlessMain(const std::string &commandLine) {
    const auto args = splitArgs(commandLine);

    std::string asmFile, imageFile, screenshotFile, wavFile;
    std::string mapFile, xrefFile, prgFile, nesFile, mesenTraceFile, definesArg, coreArg;
    std::string extraSources;
    uint32_t base = 0x8000;
    uint64_t budgetInstr = 0;
    uint64_t budgetCycles = 0;
    bool runProgram = false;
    int remotePort = 0;
    // --image coverage window: binaries cover base..base+size, iNES images
    // cover the mapped $8000-$FFFF window (assembler metadata is not set).
    bool imageMode = false;
    uint32_t imageCovLow = 0x8000, imageCovHigh = 0xFFFF;

    // Strict unsigned parse: rejects empty text, signs, leading whitespace,
    // trailing junk and overflow, printing a JSON error so the caller can
    // exit 2. Numbers are always base 10 except an explicit 0x/0X prefix -
    // strtoull's base-0 auto-detection would otherwise accept " 5" (leading
    // whitespace) and read "0777" as octal 511.
    auto parseNumber = [](const char *flag, const std::string &text, uint64_t &out,
                          uint64_t max) -> bool {
        const char *s = text.c_str();
        const bool signedText = text.empty() || s[0] == '-' || s[0] == '+';
        const bool wsText = std::isspace(static_cast<unsigned char>(s[0])) != 0;
        int base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16;
        char *end = nullptr;
        errno = 0;
        const unsigned long long n = (signedText || wsText) ? 0ull : std::strtoull(s, &end, base);
        if (signedText || wsText || errno == ERANGE || end == s || *end != '\0' || n > max) {
            hprint("{\"ok\":false,\"error\":\"invalid value for %s: %s\"}\n", flag,
                   jsonEscape(text).c_str());
            return false;
        }
        out = static_cast<uint64_t>(n);
        return true;
    };

    // Flag values are consumed exactly once (the ++i inside value()). A
    // value-taking flag in last position is an error ("missing value for
    // <flag>"), not "unknown argument". Repeated flags take the LAST value
    // (plain re-assignment); -D is the exception and accumulates.
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        bool ok = true;
        auto value = [&](const char *flag) -> std::string {
            if (i + 1 >= args.size()) {
                hprint("{\"ok\":false,\"error\":\"missing value for %s\"}\n", flag);
                ok = false;
                return {};
            }
            return args[++i];
        };
        if (a == "--headless") continue;

        // String-valued flags share one path.
        std::string *target = nullptr;
        if (a == "--asm")
            target = &asmFile;
        else if (a == "--image")
            target = &imageFile;
        else if (a == "--screenshot")
            target = &screenshotFile;
        else if (a == "--waverec")
            target = &wavFile;
        else if (a == "--map")
            target = &mapFile;
        else if (a == "--xref")
            target = &xrefFile;
        else if (a == "--prg")
            target = &prgFile;
        else if (a == "--nes")
            target = &nesFile;
        else if (a == "--mesen-trace")
            target = &mesenTraceFile;
        else if (a == "--link")
            target = &extraSources;
        else if (a == "--core")
            target = &coreArg;
        if (target) {
            const std::string v = value(a.c_str());
            if (!ok) return 2;
            *target = v;
            continue;
        }

        if (a == "-D" || a == "--base" || a == "--instr" || a == "--cycles" || a == "--remote") {
            const std::string v = value(a.c_str());
            if (!ok) return 2;
            if (a == "-D") {
                definesArg = definesArg.empty() ? v : definesArg + "," + v;
            } else if (a == "--base") {
                uint64_t n = 0;
                if (!parseNumber("--base", v, n, 0xFFFF)) return 2; // > $FFFF would truncate
                base = static_cast<uint32_t>(n);
            } else if (a == "--instr") {
                if (!parseNumber("--instr", v, budgetInstr, UINT64_MAX)) return 2;
            } else if (a == "--cycles") {
                if (!parseNumber("--cycles", v, budgetCycles, UINT64_MAX)) return 2;
            } else { // --remote
                uint64_t n = 0;
                if (!parseNumber("--remote", v, n, 65535)) return 2;
                if (n == 0) { // 0 is not a valid TCP port (absent --remote stays off)
                    hprint("{\"ok\":false,\"error\":\"invalid value for --remote: %s\"}\n",
                           jsonEscape(v).c_str());
                    return 2;
                }
                remotePort = static_cast<int>(n);
            }
            continue;
        }

        if (a == "--run") {
            runProgram = true;
            continue;
        }
        hprint("{\"ok\":false,\"error\":\"unknown argument %s\"}\n", jsonEscape(a).c_str());
        return 2;
    }

    // Zero budget with --run is invalid, not a silent no-op: exit 2 with a
    // JSON error (kept from the original behaviour; see test b.(i)).
    if (runProgram && budgetInstr == 0 && budgetCycles == 0) {
        hprint("{\"ok\":false,\"error\":\"--run requires --instr or --cycles\"}\n");
        return 2;
    }

    Emulator emu;
    Debugger dbg(emu);
    dbg.setCoverageEnabled(true);
    emu.apu().init();
    if (!wavFile.empty() && !emu.apu().startWavRecord(wavFile)) {
        hprint("{\"ok\":false,\"error\":\"cannot start wav recording: %s\"}\n",
               jsonEscape(wavFile).c_str());
        return 2;
    }

    RemoteServer &remoteSrv = RemoteServer::instance();
    if (remotePort > 0) {
        std::string err;
        if (!remoteSrv.start(emu, dbg, remotePort, err)) {
            hprint("{\"ok\":false,\"error\":\"remote: %s\"}\n", jsonEscape(err).c_str());
            return 2;
        }
        hprint("remote: %s\n", err.c_str());
    }
    // Stops the remote server on EVERY exit path from here down (including
    // the early error returns); RemoteServer::stop() is a no-op when never
    // started, so this is safe unconditionally.
    struct RemoteGuard {
        RemoteServer &srv;
        ~RemoteGuard() { srv.stop(); }
    } remoteGuard{remoteSrv};

    std::string message;
    if (!asmFile.empty()) {
        std::ifstream in(asmFile, std::ios::binary);
        if (!in) {
            hprint("{\"ok\":false,\"error\":\"cannot open %s\"}\n", jsonEscape(asmFile).c_str());
            return 2;
        }
        std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        AssembleOptions options;
        options.basePath = asmFile;
        // INI-driven include paths + warning level; command-line defines.
        {
            const AppSettings &s = Settings::get();
            std::string cur;
            for (const char c : s.includePaths) {
                if (c == ';') {
                    if (!cur.empty()) options.includePaths.push_back(cur);
                    cur.clear();
                } else
                    cur += c;
            }
            if (!cur.empty()) options.includePaths.push_back(cur);
            options.warnLevel = s.warnLevel;
        }
        options.predefinedSymbols = Assembler::splitDefines(definesArg);

        bool assembled = true;
        if (!extraSources.empty()) {
            // Multi-file link: --asm main.asm --link extra1.asm,extra2.asm
            std::vector<std::pair<std::string, std::string>> files;
            files.push_back({asmFile, source});
            std::string cur;
            for (const char c : extraSources) {
                if (c == ',' || c == ';') {
                    if (!cur.empty()) {
                        std::ifstream f(cur, std::ios::binary);
                        if (!f) {
                            hprint("{\"ok\":false,\"error\":\"cannot open %s\"}\n",
                                   jsonEscape(cur).c_str());
                            return 2;
                        }
                        files.push_back({cur, std::string((std::istreambuf_iterator<char>(f)),
                                                          std::istreambuf_iterator<char>())});
                        cur.clear();
                    }
                } else
                    cur += c;
            }
            if (!cur.empty()) {
                std::ifstream f(cur, std::ios::binary);
                if (!f) {
                    hprint("{\"ok\":false,\"error\":\"cannot open %s\"}\n", jsonEscape(cur).c_str());
                    return 2;
                }
                files.push_back({cur, std::string((std::istreambuf_iterator<char>(f)),
                                                  std::istreambuf_iterator<char>())});
            }
            Assembler projectAsm;
            AssembledProgram linked;
            std::vector<AssemblerError> errors;
            assembled = projectAsm.assembleProject(files, linked, errors, options);
            if (!assembled) {
                std::string msg = "Assembly failed:\n";
                for (const auto &e : errors)
                    msg += "  " + e.fileName + ":" + std::to_string(e.line) + ": " + e.message + "\n";
                hprint("{\"ok\":false,\"stage\":\"assemble\",\"message\":\"%s\"}\n", jsonEscape(msg).c_str());
                return 2;
            }
            // Burn the linked image through the emulator's memory.
            emu.memory().reset(0x00);
            for (std::size_t i = 0; i < linked.memory.size(); ++i)
                if (linked.written[i]) emu.memory().write(static_cast<uint16_t>(i), linked.memory[i]);
            emu.cpu().reset();
            message = "linked " + std::to_string(linked.byteCount) + " bytes";
        } else {
            if (!emu.assembleAndBurn(source, message)) {
                hprint("{\"ok\":false,\"stage\":\"assemble\",\"message\":\"%s\"}\n",
                       jsonEscape(message).c_str());
                return 2;
            }
        }
        hprint("assemble: %s\n", message.c_str());
    } else if (!imageFile.empty()) {
        std::string err;
        ines::NesRom rom;
        if (iendsWith(imageFile, ".nes")) {
            // A .nes path must load as an iNES image; never fall back to raw
            // binary loading (that would map the 16-byte header as CPU memory).
            if (!ines::loadFile(imageFile, rom, err)) {
                hprint("{\"ok\":false,\"error\":\"%s\"}\n", jsonEscape(err).c_str());
                return 2;
            }
            if (!ines::mapInto(emu, rom, err)) {
                hprint("{\"ok\":false,\"error\":\"%s\"}\n", jsonEscape(err).c_str());
                return 2;
            }
            hprint("rom: %s\n", err.c_str());
            imageMode = true;
            imageCovLow = 0x8000;
            imageCovHigh = 0xFFFF;
        } else if (emu.loadBinaryFile(imageFile, static_cast<uint16_t>(base), err)) {
            hprint("image: %s\n", err.c_str());
            imageMode = true;
            imageCovLow = static_cast<uint32_t>(static_cast<uint16_t>(base));
            std::error_code ec;
            const uintmax_t fileSize = std::filesystem::file_size(imageFile, ec);
            imageCovHigh = (ec || fileSize == 0)
                               ? 0xFFFFu
                               : std::min<uint32_t>(0xFFFFu, imageCovLow +
                                                                  static_cast<uint32_t>(fileSize - 1));
        } else {
            hprint("{\"ok\":false,\"error\":\"%s\"}\n", jsonEscape(err).c_str());
            return 2;
        }
    } else {
        hprint("{\"ok\":false,\"error\":\"nothing to do: pass --asm or --image\"}\n");
        return 2;
    }

    uint64_t executed = 0;
    const auto start = std::chrono::steady_clock::now();
    if (runProgram && budgetInstr > 0) {
        emu.start();
        while (executed < budgetInstr) {
            uint64_t got = 0;
            {
                const std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                got = dbg.runInstructions(std::min<uint64_t>(5000, budgetInstr - executed));
            }
            if (got == 0 || emu.cpu().halted()) break;
            executed += got;
        }
    } else if (runProgram && budgetCycles > 0) {
        // --cycles: run until the CPU's total cycle count reaches the target
        // (bounded instruction chunks so breakpoints/halts still stop us).
        emu.start();
        while (emu.cpu().getTotalCycles() < budgetCycles) {
            uint64_t got = 0;
            {
                const std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                // Re-check the budget under the lock: the remote server can
                // step the CPU between the loop condition and here.
                const uint64_t now = emu.cpu().getTotalCycles();
                if (now >= budgetCycles) break;
                // Worst-case instruction is 8 cycles; single-step near the
                // deadline so we overshoot by at most one instruction.
                const uint64_t remaining = budgetCycles - now;
                got = dbg.runInstructions(remaining < 40000 ? 1 : 5000);
            }
            if (got == 0 || emu.cpu().halted()) break;
            executed += got;
        }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();

    if (!screenshotFile.empty()) {
        std::vector<uint8_t> rgb(64ull * 64ull * 3);
        // Local 256-entry ARGB palette (no GUI Display dependency needed).
        std::vector<uint32_t> palette(256, 0xFF000000u);
        for (int p = 0; p < 256; ++p) {
            const uint8_t v = static_cast<uint8_t>(((p >> 2) & 7) * 36);
            palette[static_cast<std::size_t>(p)] = 0xFF000000u | (static_cast<uint32_t>(v) << 16) |
                                                   (static_cast<uint32_t>(v) << 8) | v;
        }
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const uint8_t pixel = emu.memory().read(static_cast<uint16_t>(0x0200 + y * 64 + x));
                const uint32_t argb = palette[pixel];
                const std::size_t o = (static_cast<std::size_t>(y) * 64 + x) * 3;
                rgb[o] = static_cast<uint8_t>(argb & 0xFF);
                rgb[o + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
                rgb[o + 2] = static_cast<uint8_t>((argb >> 16) & 0xFF);
            }
        std::string err;
        if (!writeBmp24(screenshotFile, 64, 64, rgb, err)) {
            hprint("{\"ok\":false,\"error\":\"screenshot: %s\"}\n", jsonEscape(err).c_str());
            return 2;
        }
        hprint("screenshot: %s (%s)\n", screenshotFile.c_str(), err.c_str());
    }

    if (!wavFile.empty()) {
        const std::size_t samples = emu.apu().recordedSamples();
        emu.apu().stopWavRecord();
        hprint("wav: %s (%zu samples)\n", wavFile.c_str(), samples);
    }

    // Post-run exports.
    std::string expErr;
    if (!mapFile.empty()) {
        Assembler::writeMapFile(emu.program(), mapFile, expErr);
        hprint("map: %s\n", expErr.c_str());
    }
    if (!xrefFile.empty()) {
        Assembler::writeXrefFile(emu.program(), xrefFile, expErr);
        hprint("xref: %s\n", expErr.c_str());
    }
    if (!prgFile.empty()) {
        Assembler::exportPrg(emu.program(), prgFile, expErr);
        hprint("prg: %s\n", expErr.c_str());
    }
    if (!nesFile.empty()) {
        Assembler::exportNes(emu.program(), nesFile, expErr);
        hprint("nes: %s\n", expErr.c_str());
    }
    if (!mesenTraceFile.empty()) {
        dbg.exportTraceMesen(mesenTraceFile, expErr);
        hprint("mesen-trace: %s\n", expErr.c_str());
    }

    const CPU &c = emu.cpu();
    const uint32_t covLow = imageMode ? imageCovLow
                                      : (emu.program().lowestAddress == 0xFFFF ? 0x8000
                                                                              : emu.program().lowestAddress);
    const uint32_t covHigh = imageMode ? imageCovHigh : emu.program().highestAddress;
    const double coveragePct =
        dbg.coveragePercent(static_cast<uint16_t>(covLow), static_cast<uint16_t>(covHigh));
    hprint("{\"ok\":true,\"instr\":%llu,\"cycles\":%llu,\"halted\":%s,\"pc\":%u,"
           "\"coverage\":%.2f,\"ms\":%lld}\n",
           static_cast<unsigned long long>(executed), static_cast<unsigned long long>(c.getTotalCycles()),
           c.halted() ? "true" : "false", c.PC, coveragePct, static_cast<long long>(elapsedMs));

    return c.halted() ? 3 : 0; // remoteGuard stops the server
}
