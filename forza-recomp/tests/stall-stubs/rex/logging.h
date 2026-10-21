#pragma once
template <typename... T> void StallTestLog(const char*, const T&...) {}
#define REXLOG_WARN(...) StallTestLog(__VA_ARGS__)
#define REXLOG_INFO(...) StallTestLog(__VA_ARGS__)
