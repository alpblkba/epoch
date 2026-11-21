/* Interrupt-driven UART echo. The USART1 receive interrupt is the only producer of the RX ring and main() is the only consumer, which sends each byte back with uart_poll_out. Built when CONFIG_UART_INTERRUPT_DRIVEN=y (irq.conf); src/main.c remains the polling echo. */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define UART_NODE DT_CHOSEN(zephyr_console)
#define RX_CAPACITY 256u /* power of two; 22 ms of input at 115200 baud 8N1 */

/* Debugger-only instrumentation, not synchronization. Each field has one writer and no firmware reader, so no atomic read-modify-write is needed; volatile forbids the compiler from dropping or merging the stores. A debugger reads each aligned word in one access. The fields are zeroed at every boot (.bss) and wrap modulo 2^32. */
struct echo_stats {
    uint32_t rx_bytes;    /* ISR: bytes queued */
    uint32_t ring_drops;  /* ISR: bytes lost because the ring was full; exact */
    uint32_t overruns;    /* ISR: interrupts that found the USART overrun flag; a lower bound, see rx_isr */
    uint32_t line_errors; /* ISR: interrupts that found a framing, noise, parity or break flag; a lower bound */
    uint32_t tx_bytes;    /* thread: bytes handed to uart_poll_out */
};
volatile struct echo_stats echo_stats;

#ifdef CONFIG_EPOCH_RX_ZEPHYR_RING_BUF
#include <zephyr/sys/ring_buffer.h>

/* Zephyr ring_buf comparator, used only to isolate a suspected EPOCH ring defect. */
RING_BUF_DECLARE(rx_rb, RX_CAPACITY);

static bool rx_init(void)
{
    return true;
}

static bool rx_put(uint8_t byte)
{
    return ring_buf_put(&rx_rb, &byte, 1) == 1u;
}

static bool rx_get(uint8_t *byte)
{
    return ring_buf_get(&rx_rb, byte, 1) == 1u;
}
#else
#include "epoch/ring.h"

static struct epoch_ring rx;
static uint8_t rx_buf[RX_CAPACITY];

static bool rx_init(void)
{
    return epoch_ring_init(&rx, rx_buf, RX_CAPACITY);
}

static bool rx_put(uint8_t byte)
{
    return epoch_ring_put(&rx, byte);
}

static bool rx_get(uint8_t *byte)
{
    return epoch_ring_get(&rx, byte);
}
#endif

static K_SEM_DEFINE(rx_sem, 0, 1);

static void rx_isr(const struct device *dev, void *user_data)
{
    uint8_t byte;
    bool queued = false;

    ARG_UNUSED(user_data);
    uart_irq_update(dev);
    /* uart_fifo_read clears the overrun flag without reporting it, so the flags are sampled first. The count is a lower bound: an overrun latched after this sample is cleared silently by uart_fifo_read, and one flag can stand for several lost bytes. */
    int err = uart_err_check(dev);
    if (err > 0) {
        if (err & UART_ERROR_OVERRUN)
            echo_stats.overruns++;
        if (err & ~UART_ERROR_OVERRUN)
            echo_stats.line_errors++;
    }
    /* Each pass consumes one received byte, and the next can arrive only one character time (87 us) later, so the loop ends as soon as the line pauses. */
    while (uart_irq_rx_ready(dev) > 0 && uart_fifo_read(dev, &byte, 1) == 1) {
        if (rx_put(byte)) {
            echo_stats.rx_bytes++;
            queued = true;
        } else {
            echo_stats.ring_drops++;
        }
    }
    if (queued)
        k_sem_give(&rx_sem);
}

int main(void)
{
    const struct device *uart = DEVICE_DT_GET(UART_NODE);
    uint8_t byte;

    /* The ring and the statically defined semaphore are ready before the callback is installed and RX interrupts are enabled. */
    if (!device_is_ready(uart) || !rx_init() || uart_irq_callback_user_data_set(uart, rx_isr, NULL) != 0)
        return -1;
    uart_irq_rx_enable(uart);

    for (;;) {
        k_sem_take(&rx_sem, K_FOREVER);
        /* One token can stand for many bytes. A byte queued after the drain empties the ring also gives a token, so the next take returns at once and nothing is stranded. */
        while (rx_get(&byte)) {
            uart_poll_out(uart, byte);
            echo_stats.tx_bytes++;
        }
    }
}
