#pragma once
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>

using Clock = std::chrono::steady_clock;

inline double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// Wall-clock seconds since the Unix epoch. Used only to timestamp phases so they can be lined
// up with vmstat/iostat/top logs; all interval measurements still use the monotonic Clock.
inline double epoch_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[noreturn]] inline void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}
