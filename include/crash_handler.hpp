#pragma once

// Installs an unhandled-exception filter that writes a timestamped minidump
// (crash_YYYYMMDD_HHMMSS.dmp) next to the executable before terminating.
void installCrashHandler();
