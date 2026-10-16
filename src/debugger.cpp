#include "debugger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

Debugger::Debugger(Emulator &emulator) : emulator_(emulator) {}

void Debugger::clear() {
    history_.clear();
    historyPosition_ = 0;
    trace_.clear();
    clearProfiler();
    breakpoints_.clear();
    watchpoints_.clear();
    clearSnapshot();
}

void Debugger::setHistoryCapacity(std::size_t capacity) {
    historyCapacity_ = std::max<std::size_t>(1, capacity);
    while (history_.size() > historyCapacity_) history_.pop_front();
    historyPosition_ = 0;
}

CPUStateSnapshot Debugger::captureState() const {
    const CPU &c = emulator_.cpu();
    CPUStateSnapshot s;
    s.A = c.A;
    s.X = c.X;
    s.Y = c.Y;
    s.PC = c.PC;
    s.SP = c.SP;
    s.P = c.P;
    s.totalCycles = c.getTotalCycles();
    s.lastOpcode = c.lastOpcode();
    s.lastCycles = c.getCycles();
    s.halted = c.halted();
    return s;
}

void Debugger::restoreState(const CPUStateSnapshot &s) {
    CPU &c = emulator_.cpu();
    c.A = s.A;
    c.X = s.X;
    c.Y = s.Y;
    c.PC = s.PC;
    c.SP = s.SP;
    c.P = static_cast<uint8_t>(s.P | FLAG_U);
    c.restoreTiming(s.totalCycles, s.lastOpcode, s.lastCycles, s.halted);
}

bool Debugger::stepBack() {
    if (history_.empty() || historyPosition_ >= history_.size()) return false;
    const std::size_t index = history_.size() - 1 - historyPosition_;
    const DebugHistoryEntry &entry = history_[index];
    for (auto it = entry.writes.rbegin(); it != entry.writes.rend(); ++it)
        emulator_.memory().writeRaw(it->address, it->oldValue);
    restoreState(entry.before);
    ++historyPosition_;
    emulator_.pause();
    return true;
}

bool Debugger::stepForward() {
    if (historyPosition_ == 0) return false;
    const std::size_t index = history_.size() - historyPosition_;
    if (index >= history_.size()) return false;
    emulator_.cpu().clearHalt();
    emulator_.debugStepInternal();
    --historyPosition_;
    return true;
}

void Debugger::clearTrace() {
    trace_.clear();
    traceSequence_ = 0;
}

bool Debugger::exportTraceMesen(const std::string &path, std::string &error) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create trace file: " + path;
        return false;
    }
    const CPU &c = emulator_.cpu();
    for (const auto &e : trace_) {
        (void)c;
        out << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << e.pc << ':' << e.disassembly
            << std::setfill(' ');
        // disassembly already begins with "OP MNEM OPERAND"; pad to a fixed
        // column so register fields align like Mesen's tracer.
        const int pad = 46 - static_cast<int>(e.disassembly.size());
        if (pad > 0) out << std::string(static_cast<std::size_t>(pad), ' ');
        out << "A:" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(e.after.A)
            << " X:" << std::setw(2) << static_cast<unsigned>(e.after.X) << " Y:" << std::setw(2)
            << static_cast<unsigned>(e.after.Y) << " P:" << std::setw(2) << static_cast<unsigned>(e.after.P)
            << " SP:" << std::setw(2) << static_cast<unsigned>(e.after.SP) << std::dec
            << " CYC:" << e.before.totalCycles << '\n';
    }
    if (!out) {
        error = "Trace write failed";
        return false;
    }
    error = "Exported " + std::to_string(trace_.size()) + " Mesen-style trace entries";
    return true;
}

bool Debugger::exportTrace(const std::string &path, std::string &error) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not create trace file: " + path;
        return false;
    }
    out << "# 6502 execution trace\n"
        << "seq,pc,opcode,mnemonic,cycles,total_cycles,A,X,Y,SP,P,writes\n";
    for (const auto &e : trace_) {
        out << e.sequence << ',' << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << e.pc
            << ',' << std::setw(2) << static_cast<unsigned>(e.opcode) << ',' << e.disassembly << ','
            << std::dec << e.cycles << ',' << e.after.totalCycles << ',' << std::hex << std::setw(2)
            << static_cast<unsigned>(e.after.A) << ',' << std::setw(2) << static_cast<unsigned>(e.after.X)
            << ',' << std::setw(2) << static_cast<unsigned>(e.after.Y) << ',' << std::setw(2)
            << static_cast<unsigned>(e.after.SP) << ',' << std::setw(2) << static_cast<unsigned>(e.after.P)
            << ',';
        for (std::size_t i = 0; i < e.writes.size(); ++i) {
            if (i) out << ';';
            out << std::setw(4) << e.writes[i].address << ':' << std::setw(2)
                << static_cast<unsigned>(e.writes[i].newValue);
        }
        out << '\n';
    }
    if (!out) {
        error = "Trace write failed";
        return false;
    }
    error = "Exported " + std::to_string(trace_.size()) + " trace entries";
    return true;
}

void Debugger::clearProfiler() {
    profiler_.fill({});
}

bool Debugger::addBreakpoint(uint16_t address, const std::string &condition) {
    for (auto &bp : breakpoints_)
        if (bp.address == address) {
            bp.enabled = true;
            bp.condition = condition;
            return true;
        }
    if (breakpoints_.size() >= 256) return false;
    breakpoints_.push_back({address, true, condition, 0, 0, false, {}});
    return true;
}

bool Debugger::removeBreakpoint(uint16_t address) {
    const auto it = std::remove_if(breakpoints_.begin(), breakpoints_.end(),
                                   [address](const auto &bp) { return bp.address == address; });
    if (it == breakpoints_.end()) return false;
    breakpoints_.erase(it, breakpoints_.end());
    return true;
}

bool Debugger::setBreakpointEnabled(uint16_t address, bool enabled) {
    for (auto &bp : breakpoints_)
        if (bp.address == address) {
            bp.enabled = enabled;
            return true;
        }
    return false;
}

bool Debugger::breakpointHit(uint16_t pc) const {
    for (const auto &bp : breakpoints_)
        if (bp.enabled && bp.address == pc && (bp.condition.empty() || evaluateCondition(bp.condition)))
            return true;
    return false;
}

int Debugger::breakpointHitCount(uint16_t address) const {
    for (const auto &bp : breakpoints_)
        if (bp.address == address) return bp.hitCount;
    return 0;
}

bool Debugger::setBreakpointLog(uint16_t address, const std::string &message) {
    for (auto &bp : breakpoints_)
        if (bp.address == address) {
            bp.logMode = true;
            bp.logMessage = message;
            return true;
        }
    return false;
}

bool Debugger::setBreakpointIgnore(uint16_t address, int count) {
    for (auto &bp : breakpoints_)
        if (bp.address == address) {
            bp.ignoreLeft = count;
            return true;
        }
    return false;
}

bool Debugger::evaluateExpression(const std::string &expr, uint64_t &value, std::string &err) const {
    return evaluateExpressionDepth(expr, value, err, 0);
}

bool Debugger::evaluateExpressionDepth(const std::string &expr, uint64_t &value, std::string &err,
                                       int recDepth) const {
    // Depth cap for the recursive paren descent: each nested '(' costs one
    // stack frame, so unbounded nesting (e.g. 100k '(' in a condition) would
    // overflow the stack. 128 comfortably covers real expressions (100 nested
    // parens still evaluate) and keeps worst-case frames ~128 deep.
    if (recDepth > 128) {
        err = "expression nesting too deep";
        return false;
    }
    std::string e = expr;
    e.erase(std::remove_if(e.begin(), e.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
            e.end());
    const CPU &c = emulator_.cpu();
    struct Tok {
        uint64_t v;
        char op;
    };
    std::vector<Tok> toks;
    std::size_t i = 0;
    bool expectValue = true;
    while (i < e.size()) {
        if (expectValue) {
            if (e[i] == '(') {
                // Parenthesized sub-expression: evaluate it recursively and
                // push the result as a single value so "(A+2)" and
                // "(LOOP-1)" parse. Unbalanced parens are an error.
                int depth = 0;
                std::size_t j = i;
                for (; j < e.size(); ++j) {
                    if (e[j] == '(')
                        ++depth;
                    else if (e[j] == ')' && --depth == 0)
                        break;
                }
                if (depth != 0 || j >= e.size()) {
                    err = "unbalanced parentheses";
                    return false;
                }
                uint64_t v = 0;
                std::string subErr;
                if (!evaluateExpressionDepth(e.substr(i + 1, j - i - 1), v, subErr, recDepth + 1)) {
                    err = subErr;
                    return false;
                }
                toks.push_back({v, 0});
                i = j + 1;
                expectValue = false;
                continue;
            }
            const std::size_t start = i;
            while (i < e.size() && (std::isxdigit(static_cast<unsigned char>(e[i])) || e[i] == '$' ||
                                    e[i] == '%' || std::isalpha(static_cast<unsigned char>(e[i]))))
                ++i;
            std::string token = e.substr(start, i - start);
            if (token.empty()) {
                err = "bad token";
                return false;
            }
            uint64_t v = 0;
            const std::string u = upper(token);
            if (u == "A")
                v = c.A;
            else if (u == "X")
                v = c.X;
            else if (u == "Y")
                v = c.Y;
            else if (u == "PC")
                v = c.PC;
            else if (u == "SP")
                v = c.SP;
            else if (u == "P")
                v = c.P;
            else if (u == "CYCLES")
                v = c.getTotalCycles();
            else {
                uint16_t parsed = 0;
                if (!Assembler::parseValue(token, parsed, emulator_.program().symbols)) {
                    err = "unknown token: " + token;
                    return false;
                }
                v = parsed;
            }
            toks.push_back({v, 0});
            expectValue = false;
        } else {
            char op = 0;
            if (i + 1 < e.size() && (e[i] == '=' && e[i + 1] == '=')) {
                op = '=';
                i += 2;
            } else if (i + 1 < e.size() && e[i] == '!' && e[i + 1] == '=') {
                op = '!';
                i += 2;
            } else if (i + 1 < e.size() && e[i] == '>' && e[i + 1] == '=') {
                op = 'g';
                i += 2;
            } else if (i + 1 < e.size() && e[i] == '<' && e[i + 1] == '=') {
                op = 'l';
                i += 2;
            } else if (e[i] == '>' || e[i] == '<') {
                op = e[i];
                ++i;
            } else if (e[i] == '&' || e[i] == '|' || e[i] == '+' || e[i] == '-') {
                op = e[i];
                ++i;
            } else {
                err = "expected operator";
                return false;
            }
            if (toks.empty()) {
                err = "syntax";
                return false;
            }
            toks.back().op = op;
            expectValue = true;
        }
    }
    if (toks.empty() || expectValue) {
        err = "incomplete expression";
        return false;
    }
    value = toks.front().v;
    for (std::size_t k = 1; k < toks.size(); ++k) {
        const uint64_t rhs = toks[k].v;
        switch (toks[k - 1].op) {
        case '=':
            value = value == rhs;
            break;
        case '!':
            value = value != rhs;
            break;
        case '>':
            value = value > rhs;
            break;
        case '<':
            value = value < rhs;
            break;
        case 'g':
            value = value >= rhs;
            break;
        case 'l':
            value = value <= rhs;
            break;
        case '&':
            value &= rhs;
            break;
        case '|':
            value |= rhs;
            break;
        case '+':
            value += rhs;
            break;
        case '-':
            value -= rhs;
            break;
        default:
            err = "bad operator";
            return false;
        }
    }
    return true;
}

bool Debugger::addWatchExpression(const std::string &expr) {
    if (watchExpressions_.size() >= 8) return false;
    watchExpressions_.push_back(expr);
    return true;
}

bool Debugger::removeWatchExpression(std::size_t index) {
    if (index >= watchExpressions_.size()) return false;
    watchExpressions_.erase(watchExpressions_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

double Debugger::coveragePercent(uint16_t low, uint16_t high) const noexcept {
    if (high <= low) return 0.0;
    const double span = static_cast<double>(high - low) + 1.0;
    double hit = 0;
    for (uint32_t a = low; a <= high; ++a)
        if (coverage_[a]) ++hit;
    return hit * 100.0 / span;
}

bool Debugger::exportCoverageCsv(const std::string &path, std::string &error) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    out << "address,executed\n";
    for (uint32_t a = 0; a < 65536; ++a)
        if (coverage_[a]) out << a << ",1\n";
    error = "coverage exported";
    return true;
}

bool Debugger::addWatchpoint(uint16_t start, uint16_t end, bool onRead, bool onWrite) {
    if (end < start) std::swap(start, end);
    // The memory layer only traces WRITES; a read watchpoint would be
    // accepted and then never fire. Report that honestly instead (see
    // debugger.hpp addWatchpoint).
    if (onRead) {
        logErrorOnce("watchpoint rejected: read tracing unsupported (writes only)");
        return false;
    }
    if (!onWrite) {
        logErrorOnce("watchpoint rejected: neither read nor write trigger requested");
        return false;
    }
    if (watchpoints_.size() >= 64) return false;
    watchpoints_.push_back({start, end, true, onRead, onWrite});
    return true;
}

void Debugger::clearWatchpoints() {
    watchpoints_.clear();
}

bool Debugger::watchpointHit(const std::vector<Memory::Write> &writes) const {
    for (const auto &w : writes)
        for (const auto &wp : watchpoints_)
            if (wp.enabled && wp.onWrite && w.address >= wp.start && w.address <= wp.end) return true;
    return false;
}

void Debugger::takeSnapshot() {
    std::copy(emulator_.memory().data(), emulator_.memory().data() + Memory::SIZE, snapshotMemory_.begin());
    snapshotCPU_ = captureState();
    snapshotValid_ = true;
}
void Debugger::clearSnapshot() {
    snapshotValid_ = false;
}

std::vector<DebugMemoryDelta> Debugger::snapshotDiff() const {
    std::vector<DebugMemoryDelta> out;
    if (!snapshotValid_) return out;
    const uint8_t *current = emulator_.memory().data();
    for (std::size_t i = 0; i < Memory::SIZE; ++i)
        if (snapshotMemory_[i] != current[i])
            out.push_back({static_cast<uint16_t>(i), snapshotMemory_[i], current[i]});
    return out;
}

bool Debugger::beforeInstruction() {
    const uint16_t pc = emulator_.cpu().PC;
    bool stop = false;
    for (auto &bp : breakpoints_) {
        if (!bp.enabled || bp.address != pc) continue;
        ++bp.hitCount;
        if (bp.ignoreLeft > 0) {
            --bp.ignoreLeft;
            continue;
        }
        if (bp.logMode) {
            if (logSink_) {
                char addr[8];
                std::snprintf(addr, sizeof(addr), "%04X", bp.address);
                logSink_(std::string("[LOG $") + addr + "] " +
                         (bp.logMessage.empty() ? bp.condition : bp.logMessage));
            }
            continue;
        }
        if (!bp.condition.empty()) {
            uint64_t v = 0;
            std::string err;
            if (!evaluateExpression(bp.condition, v, err)) {
                char addr[8];
                std::snprintf(addr, sizeof(addr), "%04X", bp.address);
                logErrorOnce(std::string("breakpoint $") + addr + " condition '" + bp.condition +
                             "': " + err);
                continue;
            }
            if (v == 0) continue;
        }
        stop = true;
    }
    lastStopBreakpoint_ = stop;
    lastStopWatchpoint_ = false;
    return !lastStopBreakpoint_;
}

int Debugger::step() {
    clearStopReason();
    CPU &cpu = emulator_.cpu();
    if (cpu.halted()) return 0;
    if (!beforeInstruction()) return 0;
    const CPUStateSnapshot before = captureState();
    const uint16_t pcBefore = cpu.PC;
    const uint8_t opcode = emulator_.memory().read(pcBefore);
    const std::string disasm = cpu.disassembleOne(pcBefore);
    emulator_.memory().beginWriteTrace();
    const int cycles = cpu.step();
    emulator_.tickApu(cycles);
    afterInstruction(pcBefore, opcode, cycles, before, disasm);
    lastStopWatchpoint_ = watchpointHit(emulator_.memory().lastWrites());
    if (lastStopWatchpoint_) emulator_.pause();
    return cycles;
}

uint64_t Debugger::runInstructions(uint64_t budget) {
    emulator_.start();
    uint64_t executed = 0;
    while (executed < budget) {
        if (step() <= 0) {
            emulator_.pause();
            break;
        }
        ++executed;
        if ((executed & 0x3FF) == 0) publishSnapshot();
        if (lastStopBreakpoint_ || lastStopWatchpoint_ || emulator_.cpu().halted()) {
            emulator_.pause();
            break;
        }
    }
    publishSnapshot();
    return executed;
}

uint64_t Debugger::stepOver(uint64_t budget) {
    CPU &cpu = emulator_.cpu();
    if (cpu.halted()) return 0;
    const uint8_t opcode = emulator_.memory().read(cpu.PC);
    const bool isCall = opcode == 0x20; // JSR abs
    if (!isCall) {
        step();
        return 1;
    }
    const uint16_t target = static_cast<uint16_t>(cpu.PC + 3);
    emulator_.start();
    uint64_t executed = 0;
    lastStopStepTarget_ = false;
    while (executed < budget) {
        if (step() <= 0) break;
        ++executed;
        if (lastStopBreakpoint_ || lastStopWatchpoint_) break;
        if (cpu.PC == target && !cpu.halted()) {
            lastStopStepTarget_ = true;
            emulator_.pause();
            break;
        }
        if (cpu.halted()) break;
    }
    emulator_.pause();
    return executed;
}

uint64_t Debugger::stepOut(uint64_t budget) {
    CPU &cpu = emulator_.cpu();
    if (cpu.halted()) return 0;
    // Walk the current stack depth by watching JSR/RTS as they execute.
    int depth = 0;
    emulator_.start();
    uint64_t executed = 0;
    lastStopStepTarget_ = false;
    while (executed < budget) {
        const uint8_t op = emulator_.memory().read(cpu.PC);
        const int cycles = step();
        if (cycles <= 0) break;
        ++executed;
        if (op == 0x20)
            ++depth;                         // JSR
        else if (op == 0x60 || op == 0x40) { // RTS / RTI pops a frame
            if (depth > 0)
                --depth;
            else {
                lastStopStepTarget_ = true;
                emulator_.pause();
                break;
            }
        }
        if (lastStopBreakpoint_ || lastStopWatchpoint_ || cpu.halted()) break;
    }
    emulator_.pause();
    return executed;
}

uint64_t Debugger::runTo(uint16_t address, uint64_t budget) {
    CPU &cpu = emulator_.cpu();
    if (cpu.halted()) return 0;
    emulator_.start();
    uint64_t executed = 0;
    lastStopStepTarget_ = false;
    while (executed < budget) {
        if (cpu.PC == address) {
            lastStopStepTarget_ = true;
            emulator_.pause();
            break;
        }
        if (step() <= 0) break;
        ++executed;
        if (lastStopBreakpoint_ || lastStopWatchpoint_ || cpu.halted()) break;
    }
    emulator_.pause();
    return executed;
}

std::vector<Debugger::CallFrame> Debugger::callStack() const {
    std::vector<CallFrame> frames;
    const CPU &cpu = emulator_.cpu();
    const Memory &mem = emulator_.memory();
    // Stack grows down from $01FF; SP points at the next free slot.
    for (uint16_t sp = static_cast<uint16_t>(cpu.SP + 1); sp + 1u <= 0xFFu; sp += 2) {
        const uint16_t lo = mem.read(static_cast<uint16_t>(0x0100 + sp));
        const uint16_t hi = mem.read(static_cast<uint16_t>(0x0100 + ((sp + 1) & 0xFF)));
        const uint16_t ret = static_cast<uint16_t>((hi << 8) | lo);
        // A plausible JSR frame: the pushed value is the LAST byte of JSR,
        // so the instruction starts at ret-2 with opcode $20 there.
        if (ret >= 3 && mem.read(static_cast<uint16_t>(ret - 2)) == 0x20) {
            CallFrame f;
            f.returnAddress = ret;
            f.callerInstruction = static_cast<uint16_t>(ret - 2);
            f.sp = sp;
            frames.push_back(f);
        }
        if (sp == 0xFF) break;
    }
    return frames;
}

void Debugger::publishSnapshot() noexcept {
    // Seqlock: odd sequence = write in progress, even = stable.
    const uint32_t seq = snapSeq_.load(std::memory_order_relaxed);
    LockFreeSnapshot &buf = snapBuffers_[(seq >> 1) & 1u];
    snapSeq_.store(seq + 1, std::memory_order_release); // mark "writing"
    buf.cpu = captureState();
    buf.totalCycles = buf.cpu.totalCycles;
    const Memory &mem = emulator_.memory();
    const unsigned sp = emulator_.cpu().SP;
    for (unsigned i = 0; i < 256; ++i)
        buf.stackTop[i] = mem.read(static_cast<uint16_t>(0x0100 + ((sp + 1 + i) & 0xFF)));
    buf.magic = LockFreeSnapshot::kMagic;
    snapSeq_.store(seq + 2, std::memory_order_release); // mark "stable"
}

bool Debugger::tryAcquireSnapshot(LockFreeSnapshot &out) const noexcept {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = snapSeq_.load(std::memory_order_acquire);
        if (before & 1u) continue; // writer in progress
        // The last COMPLETED publish used the other buffer slot.
        const LockFreeSnapshot &buf = snapBuffers_[((before >> 1) & 1u) ^ 1u];
        out = buf;
        const uint32_t after = snapSeq_.load(std::memory_order_acquire);
        if (after == before && out.magic == LockFreeSnapshot::kMagic) return true;
    }
    return false;
}

void Debugger::afterInstruction(uint16_t pcBefore, uint8_t opcode, int cycles, const CPUStateSnapshot &before,
                                const std::string &disassembly) {
    if (coverageEnabled_) coverage_[pcBefore] = true;
    const CPUStateSnapshot after = captureState();
    const auto writes = emulator_.memory().lastWrites();
    std::vector<DebugMemoryDelta> deltas;
    deltas.reserve(writes.size());
    for (const auto &w : writes) deltas.push_back({w.address, w.oldValue, w.newValue});

    if (historyEnabled_) {
        if (historyPosition_ > 0) {
            history_.erase(history_.end() - static_cast<std::ptrdiff_t>(historyPosition_), history_.end());
            historyPosition_ = 0;
        }
        DebugHistoryEntry h;
        h.pc = pcBefore;
        h.opcode = opcode;
        h.before = before;
        h.writes = deltas;
        h.cycles = cycles;
        h.disassembly = disassembly;
        history_.push_back(std::move(h));
        while (history_.size() > historyCapacity_) history_.pop_front();
    }

    if (traceEnabled_) {
        bool pass = traceFilter_.empty();
        if (!pass) {
            uint64_t v = 0;
            std::string err;
            pass = evaluateExpression(traceFilter_, v, err) && v != 0;
        }
        if (pass) {
            DebugTraceEntry t;
            t.sequence = ++traceSequence_;
            t.pc = pcBefore;
            t.opcode = opcode;
            t.before = before;
            t.after = after;
            t.cycles = cycles;
            t.disassembly = disassembly;
            t.writes = deltas;
            trace_.push_back(std::move(t));
            while (trace_.size() > traceCapacity_) trace_.pop_front();
        }
    }

    if (profilerEnabled_) {
        profiler_[pcBefore].executions++;
        profiler_[pcBefore].cycles += static_cast<uint64_t>(std::max(0, cycles));
    }
}

std::string Debugger::upper(std::string text) {
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

bool Debugger::evaluateCondition(const std::string &expression) const {
    std::string e = expression;
    e.erase(std::remove_if(e.begin(), e.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
            e.end());
    const CPU &c = emulator_.cpu();
    auto value = [&](const std::string &token, uint64_t &out) -> bool {
        const std::string u = upper(token);
        if (u == "A") {
            out = c.A;
            return true;
        }
        if (u == "X") {
            out = c.X;
            return true;
        }
        if (u == "Y") {
            out = c.Y;
            return true;
        }
        if (u == "PC") {
            out = c.PC;
            return true;
        }
        if (u == "SP") {
            out = c.SP;
            return true;
        }
        if (u == "P") {
            out = c.P;
            return true;
        }
        if (u == "CYCLES") {
            out = c.getTotalCycles();
            return true;
        }
        uint16_t parsed = 0;
        if (!Assembler::parseValue(token, parsed, emulator_.program().symbols)) {
            logErrorOnce("condition '" + expression + "': unknown operand '" + token + "'");
            return false;
        }
        out = parsed;
        return true;
    };
    static const char *ops[] = {"==", "!=", ">=", "<=", ">", "<"};
    for (const char *op : ops) {
        const std::size_t pos = e.find(op);
        if (pos == std::string::npos) continue;
        uint64_t a = 0, b = 0;
        if (!value(e.substr(0, pos), a) || !value(e.substr(pos + std::strlen(op)), b)) return false;
        if (std::strcmp(op, "==") == 0) return a == b;
        if (std::strcmp(op, "!=") == 0) return a != b;
        if (std::strcmp(op, ">=") == 0) return a >= b;
        if (std::strcmp(op, "<=") == 0) return a <= b;
        if (std::strcmp(op, ">") == 0) return a > b;
        return a < b;
    }
    // No comparison operator at all: never matches - surface it instead of
    // silently treating it as "condition false".
    logErrorOnce("condition '" + expression + "': no comparison operator");
    return false;
}

void Debugger::logErrorOnce(const std::string &msg) const {
    if (!logSink_) return;
    for (const std::string &seen : recentErrors_)
        if (seen == msg) return; // seen within the last kRecentErrors distinct messages
    recentErrors_[recentErrorsNext_] = msg;
    recentErrorsNext_ = (recentErrorsNext_ + 1) % kRecentErrors;
    logSink_(msg);
}

std::string Debugger::statusText() const {
    std::ostringstream o;
    o << "history=" << history_.size() << '/' << historyCapacity_ << "  trace=" << trace_.size() << '/'
      << traceCapacity_ << "  breakpoints=" << breakpoints_.size() << "  watchpoints=" << watchpoints_.size()
      << "  snapshot=" << (snapshotValid_ ? "yes" : "no") << "\n";
    if (historyPosition_) o << "history cursor: -" << historyPosition_ << " (debug time travel)\n";
    return o.str();
}
