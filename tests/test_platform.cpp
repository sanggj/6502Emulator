// Platform layer tests: UTF conversion, binary file IO, monotonic clock,
// tr() translation table, remote-server loopback integration and headless CLI
// subprocess behaviour.

// winsock2 must precede windows.h (platform.hpp pulls in windows.h).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "platform.hpp"
#include "strings.hpp"

#include "debugger.hpp"
#include "emulator.hpp"
#include "remote.hpp"

#include "assembler.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int g_plat_pass = 0;
int g_plat_fail = 0;

#define CHECK(expr)                                                                          \
    do {                                                                                     \
        if (expr) {                                                                          \
            ++g_plat_pass;                                                                    \
        } else {                                                                             \
            ++g_plat_fail;                                                                    \
            std::fprintf(stderr, "FAIL test_platform.cpp:%d %s\n", __LINE__, #expr);          \
        }                                                                                    \
    } while (0)

namespace {

std::string tempDir() {
    const char *t = std::getenv("TEMP");
    if (t && *t) return std::string(t);
    t = std::getenv("TMP");
    if (t && *t) return std::string(t);
    return ".";
}

#ifdef _WIN32

// Loopback client with receive/send timeouts so no test phase can hang.
SOCKET connectTo(int port, DWORD timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs),
               sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs),
               sizeof(timeoutMs));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool sendAll(SOCKET s, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Send one protocol line; length comes from the string, never hand-counted.
bool sendLine(SOCKET s, const std::string &line) {
    return sendAll(s, line.c_str(), static_cast<int>(line.size()));
}

// Read one '\n'-terminated response (timeout-bounded; returns a partial line
// if the server drops the connection first).
std::string recvLine(SOCKET s) {
    std::string out;
    char ch = 0;
    while (out.size() < 4096) {
        const int r = recv(s, &ch, 1, 0);
        if (r <= 0) break;
        if (ch == '\n') break;
        out += ch;
    }
    return out;
}

// Ask the OS for a free loopback port (bound then released immediately; an
// unconnected listener leaves no TIME_WAIT behind).
int pickFreePort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0) {
        sockaddr_in got{};
        int len = sizeof(got);
        if (getsockname(s, reinterpret_cast<sockaddr *>(&got), &len) == 0)
            port = ntohs(got.sin_port);
    }
    closesocket(s);
    return port;
}

// The "ms" field is wall-clock and varies run to run; everything else must be
// byte-identical for the determinism check.
std::string stripMs(const std::string &s) {
    std::string out = s;
    const std::size_t pos = out.find("\"ms\":");
    if (pos != std::string::npos) {
        std::size_t end = pos + 5;
        while (end < out.size() && std::isdigit(static_cast<unsigned char>(out[end])) != 0) ++end;
        out.erase(pos, end - pos);
    }
    return out;
}

struct ProcOut {
    bool ok = false;
    DWORD code = 1;
    std::string out;
};

// Run the headless CLI with stdout/stderr captured and a hard timeout
// (TerminateProcess after timeoutMs) so a wedged child cannot hang the suite.
ProcOut runIde(const std::string &exe, const std::string &args, DWORD timeoutMs) {
    ProcOut r;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    CloseHandle(wr); // child holds the only write end now
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    GetExitCodeProcess(pi.hProcess, &r.code);
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0)
        r.out.append(chunk, n);
    r.ok = true;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return r;
}

#endif // _WIN32

} // namespace

int run_test_platform() {
    // ---- UTF-8 <-> UTF-16 round-trip ----
    const std::string utf8 = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xE2\x9C\x93";
    const std::wstring wide = platform::utf8ToWide(utf8);
    CHECK(!wide.empty());
    CHECK(platform::wideToUtf8(wide) == utf8);
    CHECK(platform::utf8ToWide(platform::wideToUtf8(L"abc\xE9")) == std::wstring(L"abc\xE9"));
    CHECK(platform::wideToUtf8(platform::utf8ToWide("plain ascii")) == "plain ascii");
    CHECK(platform::utf8ToWide("").empty());

    // ---- binary file round-trip in temp dir ----
    const std::string path = tempDir() + "\\test_platform_rw.bin";
    std::vector<uint8_t> out;
    for (int i = 0; i < 256; ++i) out.push_back(static_cast<uint8_t>(i));
    CHECK(platform::writeFileBinary(path, out.data(), out.size()));
    CHECK(platform::fileExists(path));
    std::vector<uint8_t> in;
    CHECK(platform::readFileBinary(path, in));
    CHECK(in.size() == out.size());
    CHECK(in == out);
    std::remove(path.c_str());
    CHECK(!platform::fileExists(path));
    CHECK(!platform::fileExists(tempDir() + "\\test_platform_missing_42.bin"));

    // ---- steadyMillis monotonic non-decreasing ----
    uint64_t prev = platform::steadyMillis();
    bool monotonic = true;
    for (int i = 0; i < 500; ++i) {
        const uint64_t now = platform::steadyMillis();
        if (now < prev) monotonic = false;
        prev = now;
    }
    CHECK(monotonic);
    platform::sleepMillis(2);
    CHECK(platform::steadyMillis() >= prev);

    // ---- strings::tr ----
    const std::string title = tr("app.title");
    CHECK(!title.empty());
    const std::string unknown = tr("no.such.key");
    CHECK(!unknown.empty());

    // ---- debugger expressions/conditions/watchpoints (item 4) ----
    {
        Emulator dEmu;
        Debugger dDbg(dEmu);
        std::string dmsg;
        CHECK(dEmu.assembleAndBurn("        .org $8000\r\n"
                                   "LOOP:   NOP\r\n"
                                   "        JMP LOOP\r\n"
                                   "        .org $FFFC\r\n"
                                   "        .word LOOP\r\n",
                                   dmsg));
        dEmu.cpu().A = 5;
        uint64_t v = 0;
        std::string derr;
        CHECK(dDbg.evaluateExpression("(A+2)", v, derr) && v == 7);
        CHECK(dDbg.evaluateExpression("(LOOP-1)", v, derr) && v == 0x7FFF);
        CHECK(!dDbg.evaluateExpression("(A+2", v, derr)); // unbalanced -> error
        CHECK(!derr.empty());
        // Bounded paren nesting: 100 nested parens must still evaluate, 1000
        // must fail with a clear error instead of overflowing the stack.
        {
            const std::string nest100 = std::string(100, '(') + "A+2" + std::string(100, ')');
            v = 0;
            derr.clear();
            CHECK(dDbg.evaluateExpression(nest100, v, derr) && v == 7);
            const std::string nest1000 = std::string(1000, '(') + "A+2" + std::string(1000, ')');
            v = 0;
            derr.clear();
            CHECK(!dDbg.evaluateExpression(nest1000, v, derr));
            CHECK(derr.find("nesting too deep") != std::string::npos);
        }
        // evaluateCondition must see program symbols just like
        // evaluateExpression does.
        CHECK(dDbg.addBreakpoint(0x8000, "LOOP == $8000"));
        CHECK(dDbg.breakpointHit(0x8000));
        dDbg.removeBreakpoint(0x8000);
        // Log-mode message carries the breakpoint address in hex.
        std::string logOut;
        dDbg.setLogSink([&logOut](const std::string &s) { logOut += s; });
        CHECK(dDbg.addBreakpoint(0x8123));
        CHECK(dDbg.setBreakpointLog(0x8123, "hit"));
        dEmu.cpu().PC = 0x8123;
        CHECK(dDbg.beforeInstruction());
        CHECK(logOut.find("[LOG $8123] hit") != std::string::npos);
        dDbg.removeBreakpoint(0x8123);
        // Condition parse errors surface through the log sink, once per
        // distinct message instead of on every hit.
        auto countSub = [](const std::string &hay, const char *needle) {
            std::size_t n = 0;
            std::size_t pos = 0;
            while ((pos = hay.find(needle, pos)) != std::string::npos) {
                ++n;
                pos += 1;
            }
            return n;
        };
        logOut.clear();
        CHECK(dDbg.addBreakpoint(0x8123, "NOSUCHSYM == 1"));
        CHECK(!dDbg.breakpointHit(0x8123));
        const std::size_t errs1 = countSub(logOut, "condition '");
        CHECK(errs1 >= 1);
        CHECK(!dDbg.breakpointHit(0x8123));
        CHECK(countSub(logOut, "condition '") == errs1); // logged once only
        logOut.clear();
        dEmu.cpu().PC = 0x8123;
        CHECK(dDbg.beforeInstruction()); // condition false -> keep running
        CHECK(countSub(logOut, "condition '") == 1);
        dDbg.beforeInstruction();
        CHECK(countSub(logOut, "condition '") == 1);
        dDbg.removeBreakpoint(0x8123);
        // Read watchpoints are rejected, not silently accepted.
        CHECK(!dDbg.addWatchpoint(0x1000, 0x1010, true, false));
        CHECK(dDbg.watchpoints().empty());
        CHECK(dDbg.addWatchpoint(0x1000, 0x1010));
        CHECK(dDbg.watchpoints().size() == 1);
    }

    // ---- assembler: ".equ" inside a FALSE ".if" must not reach ".repeat" ----
    // Pass 1 never defines FOO in the dead branch, so the preprocessor must
    // not record it either: ".repeat FOO" can no longer expand against a
    // symbol the conditional logic would never define.
    {
        Assembler asmx;
        AssembledProgram aprog;
        std::vector<AssemblerError> aerrs;
        const std::string srcDead =
            "        .org $8000\r\n"
            "        .if 0\r\n"
            "FOO     .equ 5\r\n"
            "        .endif\r\n"
            "        .repeat FOO\r\n"
            "        NOP\r\n"
            "        .endr\r\n"
            "start:  JMP start\r\n"
            "        .org $FFFC\r\n"
            "        .word start\r\n";
        aerrs.clear();
        CHECK(!asmx.assemble(srcDead, aprog, aerrs)); // dead-branch symbol must stay invisible
        CHECK(!aerrs.empty());

        // Control: an active branch keeps today's semantics exactly - FOO is
        // recorded, .repeat expands to exactly 5 NOPs before the JMP.
        const std::string srcLive =
            "        .org $8000\r\n"
            "        .if 1\r\n"
            "FOO     .equ 5\r\n"
            "        .endif\r\n"
            "        .repeat FOO\r\n"
            "        NOP\r\n"
            "        .endr\r\n"
            "start:  JMP start\r\n"
            "        .org $FFFC\r\n"
            "        .word start\r\n";
        aerrs.clear();
        CHECK(asmx.assemble(srcLive, aprog, aerrs));
        if (aprog.byteCount > 0) {
            CHECK(aprog.memory[0x8000] == 0xEA); // NOP 1
            CHECK(aprog.memory[0x8004] == 0xEA); // NOP 5
            CHECK(aprog.memory[0x8005] == 0x4C); // JMP right after the 5 NOPs
        }
    }

#ifdef _WIN32
    // ---- remote server: real loopback sockets (item 8a) ----
    // Whole section is bounded: every socket carries SO_RCVTIMEO/SO_SNDTIMEO,
    // every server phase is measured. WSAStartup/bind failures FAIL the run
    // unless SKIP_REMOTE_TESTS=1 is set in the environment.
    {
        WSADATA wsa{};
        const char *skipRemote = std::getenv("SKIP_REMOTE_TESTS");
        const bool remoteSkipped = skipRemote && std::strcmp(skipRemote, "1") == 0;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            if (remoteSkipped)
                std::printf("SKIP remote integration: WSAStartup failed (SKIP_REMOTE_TESTS=1)\n");
            else {
                std::fprintf(stderr, "FAIL reason: WSAStartup failed\n");
                CHECK(false); // winsock must work here; skip only via SKIP_REMOTE_TESTS=1
            }
        } else {
            Emulator emu;
            Debugger dbg(emu);
            RemoteServer &srv = RemoteServer::instance();

            // (e) Invalid bind must fail cleanly: occupy a loopback port with
            // our own listener first. (Windows has no privileged ports, so
            // "port already in use" is the deterministic invalid-bind case.)
            SOCKET blocker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            int occupiedPort = 0;
            if (blocker != INVALID_SOCKET) {
                sockaddr_in ba{};
                ba.sin_family = AF_INET;
                ba.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (bind(blocker, reinterpret_cast<sockaddr *>(&ba), sizeof(ba)) == 0) {
                    int blen = sizeof(ba);
                    if (getsockname(blocker, reinterpret_cast<sockaddr *>(&ba), &blen) == 0)
                        occupiedPort = ntohs(ba.sin_port);
                    listen(blocker, 1);
                }
            }
            if (occupiedPort != 0) {
                std::string err;
                CHECK(!srv.start(emu, dbg, occupiedPort, err));
                CHECK(!srv.running());
            }
            if (blocker != INVALID_SOCKET) closesocket(blocker);

            const int port = pickFreePort();
            std::string err;
            const uint64_t startAt = platform::steadyMillis();
            const bool started = port != 0 && srv.start(emu, dbg, port, err);
            if (!started) {
                if (remoteSkipped) {
                    std::printf("SKIP remote integration: cannot bind a loopback port (%s) "
                                "(SKIP_REMOTE_TESTS=1)\n",
                                err.c_str());
                } else {
                    std::fprintf(stderr, "FAIL reason: cannot bind a loopback port (%s)\n",
                                 err.c_str());
                    CHECK(false); // loopback bind must succeed; skip only via SKIP_REMOTE_TESTS=1
                }
            } else {
                CHECK(srv.running());

                // Valid request per the line protocol in src/remote.cpp.
                SOCKET c = connectTo(port, 3000);
                CHECK(c != INVALID_SOCKET);
                if (c != INVALID_SOCKET) {
                    CHECK(sendLine(c, "{\"cmd\":\"ping\"}\n"));
                    CHECK(recvLine(c).find("\"reply\":\"pong\"") != std::string::npos);
                    CHECK(sendLine(c, "{\"cmd\":\"regs\"}\n"));
                    CHECK(recvLine(c).find("\"ok\":true") != std::string::npos);

                    // Partial line split across TCP segments must be buffered.
                    CHECK(sendLine(c, "{\"cmd\":\"pi"));
                    platform::sleepMillis(120);
                    CHECK(sendLine(c, "ng\"}\n"));
                    CHECK(recvLine(c).find("\"reply\":\"pong\"") != std::string::npos);

                    // 70KB with no newline crosses the 64KB cap -> connection
                    // dropped (recv returns 0 or an error, NOT a timeout).
                    const std::string blob(70 * 1024, 'A');
                    (void)sendAll(c, blob.c_str(), static_cast<int>(blob.size()));
                    char tmp[256];
                    const int r = recv(c, tmp, sizeof(tmp), 0);
                    const int wsaErr = WSAGetLastError();
                    CHECK(r == 0 ||
                          (r == SOCKET_ERROR && wsaErr != WSAETIMEDOUT && wsaErr != WSAEWOULDBLOCK));
                    closesocket(c);

                    // Disconnect mid-stream: partial line, then abrupt close.
                    SOCKET c2 = connectTo(port, 3000);
                    CHECK(c2 != INVALID_SOCKET);
                    if (c2 != INVALID_SOCKET) {
                        (void)sendLine(c2, "{\"cmd\":\"reg");
                        closesocket(c2);
                        platform::sleepMillis(120);
                    }
                    // The server must be back in accept(): a fresh client works.
                    SOCKET c3 = connectTo(port, 3000);
                    CHECK(c3 != INVALID_SOCKET);
                    if (c3 != INVALID_SOCKET) {
                        CHECK(sendLine(c3, "{\"cmd\":\"ping\"}\n"));
                        CHECK(recvLine(c3).find("\"reply\":\"pong\"") != std::string::npos);
                        closesocket(c3);
                    }
                }

                srv.stop(); // timing sample AFTER stop(): stop()/join must be bounded too
                const uint64_t stopAt = platform::steadyMillis();
                CHECK(!srv.running());
                CHECK(stopAt - startAt < 15000); // whole session INCLUDING stop()/join bounded
            }
            // (d) 5x start/stop cycles: each phase bounded, no hang/leak.
            for (int i = 0; i < 5; ++i) {
                const int p = pickFreePort();
                std::string cerr;
                const uint64_t t0 = platform::steadyMillis();
                const bool st = p != 0 && srv.start(emu, dbg, p, cerr);
                const uint64_t startedMs = platform::steadyMillis() - t0;
                CHECK(st);
                if (!st) break;
                CHECK(srv.running());
                CHECK(startedMs < 2500); // start() must not block
                const uint64_t t1 = platform::steadyMillis();
                srv.stop();
                const uint64_t stoppedMs = platform::steadyMillis() - t1;
                CHECK(!srv.running());
                CHECK(stoppedMs < 5000); // stop() joins promptly, no hang
            }

            // 10 rapid start/stop cycles under one overall deadline: stop() must
            // notice a worker parked in select()/accept() within ~200ms, so the
            // whole burst finishes quickly and can never hang.
            const uint64_t rapid0 = platform::steadyMillis();
            bool rapidOk = true;
            for (int i = 0; i < 10; ++i) {
                const int p = pickFreePort();
                std::string rerr;
                if (p == 0 || !srv.start(emu, dbg, p, rerr) || !srv.running()) {
                    rapidOk = false;
                    break;
                }
                srv.stop();
                if (srv.running()) {
                    rapidOk = false;
                    break;
                }
                if (platform::steadyMillis() - rapid0 > 10000) { // overall deadline
                    rapidOk = false;
                    break;
                }
            }
            CHECK(rapidOk);
            CHECK(platform::steadyMillis() - rapid0 < 15000); // 10 cycles bounded

            WSACleanup();
        }
    }

    // ---- headless CLI subprocess behaviour (item 8b) ----
    {
        std::string exe;
        // Resolve relative to THIS test binary first (GetModuleFileNameA ->
        // <dir>\build\6502_ide.exe), then the original CWD-relative fallbacks.
        char modPath[MAX_PATH] = {};
        const DWORD modLen = GetModuleFileNameA(nullptr, modPath, MAX_PATH);
        if (modLen > 0 && modLen < MAX_PATH) {
            std::string dir(modPath);
            const std::size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) dir.resize(slash + 1);
            const std::string beside = dir + "build\\6502_ide.exe";
            if (platform::fileExists(beside)) exe = beside;
        }
        if (exe.empty() && platform::fileExists("build\\6502_ide.exe"))
            exe = "build\\6502_ide.exe";
        if (exe.empty() && platform::fileExists("6502_ide.exe")) exe = "6502_ide.exe";
        const char *skipCli = std::getenv("SKIP_CLI_TESTS");
        if (exe.empty()) {
            if (skipCli && *skipCli) {
                std::printf("SKIP CLI subprocess tests: 6502_ide.exe not found "
                            "(SKIP_CLI_TESTS set)\n");
            } else {
                // The bat always builds build\6502_ide.exe first -> missing exe is a failure.
                std::fprintf(stderr, "FAIL reason: 6502_ide.exe not found beside the test binary "
                                     "or in build\\ (set SKIP_CLI_TESTS to skip)\n");
                CHECK(false);
            }
        } else {
            // (i) --run without a budget -> exit 2 + JSON error field.
            const ProcOut r1 = runIde(exe, "--headless --run", 10000);
            CHECK(r1.ok);
            if (r1.ok) {
                CHECK(r1.code == 2);
                CHECK(r1.out.find("\"ok\":false") != std::string::npos);
            }

            // (ii) unknown flag -> exit 2.
            const ProcOut r2 = runIde(exe, "--headless --bogus", 10000);
            CHECK(r2.ok);
            if (r2.ok) {
                CHECK(r2.code == 2);
                CHECK(r2.out.find("unknown argument") != std::string::npos);
            }

            // (iii) valid asm under a %TEMP% path containing spaces ->
            // exit 0 + a JSON line that starts a line and says ok:true.
            const std::string cliDir = tempDir() + "\\cli test dir";
            CreateDirectoryA(cliDir.c_str(), nullptr);
            const std::string cliAsm = cliDir + "\\smoke test.asm";
            {
                std::ofstream f(cliAsm, std::ios::binary);
                f << "        .org $8000\r\n"
                     "start:  NOP\r\n"
                     "        JMP start\r\n"
                     "        .org $FFFC\r\n"
                     "        .word start\r\n";
            }
            const std::string cmd = "--headless --asm \"" + cliAsm + "\"";
            const ProcOut r3 = runIde(exe, cmd, 10000);
            CHECK(r3.ok);
            if (r3.ok) {
                CHECK(r3.code == 0);
                const std::size_t js = r3.out.find("{\"ok\":true");
                CHECK(js != std::string::npos &&
                      (js == 0 || r3.out[js - 1] == '\n')); // JSON line starts the line
            }

            // (iv) determinism: identical output except the "ms" field.
            const ProcOut r4 = runIde(exe, cmd, 10000);
            CHECK(r4.ok);
            if (r3.ok && r4.ok) CHECK(stripMs(r3.out) == stripMs(r4.out));

            // (v) --remote 0: port 0 is not a valid TCP port -> exit 2.
            // Each case runs a valid --asm too, so exit 2 can only come from
            // the rejected flag, not from the "nothing to do" fallback.
            const ProcOut r5 = runIde(exe, "--headless --asm \"" + cliAsm + "\" --remote 0", 10000);
            CHECK(r5.ok);
            if (r5.ok) CHECK(r5.code == 2);

            // (vi) --screenshot into a path whose parent directory does not
            // exist -> exit 2, no silent success.
            const std::string missingDir = tempDir() + "\\test_platform_no_such_dir_42";
            const std::string missShot = missingDir + "\\shot.bmp";
            const ProcOut r6 =
                runIde(exe, "--headless --asm \"" + cliAsm + "\" --screenshot \"" + missShot + "\"",
                       10000);
            CHECK(r6.ok);
            if (r6.ok) CHECK(r6.code == 2);
            CHECK(!platform::fileExists(missShot)); // never created

            // (vii) --waverec into a missing parent directory -> exit 2.
            const std::string missWav = missingDir + "\\out.wav";
            const ProcOut r7 =
                runIde(exe, "--headless --asm \"" + cliAsm + "\" --waverec \"" + missWav + "\"",
                       10000);
            CHECK(r7.ok);
            if (r7.ok) CHECK(r7.code == 2);
            CHECK(!platform::fileExists(missWav)); // never created

            std::remove(cliAsm.c_str());
            RemoveDirectoryA(cliDir.c_str());
        }
    }
#endif // _WIN32

    return g_plat_fail;
}
