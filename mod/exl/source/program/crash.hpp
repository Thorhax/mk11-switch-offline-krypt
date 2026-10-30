#pragma once
// Crash logging: Atmosphère writes no crash reports for this title on the test console, so the
// module installs its own user exception handler and writes the registers and a frame-pointer
// backtrace (as main+offset) to log.txt before the game is closed.

namespace mk11::crash {
    void Install();
}
