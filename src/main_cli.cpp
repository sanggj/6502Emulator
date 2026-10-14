// Cross-platform command-line entry point. On Windows the full IDE GUI
// (main.cpp) takes precedence; on Linux, macOS and other systems this builds
// the same engine as a headless CLI tool with identical flags.
#include "headless.hpp"

#include <string>

int main(int argc, char **argv) {
    std::string args;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) args += ' ';
        args += argv[i];
    }
    return headlessMain(args);
}
