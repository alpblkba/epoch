# Portable C11 core

The initial public core exports a bounded single-producer single-consumer byte ring (`ring.h`, `ring.c`) and a CRC-32/ISO-HDLC implementation (`crc32.h`, `crc32.c`). Both are independent of Zephyr headers and heap allocation.

The ring has one producer and one consumer and relies on lock-free C11 atomic indices with acquire/release ordering. Full-ring insertion drops the incoming byte and increments a producer-owned counter; the caller must decide how to handle overflow. `epoch_crc32_words()` serializes 32-bit words in little-endian byte order before calculating the checksum.

Build and test these primitives from the repository root with `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`. Other protocol core components are still under development and are not part of this initial public source set.
