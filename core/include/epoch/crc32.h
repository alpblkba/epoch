#ifndef EPOCH_CRC32_H
#define EPOCH_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32/ISO-HDLC (reflected polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF); check value for "123456789" is 0xCBF43926. */
uint32_t epoch_crc32(const uint8_t *data, size_t len);

/* The same CRC over 32-bit words taken as little-endian bytes, so the result does not depend on host byte order. */
uint32_t epoch_crc32_words(const uint32_t *words, size_t n);

#endif
