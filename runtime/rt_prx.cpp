/* C entry point to PPSSPP's PRX decryption (Core/ELF/PrxDecrypter.cpp, GPL-2.0, Copyright (c) 2012- PPSSPP
 * Project), which CMake downloads from tag v1.20.4 and checks by SHA-256: it holds the PSP's keys, so, like
 * libkirk, it is never part of this repository. */
#include "Core/ELF/PrxDecrypter.h"
#include "rt_prx.h"

extern "C" int rt_prx_decrypt(const unsigned char *in, unsigned char *out, unsigned size) {
    return pspDecryptPRX(in, out, size);
}