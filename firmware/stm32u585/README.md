# STM32U585 firmware

This Zephyr project targets the ST B-U585I-IOT02A board with STM32U585 and uses the ST-LINK virtual COM port on USART1 at 115200 baud, 8N1 and no hardware flow control. The default application echoes incoming bytes through `uart_poll_in()` and `uart_poll_out()`; `irq.conf` enables interrupt-driven reception through the portable C11 ring while a Zephyr thread performs transmission.

Use Zephyr v4.4.2 and Zephyr SDK 1.0.1 with the board qualifier `b_u585i_iot02a/stm32u585xx`:

```sh
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-poll
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-irq -- -DEXTRA_CONF_FILE=irq.conf
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-probe -- -DEXTRA_CONF_FILE=retention.conf -DEXTRA_DTC_OVERLAY_FILE=boards/retention.overlay
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-controller -- -DEXTRA_CONF_FILE=controller.conf -DEXTRA_DTC_OVERLAY_FILE=boards/retention.overlay
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-controller-fault -- "-DEXTRA_CONF_FILE=controller.conf;fault.conf" -DEXTRA_DTC_OVERLAY_FILE=boards/retention.overlay
```

`retention.conf` builds a probe that reports reset flags and two test records, one in SRAM1 and one in backup SRAM, as text lines. `controller.conf` builds the controller, which keeps its journal in backup SRAM and draws boot identifiers, sessions and eras from the hardware RNG. Adding `fault.conf` enables a test-only endpoint that arms software resets at named checkpoints and forges inconsistent slot pairs. Use that build only for experiments, because a host can reset the board and overwrite its retained state through it. Adding `corrupt.conf` as well (`controller.conf;fault.conf;corrupt.conf`) builds the corrupted-slot image: the second forge mode then flips one bit of the counter word of the newest valid slot in place, so its CRC fails while its first word still holds the magic, and the next boot must report retention invalid and start a new era. The standard fault build is unchanged by this option.

Every configuration disables Zephyr console output and logging, so USART1 carries only the application's own bytes. Before programming a board, verify the HEX address ranges, the original firmware backup, flash erase scope and target identity. An ordinary `west flash` command is not an approved substitute for a reviewed incremental programming procedure.
