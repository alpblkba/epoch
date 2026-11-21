#include <string.h>

#include "epoch/crc32.h"
#include "test.h"

int main(void)
{
    CHECK(epoch_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u); /* catalogue check value */
    CHECK(epoch_crc32(NULL, 0) == 0u);
    const uint8_t zero = 0;
    CHECK(epoch_crc32(&zero, 1) == 0xD202EF8Du);
    const uint32_t w[2] = {0x04030201u, 0x08070605u};
    const uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(epoch_crc32_words(w, 2) == epoch_crc32(b, 8)); /* words are little-endian bytes */
    CHECK(epoch_crc32_words(w, 0) == 0u);
    return TEST_DONE("crc32");
}
