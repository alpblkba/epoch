# EPOCH

EPOCH investigates fault-tolerant host-controller IPC when a microcontroller can reset or lose a reply during a non-idempotent operation. It combines session-scoped requests, retained transaction records and recovery policies to distinguish confirmed execution from outcomes that remain unknown under a defined failure model.

The project uses a portable C11 core, a Zephyr-based STM32U585 target and deterministic host-side experiments. The initial public source set contains the validated UART transport and its supporting primitives; the wider IPC implementation is still being prepared for publication.

## Components

The public implementation includes a lock-free single-producer single-consumer byte ring, CRC-32, polling and interrupt-driven UART echo firmware, and a Python transport test harness. Session management, retained journaling and reset-recovery experiments are part of the broader development system, but are not included in this initial public source set.

See [the architecture overview](docs/architecture.md) and the short implementation guides in [core](core/README.md), [host](host/README.md) and [STM32 firmware](firmware/stm32u585/README.md).

## Building and testing

The public host build requires CMake 3.20, a C11 compiler and POSIX threads:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This default build runs the ring and CRC-32 tests. A separate `EPOCH_BUILD_EXPERIMENTAL=ON` option enables the wider native IPC suite when its additional development sources are present; it is not supported by the initial public source set.

The UART harness needs Python 3; physical serial testing also requires pySerial. The offline harness tests do not need a board:

```sh
cd host/client
python3 -m unittest test_uart_echo
```

The echo firmware uses Zephyr v4.4.2 (`dccb09599635bdff17633fa7e9dab014b91dce90`) and Zephyr SDK 1.0.1, targeting `b_u585i_iot02a/stm32u585xx`. The polling application is the default; `irq.conf` selects interrupt-driven reception:

```sh
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-poll
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-irq -- -DEXTRA_CONF_FILE=irq.conf
```

The board's configured `west flash` runner may erase more memory than the image occupies. Do not flash without first reviewing the target, erase scope and preserved firmware backup; the recorded experiments used bounded incremental programming and read-back verification through STM32CubeProgrammer.

## Hardware results

All results below were recorded on one B-U585I-IOT02A board in November 2026. The polling echo returned all nine fixed binary vectors exactly. The interrupt-driven echo passed the same vectors and seven 100,000-byte stream runs with matching SHA-256 hashes and no observed missing bytes. Its highest observed completed-echo rate was 11,499 B/s at 115200 baud; this is a host-observed end-to-end rate, not a direct UART line-utilization measurement.

Additional development-firmware experiments, not included in this public source set, reported successful retention checks for both SRAM1 and backup SRAM across 21 software, 21 watchdog and 21 pin resets per backend. Cold power-loss retention was not tested. The development campaign also reported a 600-trial recovery-policy comparison and 500 journal-interruption trials. Those reports are bounded by the tested software-reset checkpoints; their raw evidence and current campaign acceptance logic require separate review before a general correctness claim.

## Status and licensing

EPOCH is experimental and its complete IPC implementation and wire protocol are not yet published as a stable interface. The public UART transport, CRC-32 and ring primitives are available for review and reuse.

Original EPOCH software, including firmware, is licensed under [Apache License 2.0](LICENSE). Original hardware design sources, if published, will be separately designated under [CERN Open Hardware Licence Version 2 - Permissive](https://cern-ohl.web.cern.ch/); no original RTL, schematic or PCB design is included in this source set. Third-party Zephyr, STM32 HAL/BSP and vendor materials retain their own applicable upstream licenses. See [licensing scope](docs/licensing.md).
