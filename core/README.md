# Portable C11 core

The portable core contains the wire codec (`wire.h`), the controller state machine for session control and retained transactions (`controller.h`), the host outcome classifier (`host.h`), the two-slot retained journal (`journal.h`), a bounded single-producer single-consumer byte ring (`ring.h`) and a CRC-32/ISO-HDLC implementation (`crc32.h`). None of them uses Zephyr headers or heap allocation, and the controller reaches its platform through callbacks for randomness, transmission, memory barriers and test checkpoints.

The ring has one producer and one consumer and relies on lock-free C11 atomic indices with acquire/release ordering. Full-ring insertion drops the incoming byte and increments a producer-owned counter; the caller must decide how to handle overflow. `epoch_crc32_words()` serializes 32-bit words in little-endian byte order before calculating the checksum.

The default CMake build compiles and tests only the ring and CRC-32. Configure with `-DEPOCH_BUILD_EXPERIMENTAL=ON` to build the remaining modules and their tests.
