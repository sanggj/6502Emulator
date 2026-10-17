#include "remote.hpp"
#include "cpu.hpp"
#include "debugger.hpp"
#include "emulator.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {
std::mutex g_emuMutex;
}

namespace remote {

std::mutex &emulatorMutex() {
    return g_emuMutex;
}

} // namespace remote

struct RemoteServer::Impl {
    Emulator *emu = nullptr;
    Debugger *dbg = nullptr;
    std::thread worker;
    std::atomic<bool> running_{false};
#ifdef _WIN32
    SOCKET listenSock = INVALID_SOCKET;
    // Active client socket, tracked so stop() can wake a worker blocked in
    // recv() by shutting it down before join().
    std::atomic<SOCKET> clientSock{INVALID_SOCKET};
    static constexpr std::size_t kMaxClientBuffer = 64 * 1024;
#endif

    static std::string jsonEscape(const std::string &s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n')
                out += "\\n";
            else if (c == '\r') {
            } else
                out += c;
        }
        return out;
    }

    static bool findU64(const std::string &line, const char *key, uint64_t &out) {
        const std::string needle = std::string("\"") + key + "\":";
        const auto pos = line.find(needle);
        if (pos == std::string::npos) return false;
        out = std::strtoull(line.c_str() + pos + needle.size(), nullptr, 0);
        return true;
    }

    static void sendAll(SOCKET sock, const char *data, int len) {
        int sent = 0;
        while (sent < len) {
            const int n = send(sock, data + sent, len - sent, 0);
            if (n <= 0) return;
            sent += n;
        }
    }

    static std::string handleLine(const std::string &line) {
        auto has = [&line](const char *cmd) {
            return line.find("\"cmd\":\"" + std::string(cmd) + "\"") != std::string::npos;
        };
        std::lock_guard<std::mutex> guard(g_emuMutex);
        Emulator &emu = *impl()->emu;
        Debugger &dbg = *impl()->dbg;

        if (has("ping")) return "{\"ok\":true,\"reply\":\"pong\"}";
        if (has("regs")) {
            const CPU &c = emu.cpu();
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "{\"ok\":true,\"a\":%u,\"x\":%u,\"y\":%u,\"pc\":%u,\"sp\":%u,\"p\":%u,\"cycles\":%"
                          "llu,\"halted\":%s}",
                          c.A, c.X, c.Y, c.PC, c.SP, c.P, static_cast<unsigned long long>(c.getTotalCycles()),
                          c.halted() ? "true" : "false");
            return buf;
        }
        if (has("mem")) {
            uint64_t addr = 0, len = 16;
            findU64(line, "addr", addr);
            findU64(line, "len", len);
            len = std::min<uint64_t>(len, 256);
            std::string bytes;
            char b[8];
            for (uint64_t i = 0; i < len; ++i) {
                std::snprintf(b, sizeof(b), "%02X ", emu.memory().read(static_cast<uint16_t>(addr + i)));
                bytes += b;
            }
            return "{\"ok\":true,\"bytes\":\"" + jsonEscape(bytes) + "\"}";
        }
        if (has("poke")) {
            uint64_t addr = 0, val = 0;
            if (!findU64(line, "addr", addr) || !findU64(line, "val", val))
                return "{\"ok\":false,\"error\":\"need addr/val\"}";
            emu.memory().write(static_cast<uint16_t>(addr), static_cast<uint8_t>(val));
            return "{\"ok\":true}";
        }
        if (has("step")) {
            const int cycles = dbg.step();
            return "{\"ok\":true,\"cycles\":" + std::to_string(cycles) + "}";
        }
        if (has("run")) {
            emu.start();
            return "{\"ok\":true}";
        }
        if (has("pause") || has("stop")) {
            emu.pause();
            return "{\"ok\":true}";
        }
        if (has("reset")) {
            emu.reset();
            dbg.clear();
            return "{\"ok\":true}";
        }
        if (has("bp")) {
            uint64_t addr = 0;
            findU64(line, "addr", addr);
            bool removeIt = line.find("\"on\":false") != std::string::npos;
            if (removeIt)
                return std::string("{\"ok\":") +
                       (dbg.removeBreakpoint(static_cast<uint16_t>(addr)) ? "true" : "false") + "}";
            return std::string("{\"ok\":") +
                   (dbg.addBreakpoint(static_cast<uint16_t>(addr)) ? "true" : "false") + "}";
        }
        if (has("coverage")) {
            const double pct = dbg.coveragePercent(0x8000, 0xFFFF);
            return "{\"ok\":true,\"coverage\":" + std::to_string(pct) + "}";
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }

    void serveClient(SOCKET client) {
        clientSock.store(client);
        // Closes the client only if stop() has not already taken it.
        auto closeClient = [this, client]() {
            SOCKET expected = client;
            if (clientSock.compare_exchange_strong(expected, INVALID_SOCKET)) closesocket(client);
        };
        std::string buffer;
        char chunk[1024];
        while (running_.load()) {
            const int got = recv(client, chunk, sizeof(chunk), 0);
            if (got <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(got));
            if (buffer.size() > kMaxClientBuffer) break; // oversized input: drop connection
            std::size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (line.empty()) continue;
                if (line.find("\"cmd\":\"quit\"") != std::string::npos) {
                    static const char kBye[] = "{\"ok\":true,\"bye\":true}\n";
                    sendAll(client, kBye, static_cast<int>(sizeof(kBye) - 1));
                    closeClient();
                    return;
                }
                const std::string response = handleLine(line);
                const std::string out = response + "\n";
                sendAll(client, out.c_str(), static_cast<int>(out.size()));
            }
        }
        closeClient();
    }

    static RemoteServer::Impl *impl() { return RemoteServer::instance().impl_; }

    void run() {
#ifdef _WIN32
        while (running_.load()) {
            // select() with a timeout instead of a bare accept(): Windows does
            // not reliably cancel a blocked accept() when closesocket() runs on
            // another thread, so stop() must be noticed via running_ within
            // ~200ms rather than by the socket being closed underneath us.
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listenSock, &readfds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000; // 200ms
            const int ready = select(0, &readfds, nullptr, nullptr, &tv);
            if (ready < 0) {
                // select() failed spontaneously: this worker exits, so clear
                // the flag or running() would keep claiming a live server.
                // Only the flag is cleared here - stop()/next start() still
                // own closing listenSock (after the join) and WSACleanup,
                // so no socket is closed on this path (no double-close).
                running_.store(false);
                break;
            }
            if (ready == 0) continue; // timeout: re-check running_
            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            SOCKET client = accept(listenSock, reinterpret_cast<sockaddr *>(&peer), &peerLen);
            if (client == INVALID_SOCKET) {
                running_.store(false); // same: worker leaving the loop, flag must follow
                break;
            }
            serveClient(client);
        }
#endif
    }
};

RemoteServer &RemoteServer::instance() {
    static RemoteServer server;
    return server;
}

bool RemoteServer::running() const noexcept {
    return impl_ && impl_->running_.load();
}

bool RemoteServer::start(Emulator &emu, Debugger &dbg, int port, std::string &error) {
    stop();
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    impl_ = new Impl();
    impl_->emu = &emu;
    impl_->dbg = &dbg;
    impl_->listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listenSock == INVALID_SOCKET) {
        error = "socket() failed";
        stop(); // release Impl + balance the WSAStartup above on failure too
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // Loopback only: the debug server must never be reachable off-box.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(impl_->listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(impl_->listenSock, 1) != 0) {
        error = "bind/listen failed on port " + std::to_string(port);
        stop(); // closes listenSock, joins nothing, WSACleanup, deletes Impl
        return false;
    }
    impl_->running_.store(true);
    impl_->worker = std::thread([impl = impl_] { impl->run(); });
    error = "remote debug listening on 127.0.0.1:" + std::to_string(port);
    return true;
#else
    (void)emu;
    (void)dbg;
    (void)port;
    error = "remote debug is Windows-only";
    return false;
#endif
}

void RemoteServer::stop() {
    if (!impl_) return;
    impl_->running_.store(false);
#ifdef _WIN32
    // Wake a worker blocked in recv() before joining it. (recv needs no
    // SO_RCVTIMEO: shutdown() here wakes it, and a receive timeout would only
    // drop idle-but-alive clients.)
    const SOCKET client = impl_->clientSock.exchange(INVALID_SOCKET);
    if (client != INVALID_SOCKET) {
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
#endif
    if (impl_->worker.joinable()) impl_->worker.join(); // worker polls running_ every 200ms (select)
#ifdef _WIN32
    // Close the listener only after the worker has left its select()/accept
    // loop: stop() must never race the worker on listenSock, and must not
    // depend on closesocket() cancelling a blocked accept() (Windows does not
    // guarantee that).
    if (impl_->listenSock != INVALID_SOCKET) {
        closesocket(impl_->listenSock);
        impl_->listenSock = INVALID_SOCKET;
    }
    WSACleanup();
#endif
    delete impl_;
    impl_ = nullptr;
}
