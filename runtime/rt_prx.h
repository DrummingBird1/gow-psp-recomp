/* Decrypts a "~PSP" module (an EBOOT.BIN or PRX off the disc) with PPSSPP's PrxDecrypter (rt_prx.cpp).
 * out must hold max(elf_size, psp_size) bytes of the header (0x28, 0x2C); returns the decrypted size,
 * or a negative value when no known key fits. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int rt_prx_decrypt(const unsigned char *in, unsigned char *out, unsigned size);
#ifdef __cplusplus
}
#endif