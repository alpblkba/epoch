#include "epoch/crc32.h"

static uint32_t update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t epoch_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = update(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t epoch_crc32_words(const uint32_t *words, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        for (unsigned s = 0; s < 32u; s += 8u) {
            crc = update(crc, (uint8_t)(words[i] >> s));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
