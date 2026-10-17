/* Stand-in for PPSSPP's Common/CommonTypes.h, so its Core/ELF/PrxDecrypter.cpp (downloaded at configure
 * time, see CMakeLists.txt) builds alone. The PSP and every host we build for are little-endian, so the
 * *_le types are plain integers. */
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u16 u16_le;
typedef u32 u32_le;
typedef s32 s32_le;