/* Stand-in for PPSSPP's ppsspp_config.h: just the architecture tests at3_standalone uses. */
#pragma once

#define PPSSPP_ARCH(x) (PPSSPP_ARCH_##x)

#if defined(_M_X64) || defined(__x86_64__)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 1
#elif defined(_M_IX86) || defined(__i386__)
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 1
#define PPSSPP_ARCH_SSE2 1
#else
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 0
#endif

#if defined(__ARM_NEON) || defined(_M_ARM64)
#define PPSSPP_ARCH_ARM_NEON 1
#else
#define PPSSPP_ARCH_ARM_NEON 0
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
#define PPSSPP_ARCH_ARM64 1
#else
#define PPSSPP_ARCH_ARM64 0
#endif
