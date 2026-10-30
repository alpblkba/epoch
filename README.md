# EPOCH

EPOCH is an in-progress host-controller IPC and fault-injection research project.

## HW-01: STM32U585 bring-up

Completed on 30 October 2026.

- Hardware: B-U585I-IOT02A, STM32U585 Cortex-M33.
- Zephyr: v4.4.2, commit dccb09599635bdff17633fa7e9dab014b91dce90.
- Toolchain: Zephyr SDK 1.0.1, West 1.5.0.
- Successfully built the Zephyr Hello World sample.
- Programmed the board with STM32CubeProgrammer using incremental flashing.
- Verified the programming operation and observed Hello World over UART after reset.
- Preserved a pre-flash internal memory backup outside the repository.

The EPOCH IPC runtime, reset recovery experiments and fault-injection infrastructure are not yet implemented.
