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
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto value = [&]() -> const std::string & { return i + 1 < args.size() ? args[i + 1] : a; };
        if (a == "--headless") continue;
        if (a == "--asm" && i + 1 < args.size())
            asmFile = value(), ++i;
        else if (a == "--image" && i + 1 < args.size())
            imageFile = value(), ++i;
        else if (a == "--base" && i + 1 < args.size())
            base = static_cast<uint32_t>(std::strtoul(value().c_str(), nullptr, 0)), ++i;
        else if (a == "--instr" && i + 1 < args.size())
            budgetInstr = std::strtoull(value().c_str(), nullptr, 0), ++i;
        else if (a == "--cycles" && i + 1 < args.size())
            budgetCycles = std::strtoull(value().c_str(), nullptr, 0), ++i;
        else if (a == "--screenshot" && i + 1 < args.size())
            screenshotFile = value(), ++i;
        else if (a == "--waverec" && i + 1 < args.size())
            wavFile = value(), ++i;
        else if (a == "--remote" && i + 1 < args.size())
            remotePort = std::atoi(value().c_str()), ++i;
        else if (a == "--map" && i + 1 < args.size())
            mapFile = value(), ++i;
        else if (a == "--xref" && i + 1 < args.size())
            xrefFile = value(), ++i;
        else if (a == "--prg" && i + 1 < args.size())
            prgFile = value(), ++i;
        else if (a == "--nes" && i + 1 < args.size())
            nesFile = value(), ++i;
        else if (a == "--mesen-trace" && i + 1 < args.size())
            mesenTraceFile = value(), ++i;
        else if (a == "-D" && i + 1 < args.size()) {
            definesArg = definesArg.empty() ? value() : definesArg + "," + value();
            ++i;
        } else if (a == "--core" && i + 1 < args.size())
            coreArg = value(), ++i;
        else if (a == "--link" && i + 1 < args.size())
            extraSources = value(), ++i;
        else if (a == "--run")
            runProgram = true;
        else {
            hprint("{\"ok\":false,\"error\":\"unknown argument %s\"}\n", jsonEscape(a).c_str());
            return 2;
        }
    }

    if (runProgram && budgetInstr == 0 && budgetCycles == 0) {
        hprint("{\"ok\":false,\"error\":\"--run requires --instr or --cycles\"}\n");
        return 2;
    }

    Emulator emu;
    Debugger dbg(emu);
    dbg.setCoverageEnabled(true);
    emu.apu().init();
    if (!wavFile.empty()) emu.apu().startWavRecord(wavFile);

    RemoteServer &remoteSrv = RemoteServer::instance();
    if (remotePort > 0) {
        std::string err;
        if (!remoteSrv.start(emu, dbg, remotePort, err))
            hprint("remote: %s\n", err.c_str());
        else
            hprint("remote: %s\n", err.c_str());
    }

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
        if (imageFile.size() > 4 && iendsWith(imageFile, ".nes") && ines::loadFile(imageFile, rom, err)) {
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
        writeBmp24(screenshotFile, 64, 64, rgb, err);
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

    remoteSrv.stop();
    return c.halted() ? 3 : 0;
}
