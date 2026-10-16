/* Stand-in for PPSSPP's Common/Log.h, so the downloaded at3_standalone sources build unchanged.
 * Decoder messages are dropped except errors, which go to stdout with an [atrac] prefix. */
#pragma once
#include <cstdio>

namespace Log { enum LogType { ME }; }

#define DEBUG_LOG(t, ...) do {} while (0)
#define INFO_LOG(t, ...) do {} while (0)
#define WARN_LOG(t, ...) do {} while (0)
#define ERROR_LOG(t, ...) do { std::printf("[atrac] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
