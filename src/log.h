#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

// Returns a timestamp prefix: "HH:MM:SS.mmm "
inline std::string log_ts() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}
