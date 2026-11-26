# EPOCH

EPOCH investigates fault-tolerant host-controller IPC when a microcontroller can reset or lose a reply during a non-idempotent operation. It combines session-scoped requests, retained transaction records and recovery policies to distinguish confirmed execution from outcomes that remain unknown under a defined failure model.

The project uses a portable C11 core, a Zephyr-based STM32U585 target and deterministic host-side experiments. This source set contains the session protocol, a retained two-slot transaction journal, controller firmware with an optional test-only reset-injection build, a deterministic simulator and the host tools that run the reset experiments and re-check their records independently.

## Components

`core/` holds the portable wire codec, session controller, host outcome classifier, retained journal, byte ring and CRC-32. `host/` holds the simulator, a C host client and the Python experiment and verification tools. `firmware/stm32u585/` builds the UART echo, retention probe and controller applications from one Zephyr project, and `tests/` holds the native tests.

See [the architecture overview](docs/architecture.md) and the short guides in [core](core/README.md), [host](host/README.md), [STM32 firmware](firmware/stm32u585/README.md) and [tests](tests/README.md).

## Building and testing

The public host build requires CMake 3.20, a C11 compiler and POSIX threads:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This default build runs the ring and CRC-32 tests. `EPOCH_BUILD_EXPERIMENTAL=ON` adds the protocol core, the simulator, the C host client and the native tests of the wire codec, controller state machine, journal, a journal model check, an EP1 effect model, simulated fault campaign, trial verdicts and reset-injection hooks. `epoch_sim 1 300` runs 300 simulator seeds and exits nonzero when any seed reports a false outcome, a duplicate commit, a committed request reported rejected, a table or queue overflow, or no completed request after its faults:

```sh
cmake -S . -B build-exp -DEPOCH_BUILD_EXPERIMENTAL=ON
cmake --build build-exp
ctest --test-dir build-exp --output-on-failure
build-exp/epoch_sim 1 300
```

The host tools were tested with Python 3.14 and pySerial 3.5, their only dependency outside the standard library. The offline tests do not need a board, but the experiment-runner tests drive the serial path through a fake port and import pySerial, so install it first:

```sh
python3 -m pip install -r host/client/requirements.txt
cd host/client
python3 -m unittest test_uart_echo test_epoch_wire test_verify_trials test_retention_probe test_cold_init_trials test_verify_cold_init_trials
```

The firmware uses Zephyr v4.4.2 (`dccb09599635bdff17633fa7e9dab014b91dce90`) and Zephyr SDK 1.0.1, targeting `b_u585i_iot02a/stm32u585xx`. The polling echo is the default application, and the controller is selected with a configuration file and the backup SRAM overlay:

```sh
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-poll
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-controller -- -DEXTRA_CONF_FILE=controller.conf -DEXTRA_DTC_OVERLAY_FILE=boards/retention.overlay
```

The [firmware guide](firmware/stm32u585/README.md) lists the interrupt-driven echo, retention probe and reset-injection configurations.

The board's configured `west flash` runner may erase more memory than the image occupies. Do not flash without first reviewing the target, erase scope and preserved firmware backup; the recorded experiments used bounded incremental programming and read-back verification through STM32CubeProgrammer.

## Hardware results

All results below were recorded on one B-U585I-IOT02A board in November 2026, and their raw evidence is not part of this source set. The polling echo returned all nine fixed binary vectors exactly. The interrupt-driven echo passed the same vectors and seven 100,000-byte stream runs with matching SHA-256 hashes and no observed missing bytes. Its highest observed completed-echo rate was 11,499 B/s at 115200 baud; this is a host-observed end-to-end rate, not a direct UART line-utilization measurement.

The retention probe read back records in SRAM1 and in backup SRAM intact after each of 21 software, 21 independent-watchdog and 21 pin resets per memory. With the controller's reset-injection build, 600 trials compared three host recovery policies that do not read the journal, with a reset before the journal write or after the commit, and 500 trials reset journal-backed recovery at five points from before the journal write to after the commit. The naive retry policy duplicated the committed increment in all 100 of its post-commit trials, as expected for that baseline. A later build, which added the cold-initialization order, boot-time recovery from an inconsistent slot pair and the protocol-version check, repeated eight of these policy and reset-point cells in 110 trials. No trial classified a request `COMPLETED` or `NOT_EXECUTED` contrary to the retained counter. `host/client/verify_trials.py` recomputes these verdicts from the recorded retained snapshots.

A further 68 trials, on a third build that added the cold-initialization reset hooks, covered cold journal initialization and inconsistent slots. 56 forced a software reset at each of the four stores of cold initialization, 48 with the newest record in the first slot and 8 with it in the second, and 12 forged an inconsistent slot pair. Every request committed before an injected reset was classified `UNKNOWN`, and every trial passed. `host/client/verify_cold_init_trials.py` checks these records without the host code, from the raw received bytes where they were recorded, which excludes the first 10 trials.

On the third build, two supervised runs removed board power six times. An inspection before each run found solder bridge SB6 fitted, which the Zephyr documentation for this board describes as connecting VBAT to VDD, no coin cell and the ST-LINK USB connector as the only supply, and each removal lasted at least 60 seconds with every LED off. After each removal the board reported a brown-out reset and started cold, backup SRAM held neither committed record, every committed request classified `UNKNOWN` and a new session counted from 1, while pin resets before and after the removals kept the journal. No voltage was measured, and SRAM1 retention across power loss was not characterized.

The current controller image, which adds the rule for a corrupted committed slot, passed one complete run of 13 control-endpoint cases, 3 `PING` and 5 `INCREMENT` requests, one session rebind, 15 journal-backed reset trials at three reset points, 8 resets inside cold initialization, two at each store with the next boot judged cold and judged warm, and 2 forged inconsistent slot pairs, all with the newest record in the first slot. `host/client/verify_trials.py` and `host/client/verify_cold_init_trials.py` recompute the rebind, reset and cold-initialization verdicts, and the rule for a corrupted committed slot is tested natively and in the simulator only. Every reset-injection trial used software, watchdog or pin resets with the board powered, and the injected resets fell between word stores, not inside one. A finite campaign on one board bounds the tested cases and does not establish a failure rate.

## Status and licensing

EPOCH is experimental. The wire format values, the journal layout and the recovery rules are provisional and do not form a stable interface. The known gaps are listed under [limitations](docs/architecture.md#limitations).

Original EPOCH software, including firmware, is licensed under [Apache License 2.0](LICENSE). Original hardware design sources, if published, will be separately designated under [CERN Open Hardware Licence Version 2 - Permissive](https://cern-ohl.web.cern.ch/); no original RTL, schematic or PCB design is included in this source set. Third-party Zephyr, STM32 HAL/BSP and vendor materials retain their own applicable upstream licenses. See [licensing scope](docs/licensing.md).
