# STM32U585 UART firmware

This Zephyr application targets the ST B-U585I-IOT02A board with STM32U585 and uses the ST-LINK virtual COM port on USART1 at 115200 baud, 8N1 and no hardware flow control. The default firmware echoes incoming bytes through `uart_poll_in()` and `uart_poll_out()`; `irq.conf` enables interrupt-driven reception through the portable C11 ring while a Zephyr thread performs transmission.

Use Zephyr v4.4.2 and Zephyr SDK 1.0.1 with the board qualifier `b_u585i_iot02a/stm32u585xx`:

```sh
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-poll
west build -b b_u585i_iot02a/stm32u585xx firmware/stm32u585 -d build-uart-irq -- -DEXTRA_CONF_FILE=irq.conf
```

The application disables Zephyr console output and logging to keep the binary echo channel clean. Before programming a board, verify the HEX address ranges, the original firmware backup, flash erase scope and target identity. An ordinary `west flash` command is not an approved substitute for a reviewed incremental programming procedure.

Retention probes, session-enabled firmware and reset-injection variants remain outside this initial public source set.
