# Native tests

The default CMake configuration builds and runs `test_ring` and `test_crc32`, including a two-million-byte producer/consumer stress test and CRC reference vectors. These tests do not require a connected microcontroller.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DEPOCH_BUILD_EXPERIMENTAL=ON` adds `test_journal`, `test_journal_model`, `test_state_machine`, `test_ep1_model`, `test_fault_campaign`, `test_wire`, `test_trial_verdict`, `test_fault_hooks` and `test_fault_hooks_corrupt`, which cover the journal at every store of publication and cold initialization, a check of every classification against the publish history that produced the slots, the controller state machine with its full EP0 transition matrix, exactly-once EP1 effects across resets at every checkpoint, the simulated fault campaign, the wire codec, the trial verdict rules and the firmware's reset-injection hooks in the standard and the corrupted-slot build. Passing native tests does not by itself establish correct MCU retention or reset behavior.
