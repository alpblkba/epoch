#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#define UART_NODE DT_CHOSEN(zephyr_console)

int main(void)
{
    const struct device *uart = DEVICE_DT_GET(UART_NODE);
    unsigned char byte;

    if (!device_is_ready(uart)) {
        return -1;
    }

    while (1) {
        if (uart_poll_in(uart, &byte) == 0) {
            uart_poll_out(uart, byte);
        }
    }

    return 0;
}