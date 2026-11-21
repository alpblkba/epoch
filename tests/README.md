# Native tests

The default CMake configuration builds and runs `test_ring` and `test_crc32`, including a two-million-byte producer/consumer stress test and CRC reference vectors. These tests do not require a connected microcontroller.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Additional tests for the journal, controller, wire codec and simulator can be enabled with `-DEPOCH_BUILD_EXPERIMENTAL=ON` when the unpublished development sources are present. Passing native tests does not by itself establish correct MCU retention or reset behavior.
