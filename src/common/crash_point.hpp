#pragma once

#include <cstdlib>
#include <string_view>

namespace minilsm {

inline constexpr int kCrashPointExitCode = 86;

// Test-only persistence-boundary hook. A match exits immediately without
// unwinding, which models sudden process loss without a fault framework.
inline void CrashPoint(std::string_view name) {
    const char* requested = std::getenv("MINILSM_CRASH_AT");
    if (requested != nullptr && name == requested) {
        std::_Exit(kCrashPointExitCode);
    }
}

}  // namespace minilsm
