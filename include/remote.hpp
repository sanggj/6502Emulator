#pragma once

#include <mutex>
#include <string>

class Emulator;
class Debugger;

namespace remote {
std::mutex &emulatorMutex();
}

class RemoteServer {
  public:
    static RemoteServer &instance();

    bool start(Emulator &emu, Debugger &dbg, int port, std::string &error);
    void stop();
    bool running() const noexcept;

  private:
    RemoteServer() = default;
    struct Impl;
    Impl *impl_ = nullptr;
};
