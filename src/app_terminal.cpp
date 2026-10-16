#include "app.hpp"
#include "apu.hpp"
#include "assembler.hpp"
#include "cpu.hpp"
#include "feature_catalog.hpp"
#include "hexfile.hpp"
#include "ines.hpp"
#include "platform.hpp"
#include "remote.hpp"
#include "savestate.hpp"
#include "selftest_roms.hpp"
#include "settings.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

std::string trimCmd(const std::string &s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

std::string lowerCmd(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseAddrArg(const std::string &text, uint16_t &value) {
    return Assembler::parseValue(text, value, g_app.emu.program().symbols);
}

std::vector<std::string> terminalHistory;

const char *kHelpText = "commands:\r\n"
                        "  burn | run | pause | step | reset | regs [REG VAL]\r\n"
                        "  set FLAG 0|1            flags: n v d i z c\r\n"
                        "  t N | tc CYCLES | p | until ADDR | skip | ret\r\n"
                        "  so (step-over JSR) | sr (step-out) | rto ADDR | stack\r\n"
                        "  mem ADDR [LEN] | poke ADDR BYTE | fill A LEN B | copy S D N\r\n"
                        "  disasm ADDR [N] (alias u) | hex ADDR (alias d)\r\n"
                        "  break ADDR [COND] | bc ADDR | blist | watch A [B] | wclear\r\n"
                        "  back | forward | history | trace on|off|clear|export F|mesen F\r\n"
                        "  tracefilter EXPR|off     e.g. tracefilter a==0\r\n"
                        "  nmi | irq 0|1 | core 6502|65c02\r\n"
                        "  profile on|off|clear|top | coverage | snapshot | diff\r\n"
                        "  savebin FILE $A $B | loadbin FILE ADDR | crc ADDR LEN\r\n"
                        "  symbols export FILE.json | listing FILE.txt | map F | xref F\r\n"
                        "  features [search TEXT] | export FILE.txt|.json\r\n"
                        "  exportprg FILE.prg | exportnes FILE.nes\r\n"
                        "  gif FILE.gif | gif stop | input rec|play|loop|stop FILE\r\n"
                        "  screenshot [FILE] | speed N | turbo | frame\r\n"
                        "  klaus | nestest | core 6502|65c02 | nmi | irq 0|1\r\n"
                        "  theme dark|light|contrast | font SIZE | selftest | clear\r\n";

void cmdRegs(std::istringstream &in) {
    std::string which;
    in >> which;
    which = lowerCmd(which);
    CPU &cpu = g_app.emu.cpu();
    static const struct RegDef {
        const char *name;
        int bits;
    } defs[] = {{"a", 8}, {"x", 8}, {"y", 8}, {"pc", 16}, {"sp", 8}, {"p", 8}};
    if (which.empty()) {
        termInfo(g_app.dbg.statusText() + "\r\n");
        return;
    }
    for (const RegDef &def : defs) {
        if (which != def.name) continue;
        std::string valText;
        if (!(in >> valText)) {
            termError("usage: regs REG VALUE\r\n");
            return;
        }
        uint16_t v = 0;
        if (!parseAddrArg(valText, v)) {
            termError("bad value\r\n");
            return;
        }
        if (def.bits == 16)
            cpu.PC = v;
        else if (which == "a")
            cpu.A = static_cast<uint8_t>(v);
        else if (which == "x")
            cpu.X = static_cast<uint8_t>(v);
        else if (which == "y")
            cpu.Y = static_cast<uint8_t>(v);
        else if (which == "sp")
            cpu.SP = static_cast<uint8_t>(v);
        else
            cpu.P = static_cast<uint8_t>(v);
        termOk("ok\r\n");
        return;
    }
    termError("unknown register\r\n");
}

void cmdSetFlag(std::istringstream &in) {
    std::string flag, valText;
    in >> flag >> valText;
    flag = lowerCmd(flag);
    CPU &cpu = g_app.emu.cpu();
    uint8_t bit = 0;
    switch (!flag.empty() ? flag[0] : ' ') {
    case 'n':
        bit = FLAG_N;
        break;
    case 'v':
        bit = FLAG_V;
        break;
    case 'd':
        bit = FLAG_D;
        break;
    case 'i':
        bit = FLAG_I;
        break;
    case 'z':
        bit = FLAG_Z;
        break;
    case 'c':
        bit = FLAG_C;
        break;
    default:
        termError("flags: n v d i z c\r\n");
        return;
    }
    if (valText == "1")
        cpu.P |= bit;
    else if (valText == "0")
        cpu.P &= static_cast<uint8_t>(~bit & 0xFF);
    else {
        termError("usage: set FLAG 0|1\r\n");
        return;
    }
    termOk("ok\r\n");
}

} // namespace

void execTerminalCommand(std::string raw) {
    std::string cmd = trimCmd(raw);
    if (cmd.empty()) return;
    if (cmd == "!!" && !terminalHistory.empty()) cmd = terminalHistory.back();
    g_app.term.append(TermView::Role::Echo, "> " + cmd + "\r\n");
    terminalHistory.push_back(cmd);
    if (terminalHistory.size() > 200) terminalHistory.erase(terminalHistory.begin());

    std::istringstream in(cmd);
    std::string op;
    in >> op;
    op = lowerCmd(op);

    try {
        if (op == "help" || op == "?") {
            termInfo(kHelpText);
        } else if (op == "burn") {
            extern void appBurn(bool andRun);
            appBurn(false);
        } else if (op == "run") {
            extern void appStart();
            appStart();
        } else if (op == "pause" || op == "stop") {
            extern void appPause();
            appPause();
        } else if (op == "step") {
            extern void appStepInstruction();
            appStepInstruction();
        } else if (op == "reset") {
            extern void appReset();
            appReset();
        } else if (op == "regs") {
            cmdRegs(in);
        } else if (op == "set") {
            cmdSetFlag(in);
        } else if (op == "t" || op == "stepn") {
            std::string countText;
            in >> countText;
            const int count = std::clamp(countText.empty() ? 1 : std::stoi(countText), 1, 100000);
            for (int i = 0; i < count; ++i)
                if (g_app.dbg.step() == 0) break;
        } else if (op == "tc") {
            std::string cycText;
            in >> cycText;
            const int budget = std::clamp(cycText.empty() ? 1 : std::stoi(cycText), 1, 1000000);
            int spent = 0;
            while (spent < budget) {
                const int before = g_app.emu.cpu().getCycles();
                if (g_app.dbg.step() == 0) break;
                spent += g_app.emu.cpu().getCycles() - before;
            }
            termInfo("spent ~" + std::to_string(spent) + " cycles\r\n");
        } else if (op == "until") {
            std::string addrText;
            in >> addrText;
            uint16_t addr = 0;
            if (parseAddrArg(addrText, addr)) {
                // One-shot breakpoint like run-to-cursor (main.cpp's
                // lastStopWasBreakpoint gate never fires without an installed
                // breakpoint); bp-vector mutation under emulatorMutex() like
                // every other run/step path (remote.cpp iterates it locked).
                std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                bool userBpThere = false;
                for (const auto &bp : g_app.dbg.breakpoints())
                    if (bp.address == addr) {
                        userBpThere = true;
                        break;
                    }
                g_app.runToAddress = addr;
                g_app.runToBpOurs = !userBpThere;
                if (!userBpThere) g_app.dbg.addBreakpoint(addr);
                g_app.emu.start();
            } else
                termError("bad address\r\n");
        } else if (op == "skip") {
            CPU &cpu = g_app.emu.cpu();
            const uint8_t opcode = cpu.lastOpcode();
            cpu.PC = static_cast<uint16_t>(cpu.PC + CPU::instructionLength(opcode));
            termOk("skipped\r\n");
        } else if (op == "mem" || op == "dump") {
            std::string a, l;
            in >> a >> l;
            uint16_t addr = g_app.emu.cpu().PC;
            if (!a.empty() && !parseAddrArg(a, addr)) {
                termError("bad address\r\n");
                return;
            }
            const int len = std::clamp(l.empty() ? 64 : std::stoi(l), 1, 512);
            std::ostringstream o;
            o << std::hex << std::uppercase << std::setfill('0');
            for (int i = 0; i < len; ++i) {
                if (i % 16 == 0)
                    o << "\r\n" << std::setw(4) << static_cast<unsigned>((addr + i) & 0xFFFF) << "  ";
                o << std::setw(2)
                  << static_cast<unsigned>(
                         g_app.emu.memory().read(static_cast<uint16_t>((addr + i) & 0xFFFF)))
                  << ' ';
            }
            o << "\r\n";
            termInfo(o.str());
        } else if (op == "poke") {
            std::string a, v;
            in >> a >> v;
            uint16_t addr = 0, byte = 0;
            if (parseAddrArg(a, addr) && parseAddrArg(v, byte) && byte <= 0xFF) {
                g_app.emu.memory().write(addr, static_cast<uint8_t>(byte));
                termOk("ok\r\n");
            } else
                termError("usage: poke $ADDR BYTE\r\n");
        } else if (op == "fill") {
            std::string a, n, v;
            in >> a >> n >> v;
            uint16_t addr = 0, byte = 0;
            if (parseAddrArg(a, addr) && parseAddrArg(v, byte)) {
                const std::size_t len = static_cast<std::size_t>(std::strtoull(n.c_str(), nullptr, 0));
                g_app.emu.memory().fill(addr, len, static_cast<uint8_t>(byte));
                termOk("filled\r\n");
            } else
                termError("bad args\r\n");
        } else if (op == "disasm" || op == "u") {
            std::string a, n;
            in >> a >> n;
            uint16_t addr = g_app.emu.cpu().PC;
            if (!a.empty() && !parseAddrArg(a, addr)) addr = g_app.emu.cpu().PC;
            const int count = std::clamp(n.empty() ? 12 : std::stoi(n), 1, 128);
            termInfo(g_app.emu.cpu().disassembleText(addr, count) + "\r\n");
            g_app.disasm.centerOn(addr);
        } else if (op == "hex" || op == "d") {
            std::string a;
            in >> a;
            uint16_t addr = 0;
            if (parseAddrArg(a, addr)) g_app.hex.jumpTo(addr);
        } else if (op == "break") {
            std::string a;
            in >> a;
            uint16_t addr = 0;
            std::string cond;
            std::getline(in, cond);
            cond.erase(cond.begin(), std::find_if(cond.begin(), cond.end(),
                                                  [](unsigned char ch) { return !std::isspace(ch); }));
            if (parseAddrArg(a, addr) && g_app.dbg.addBreakpoint(addr, cond))
                termOk("breakpoint set\r\n");
            else
                termError("invalid breakpoint\r\n");
        } else if (op == "bc") {
            std::string a;
            in >> a;
            uint16_t addr = 0;
            if (parseAddrArg(a, addr) && g_app.dbg.removeBreakpoint(addr))
                termOk("removed\r\n");
            else
                termError("not found\r\n");
        } else if (op == "blist") {
            std::ostringstream o;
            for (const auto &b : g_app.dbg.breakpoints())
                o << std::hex << std::uppercase << "$" << b.address << (b.enabled ? " enabled" : " disabled")
                  << (b.condition.empty() ? "" : " if " + b.condition) << "\r\n";
            termInfo(o.str().empty() ? "no breakpoints\r\n" : o.str());
        } else if (op == "watch") {
            std::string a, b;
            in >> a >> b;
            uint16_t aa = 0, bb = 0;
            if (parseAddrArg(a, aa)) {
                bb = b.empty() || !parseAddrArg(b, bb) ? aa : bb;
                termOk(g_app.dbg.addWatchpoint(aa, bb) ? "watchpoint set\r\n" : "too many\r\n");
            } else
                termError("bad range\r\n");
        } else if (op == "wclear") {
            g_app.dbg.clearWatchpoints();
            termOk("cleared\r\n");
        } else if (op == "back") {
            extern void appStepBack();
            appStepBack();
        } else if (op == "forward") {
            extern void appStepForward();
            appStepForward();
        } else if (op == "history") {
            std::ostringstream o;
            int shown = 0;
            for (auto it = g_app.dbg.trace().rbegin(); it != g_app.dbg.trace().rend() && shown < 20;
                 ++it, ++shown)
                o << it->sequence << " $" << std::hex << std::uppercase << it->pc << "  " << it->disassembly
                  << "  [" << std::dec << it->cycles << "c]\r\n";
            termInfo(o.str().empty() ? "no trace entries\r\n" : o.str());
        } else if (op == "trace") {
            std::string sub, arg;
            in >> sub;
            std::getline(in, arg);
            arg = trimCmd(arg);
            sub = lowerCmd(sub);
            if (sub == "on")
                g_app.dbg.setTraceEnabled(true);
            else if (sub == "off")
                g_app.dbg.setTraceEnabled(false);
            else if (sub == "clear")
                g_app.dbg.clearTrace();
            else if (sub == "export" && !arg.empty()) {
                std::string err;
                if (!g_app.dbg.exportTrace(arg, err)) err = "export failed: " + err;
                termInfo(err + "\r\n");
            } else if (sub == "mesen" && !arg.empty()) {
                std::string err;
                if (!g_app.dbg.exportTraceMesen(arg, err)) err = "export failed: " + err;
                termInfo(err + "\r\n");
            }
            termInfo(g_app.dbg.statusText());
        } else if (op == "profile") {
            std::string sub;
            in >> sub;
            sub = lowerCmd(sub);
            if (sub == "on")
                g_app.dbg.setProfilerEnabled(true);
            else if (sub == "off")
                g_app.dbg.setProfilerEnabled(false);
            else if (sub == "clear")
                g_app.dbg.clearProfiler();
            else if (sub == "top") {
                std::vector<std::pair<uint64_t, uint16_t>> rows;
                for (uint32_t a2 = 0; a2 < 65536; ++a2)
                    if (g_app.dbg.profiler(static_cast<uint16_t>(a2)).executions)
                        rows.push_back({g_app.dbg.profiler(static_cast<uint16_t>(a2)).executions,
                                        static_cast<uint16_t>(a2)});
                std::sort(rows.rbegin(), rows.rend());
                std::ostringstream o;
                for (std::size_t i = 0; i < std::min<std::size_t>(20, rows.size()); ++i)
                    o << "$" << std::hex << std::uppercase << rows[i].second << " " << std::dec
                      << rows[i].first << " exec, " << g_app.dbg.profiler(rows[i].second).cycles
                      << " cycles\r\n";
                termInfo(o.str());
            }
        } else if (op == "snapshot") {
            g_app.dbg.takeSnapshot();
            termOk("snapshot taken\r\n");
        } else if (op == "diff") {
            auto d = g_app.dbg.snapshotDiff();
            std::ostringstream o;
            o << d.size() << " changed bytes\r\n";
            for (std::size_t i = 0; i < std::min<std::size_t>(64, d.size()); ++i)
                o << "$" << std::hex << std::uppercase << d[i].address << " "
                  << static_cast<unsigned>(d[i].oldValue) << " -> " << static_cast<unsigned>(d[i].newValue)
                  << "\r\n";
            termInfo(o.str());
        } else if (op == "savebin") {
            std::string f, a, b;
            in >> f >> a >> b;
            uint16_t start = 0x8000, end = 0xFFFF;
            if (!a.empty()) parseAddrArg(a, start);
            if (!b.empty()) parseAddrArg(b, end);
            std::string m;
            if (g_app.emu.saveBinaryFile(f, start, end, m))
                termInfo(m + "\r\n");
            else
                termError(m + "\r\n");
        } else if (op == "loadbin") {
            std::string f, a;
            in >> f >> a;
            uint16_t addr = 0x8000;
            if (!a.empty()) parseAddrArg(a, addr);
            std::string m;
            if (g_app.emu.loadBinaryFile(f, addr, m))
                termInfo(m + "\r\n");
            else
                termError(m + "\r\n");
        } else if (op == "crc") {
            std::string a, n;
            in >> a >> n;
            uint16_t addr = 0;
            if (!parseAddrArg(a, addr)) {
                termError("bad address\r\n");
            } else {
                const std::size_t len = static_cast<std::size_t>(std::strtoull(n.c_str(), nullptr, 0));
                std::ostringstream o;
                o << "CRC32 $" << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
                  << g_app.emu.memory().crc32(addr, len) << "\r\n";
                termInfo(o.str());
            }
        } else if (op == "screenshot") {
            std::string f;
            in >> f;
            extern void appScreenshot(const std::string &);
            appScreenshot(f);
        } else if (op == "speed") {
            std::string v;
            in >> v;
            Settings::get().speed = static_cast<uint32_t>(
                std::clamp<long long>(std::strtoll(v.c_str(), nullptr, 10), 100, 50000000));
            Settings::save();
            termOk("speed set\r\n");
        } else if (op == "turbo") {
            Settings::get().speed = 50000000;
            Settings::save();
            termOk("turbo\r\n");
        } else if (op == "dbghz") {
            std::string v;
            in >> v;
            int hz = std::clamp(std::atoi(v.c_str()), 1, 60);
            Settings::get().debugConsoleHz = hz;
            Settings::save();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "debug console %d Hz\r\n", hz);
            termOk(buf);
        } else if (op == "frame") {
            uint64_t executed = 0;
            while (executed < 30000 && g_app.emu.running()) {
                const uint64_t got = g_app.dbg.runInstructions(200);
                if (got == 0) break;
                executed += got;
            }
            termInfo("ran " + std::to_string(executed) + " instr\r\n");
        } else if (op == "apu") {
            std::string sub, arg;
            in >> sub >> arg;
            sub = lowerCmd(sub);
            if (sub == "on") {
                g_app.emu.apu().init();
                termOk("audio on\r\n");
            } else if (sub == "off") {
                g_app.emu.apu().setEnabled(false);
                termOk("audio off\r\n");
            } else if (sub == "vol") {
                const float vol = static_cast<float>(std::atof(arg.c_str())) / 100.0f;
                g_app.emu.apu().setVolume(vol);
                termOk("volume set\r\n");
            } else if (sub == "record") {
                g_app.emu.apu().startWavRecord(arg.empty() ? platform::settingsDirectory() + "/apu_out.wav"
                                                           : arg);
                termOk("recording started\r\n");
            } else if (sub == "stop") {
                const std::size_t samples = g_app.emu.apu().recordedSamples();
                g_app.emu.apu().stopWavRecord();
                termOk("stopped, " + std::to_string(samples) + " samples\r\n");
            } else {
                char buf[128];
                const Apu &apu = g_app.emu.apu();
                std::snprintf(buf, sizeof(buf), "enabled=%d recording=%d vol=%.0f%%\r\n", apu.enabled(),
                              apu.recording(), apu.volume() * 100.0f);
                termInfo(buf);
            }
        } else if (op == "nesload") {
            std::string f;
            in >> f;
            ines::NesRom rom;
            std::string err;
            if (ines::loadFile(f, rom, err) && ines::mapInto(g_app.emu, rom, err)) {
                g_app.dbg.clear();
                termOk(err + "\r\n");
                g_app.disasm.jumpToPc();
            } else {
                termError(err + "\r\n");
            }
        } else if (op == "hexload") {
            std::string f;
            in >> f;
            std::string err;
            termOk(hexfile::loadIntelHex(f, g_app.emu.memory(), err) ? err + "\r\n" : err + "\r\n");
        } else if (op == "hexsave" || op == "srecsave") {
            std::string f, a, b;
            in >> f >> a >> b;
            uint16_t start = 0x8000, end = 0xFFFF;
            if (!a.empty()) parseAddrArg(a, start);
            if (!b.empty()) parseAddrArg(b, end);
            std::string err;
            const bool ok = op == "hexsave"
                                ? hexfile::saveIntelHex(g_app.emu.memory(), start,
                                                        static_cast<std::size_t>(end - start + 1), f, err)
                                : hexfile::saveSrec(g_app.emu.memory(), start,
                                                    static_cast<std::size_t>(end - start + 1), f, err);
            termInfo((ok ? "" : "error: ") + err + "\r\n");
        } else if (op == "state") {
            std::string sub, slotText;
            in >> sub >> slotText;
            sub = lowerCmd(sub);
            const int slot = std::clamp(slotText.empty() ? 0 : std::atoi(slotText.c_str()), 0, 9);
            std::string err;
            if (sub == "save")
                savestate::saveToFile(g_app.emu, savestate::slotPath(slot), err);
            else if (sub == "load")
                savestate::loadFromFile(g_app.emu, savestate::slotPath(slot), err);
            else {
                termError("usage: state save|load [slot]\r\n");
                return;
            }
            termInfo(err + "\r\n");
        } else if (op == "wa") {
            std::string expr;
            std::getline(in, expr);
            expr = trimCmd(expr);
            termOk(g_app.dbg.addWatchExpression(expr) ? "watch added\r\n" : "too many watches\r\n");
        } else if (op == "wd") {
            std::string nText;
            in >> nText;
            g_app.dbg.removeWatchExpression(static_cast<std::size_t>(std::atoi(nText.c_str())));
            termOk("removed\r\n");
        } else if (op == "wlist") {
            const auto &watches = g_app.dbg.watchExpressions();
            for (std::size_t i = 0; i < watches.size(); ++i) {
                uint64_t v = 0;
                std::string err;
                char buf[160];
                if (g_app.dbg.evaluateExpression(watches[i], v, err))
                    std::snprintf(buf, sizeof(buf), "[%zu] %s = $%llX\r\n", i, watches[i].c_str(),
                                  static_cast<unsigned long long>(v));
                else
                    std::snprintf(buf, sizeof(buf), "[%zu] %s = ? (%s)\r\n", i, watches[i].c_str(),
                                  err.c_str());
                termInfo(buf);
            }
            if (watches.empty()) termInfo("no watch expressions\r\n");
        } else if (op == "coverage") {
            std::string sub, f;
            in >> sub >> f;
            const auto &prog = g_app.emu.program();
            const uint16_t low = prog.lowestAddress == 0xFFFF ? uint16_t{0x8000} : prog.lowestAddress;
            const double pct = g_app.dbg.coveragePercent(low, prog.highestAddress);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "coverage $%04X-$%04X: %.2f%%\r\n", low, prog.highestAddress,
                          pct);
            termInfo(buf);
            if (sub == "export" && !f.empty()) {
                std::string err;
                g_app.dbg.exportCoverageCsv(f, err);
                termInfo(err + "\r\n");
            }
        } else if (op == "benchmark") {
            std::string nText;
            in >> nText;
            const uint64_t target =
                std::clamp<uint64_t>(std::strtoull(nText.c_str(), nullptr, 0), 100000, 500000000ull);
            g_app.dbg.setProfilerEnabled(false);
            g_app.emu.start();
            const auto begin = std::chrono::steady_clock::now();
            uint64_t executed = 0;
            while (executed < target && g_app.emu.running()) {
                executed += g_app.dbg.runInstructions(20000);
            }
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.2f MIPS (%llu instr / %.3fs)\r\n",
                          seconds > 0 ? static_cast<double>(executed) / seconds / 1e6 : 0.0,
                          static_cast<unsigned long long>(executed), seconds);
            termOk(buf);
        } else if (op == "logbp") {
            termError("use: break ADDR then 'bplog ADDR MSG'\r\n");
        } else if (op == "bplog") {
            std::string a, msg;
            in >> a;
            std::getline(in, msg);
            msg = trimCmd(msg);
            uint16_t addr = 0;
            const bool ok = parseAddrArg(a, addr) && g_app.dbg.setBreakpointLog(addr, msg);
            if (ok) g_app.dbg.setLogSink([](const std::string &s) { termInfo(s + "\r\n"); });
            termOk(ok ? "logging breakpoint\r\n" : "breakpoint not found\r\n");
        } else if (op == "ignore") {
            std::string a, n;
            in >> a >> n;
            uint16_t addr = 0;
            const int count = std::atoi(n.c_str());
            const bool ok = parseAddrArg(a, addr) && g_app.dbg.setBreakpointIgnore(addr, count);
            termOk(ok ? "will ignore next " + std::to_string(count) + " hits\r\n"
                      : "breakpoint not found\r\n");
        } else if (op == "remote") {
            std::string sub, portText;
            in >> sub >> portText;
            sub = lowerCmd(sub);
            if (sub == "on" || sub == "start") {
                const int port = portText.empty() ? 6502 : std::atoi(portText.c_str());
                Settings::get().remotePort = port;
                Settings::save();
                std::string err;
                RemoteServer::instance().start(g_app.emu, g_app.dbg, port, err);
                termInfo(err + "\r\n");
            } else {
                Settings::get().remotePort = 0;
                Settings::save();
                RemoteServer::instance().stop();
                termOk("remote stopped\r\n");
            }
        } else if (op == "remap") {
            std::string key, vkText;
            in >> key >> vkText;
            AppSettings &s = Settings::get();
            int *target = nullptr;
            key = lowerCmd(key);
            if (key == "up")
                target = &s.vkUp;
            else if (key == "down")
                target = &s.vkDown;
            else if (key == "left")
                target = &s.vkLeft;
            else if (key == "right")
                target = &s.vkRight;
            else if (key == "a")
                target = &s.vkButtonA;
            else if (key == "b")
                target = &s.vkButtonB;
            else if (key == "start")
                target = &s.vkStart;
            else if (key == "select")
                target = &s.vkSelect;
            if (!target || vkText.empty()) {
                termError("usage: remap up|down|left|right|a|b|start|select VKCODE\r\n");
                return;
            }
            *target = std::atoi(vkText.c_str());
            Settings::save();
            termOk("remapped\r\n");
        } else if (op == "theme") {
            std::string v;
            in >> v;
            v = lowerCmd(v);
            const int idx = v == "light"      ? 1
                            : v == "contrast" ? 2
                            : v == "dark"     ? 0
                                              : Settings::get().themeIndex;
            Settings::get().themeIndex = idx;
            setThemeIndex(idx);
            extern void applyThemeToWindow(HWND root);
            applyThemeToWindow(g_app.mainWnd);
            termOk("theme applied\r\n");
        } else if (op == "font") {
            std::string v;
            in >> v;
            const int size = std::clamp(std::stoi(v), 9, 28);
            Settings::get().editorFontSize = size;
            Settings::save();
            extern void rebuildFontsAndInvalidate();
            rebuildFontsAndInvalidate();
            termOk("font size applied\r\n");
        } else if (op == "symbols") {
            std::string sub, f;
            in >> sub >> f;
            if (sub == "export" && !f.empty()) {
                std::string err;
                Assembler::exportSymbolsJson(g_app.emu.program(), f, err);
                termInfo(err + "\r\n");
            } else {
                std::ostringstream o;
                for (const auto &kv : g_app.emu.program().symbols)
                    o << kv.first << " = $" << std::hex << std::uppercase << kv.second << "\r\n";
                termInfo(o.str().empty() ? "no symbols\r\n" : o.str());
            }
        } else if (op == "listing") {
            std::string f;
            in >> f;
            if (!f.empty()) {
                std::string err;
                Assembler::writeListingFile(g_app.emu.program(), f, err);
                termInfo(err + "\r\n");
            } else
                termError("usage: listing FILE.txt\r\n");
        } else if (op == "features") {
            std::string sub, arg;
            in >> sub;
            std::getline(in, arg);
            arg = trimCmd(arg);
            if (sub == "export" && !arg.empty()) {
                std::string err;
                if (arg.size() > 5 && arg.substr(arg.size() - 5) == ".json")
                    FeatureCatalog::exportJson(arg, err);
                else
                    FeatureCatalog::exportText(arg, err);
                termInfo(err + "\r\n");
            } else {
                auto rows = FeatureCatalog::search(sub == "search" ? arg : std::string{});
                std::ostringstream o;
                std::size_t shown = 0;
                for (const auto &e : rows) {
                    o << "[" << FeatureCatalog::statusName(e.status) << "] " << e.name << "\r\n";
                    if (++shown >= 80) break;
                }
                o << "-- " << std::min<size_t>(rows.size(), 80) << " of " << rows.size() << " --\r\n";
                termInfo(o.str());
            }
        } else if (op == "selftest") {
            extern bool runSelfTests();
            runSelfTests();
        } else if (op == "klaus") {
            std::string f;
            in >> f;
            const std::string bin = f.empty() ? findTestDataFile("6502_functional_test.bin") : f;
            if (bin.empty()) {
                termError("6502_functional_test.bin not found (tests\\data)\r\n");
            } else {
                extern bool runKlausInEmulator(const std::string &path, std::string &report);
                std::string report;
                const bool ok = runKlausInEmulator(bin, report);
                (ok ? termOk : termError)(report + "\r\n");
            }
        } else if (op == "nestest") {
            std::string romf, logf;
            in >> romf >> logf;
            const std::string rom = romf.empty() ? findTestDataFile("nestest.nes") : romf;
            const std::string log = logf.empty() ? findTestDataFile("nestest.log") : logf;
            if (rom.empty() || log.empty()) {
                termError("nestest.nes / nestest.log not found (tests\\data)\r\n");
            } else {
                extern bool runNestestInEmulator(const std::string &rom, const std::string &log,
                                                 std::string &report);
                std::string report;
                const bool ok = runNestestInEmulator(rom, log, report);
                (ok ? termOk : termError)(report + "\r\n");
            }
        } else if (op == "tracefilter") {
            std::string expr;
            std::getline(in, expr);
            expr = trimCmd(expr);
            if (expr.empty() || lowerCmd(expr) == "off") {
                g_app.dbg.setTraceFilter({});
                termOk("trace filter cleared\r\n");
            } else {
                g_app.dbg.setTraceFilter(expr);
                termOk("trace filter: " + expr + "\r\n");
            }
        } else if (op == "so" || op == "stepover") {
            {
                std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                const uint64_t n = g_app.dbg.stepOver();
                char b[64];
                std::snprintf(b, sizeof(b), "[OVER] %" PRIu64 " instructions\r\n", n);
                termInfo(b);
            }
        } else if (op == "sr" || op == "stepout") {
            {
                std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                const uint64_t n = g_app.dbg.stepOut();
                char b[64];
                std::snprintf(b, sizeof(b), "[OUT] %" PRIu64 " instructions\r\n", n);
                termInfo(b);
            }
        } else if (op == "rto" || op == "runto") {
            std::string a;
            in >> a;
            uint16_t addr = 0;
            if (!parseAddrArg(a, addr)) {
                termError("usage: rto $ADDR\r\n");
            } else {
                std::lock_guard<std::mutex> guard(remote::emulatorMutex());
                const uint64_t n = g_app.dbg.runTo(addr);
                char b[96];
                std::snprintf(b, sizeof(b), "[RUNTO] %" PRIu64 " instructions\r\n", n);
                termInfo(b);
            }
        } else if (op == "stack") {
            auto frames = g_app.dbg.callStack();
            std::ostringstream o;
            o << frames.size() << " call frame(s)\r\n";
            for (std::size_t i = frames.size(); i-- > 0;) {
                const auto &f = frames[i];
                o << "  sp $" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << f.sp
                  << "  jsr $" << std::setw(4) << f.callerInstruction << "  ret $" << std::setw(4)
                  << f.returnAddress << "\r\n";
            }
            termInfo(o.str());
        } else if (op == "nmi") {
            g_app.emu.requestNmi();
            termOk("NMI asserted\r\n");
        } else if (op == "irq") {
            std::string v;
            in >> v;
            const bool on = v.empty() || v == "1" || lowerCmd(v) == "on";
            g_app.emu.requestIrq(on);
            termOk(std::string("IRQ line ") + (on ? "asserted\r\n" : "released\r\n"));
        } else if (op == "core") {
            std::string v;
            in >> v;
            v = lowerCmd(v);
            if (v == "65c02" || v == "c02") {
                g_app.emu.setCoreVariant(CPU::Core::WDC65C02);
                Settings::get().cpuCore = 1;
                Settings::save();
                termOk("CPU core: WDC 65C02\r\n");
            } else if (v == "6502") {
                g_app.emu.setCoreVariant(CPU::Core::NMOS6502);
                Settings::get().cpuCore = 0;
                Settings::save();
                termOk("CPU core: NMOS 6502\r\n");
            } else {
                termInfo(std::string("current core: ") +
                         (g_app.emu.coreVariant() == CPU::Core::WDC65C02 ? "WDC 65C02" : "NMOS 6502") +
                         "  (use: core 6502|65c02)\r\n");
            }
        } else if (op == "gif") {
            std::string arg;
            std::getline(in, arg);
            arg = trimCmd(arg);
            if (arg.empty() || lowerCmd(arg) == "stop") {
                std::string err;
                if (!g_app.gif.isOpen())
                    termError("no GIF capture active\r\n");
                else {
                    const unsigned n = g_app.gif.frames();
                    g_app.gif.close(err);
                    char b[128];
                    std::snprintf(b, sizeof(b), "GIF saved (%u frames)%s\r\n", n,
                                  err.empty() ? "" : err.c_str());
                    termOk(b);
                }
            } else {
                std::string err;
                if (g_app.gif.isOpen()) {
                    termError("capture already running (gif stop first)\r\n");
                } else if (g_app.gif.open(arg, Settings::get().fbSize, Settings::get().fbSize, 4, err))
                    termOk("GIF capture started: " + arg + "\r\n");
                else
                    termError(err + "\r\n");
            }
        } else if (op == "input") {
            std::string sub, file;
            in >> sub >> file;
            sub = lowerCmd(sub);
            std::string err;
            if (sub == "rec") {
                if (file.empty())
                    termError("usage: input rec FILE.inp\r\n");
                else if (g_app.input.startRecord(file, err))
                    termOk("input recording to " + file + "\r\n");
                else
                    termError(err + "\r\n");
            } else if (sub == "play") {
                if (file.empty())
                    termError("usage: input play FILE.inp\r\n");
                else if (g_app.input.startReplay(file, false, err))
                    termOk("input replay (once)\r\n");
                else
                    termError(err + "\r\n");
            } else if (sub == "loop") {
                if (file.empty())
                    termError("usage: input loop FILE.inp\r\n");
                else if (g_app.input.startReplay(file, true, err))
                    termOk("input replay (looping)\r\n");
                else
                    termError(err + "\r\n");
            } else if (sub == "stop") {
                g_app.input.stop();
                termOk("input stopped\r\n");
            } else
                termError("usage: input rec|play|loop|stop [FILE]\r\n");
        } else if (op == "map") {
            std::string f;
            in >> f;
            if (f.empty())
                termError("usage: map FILE.map\r\n");
            else {
                std::string err;
                Assembler::writeMapFile(g_app.emu.program(), f, err);
                termInfo(err + "\r\n");
            }
        } else if (op == "xref") {
            std::string f;
            in >> f;
            if (f.empty())
                termError("usage: xref FILE.txt\r\n");
            else {
                std::string err;
                Assembler::writeXrefFile(g_app.emu.program(), f, err);
                termInfo(err + "\r\n");
            }
        } else if (op == "exportprg" || op == "exportnes") {
            std::string f;
            in >> f;
            if (f.empty())
                termError("usage: " + op + " FILE\r\n");
            else {
                std::string err;
                const bool okPrg = op == "exportprg";
                const bool ok = okPrg ? Assembler::exportPrg(g_app.emu.program(), f, err)
                                      : Assembler::exportNes(g_app.emu.program(), f, err);
                termInfo((ok ? "wrote " : "failed: ") + err + "\r\n");
            }
        } else if (op == "clear" || op == "cls") {
            g_app.term.clear();
        } else {
            g_app.term.append(TermView::Role::Warn, "unknown command '" + op + "' - type help\r\n");
        }
    } catch (const std::exception &ex) {
        termError(std::string("error: ") + ex.what() + "\r\n");
    }
}

namespace terminalui {

bool historyNavigate(HWND input, int direction, std::string &bufferInOut) {
    static int position = 0;
    const int count = static_cast<int>(terminalHistory.size());
    if (count == 0) return false;
    position = std::clamp(position + direction, 0, count);
    char current[2048]{};
    GetWindowTextA(input, current, sizeof(current));
    if (position == count) {
        SetWindowTextA(input, bufferInOut.c_str());
        return true;
    }
    if (direction > 0 && position == count - direction && strcmp(current, "") != 0 &&
        std::string(current).size() > 0 && bufferInOut.empty())
        bufferInOut = current;
    SetWindowTextA(input, terminalHistory[static_cast<std::size_t>(position)].c_str());
    return true;
}

void resetHistoryPosition() {}

} // namespace terminalui
