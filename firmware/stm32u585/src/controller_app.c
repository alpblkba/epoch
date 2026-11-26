/* EPOCH controller, session control (EP0) and retained transactions (EP1): the portable EPOCH controller (core/src/controller.c) behind the UART wire codec, with its journal in backup SRAM (R2, qualified by the retention probe for software, IWDG and pin resets).
 * Data path as in the interrupt-driven echo (echo_irq.c): the USART1 RX interrupt fills the EPOCH SPSC ring, and this thread drains it, parses frames, runs the controller and transmits with uart_poll_out. */

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/reboot.h>

#include "epoch/controller.h"
#include "epoch/crc32.h"
#include "epoch/ring.h"
#include "epoch/wire.h"
#ifdef CONFIG_EPOCH_FAULT_TEST
#include "fault_hooks.h"
#endif

#define UART_NODE DT_CHOSEN(zephyr_console)
#define RX_CAPACITY 256u
#define TICK_MS 10
#define JOURNAL_MAGIC_VERSION 0x45504301u /* MAGIC 0x455043 ("EPC"), VERSION 1; provisional */
#define R2_ERA_MARKER 0xE9A0C0DEu         /* TAMP_BKP30R: cold initialization of the R2 journal completed since the last backup-domain reset */

static const struct device *const uart = DEVICE_DT_GET(UART_NODE);
static const struct device *const rng = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));

static __stm32_backup_sram_section struct epoch_retained retained;

static struct epoch_ring rx;
static uint8_t rx_buf[RX_CAPACITY];
static K_SEM_DEFINE(rx_sem, 0, 1);

/* Debugger-only counters, one writer each, no firmware reader. */
struct ctrl_stats {
    uint32_t ring_drops;   /* ISR */
    uint32_t overruns;     /* ISR, lower bound: uart_fifo_read clears an overrun latched after the sample */
    uint32_t wire_errors;  /* thread: frames rejected by the codec */
    uint32_t rng_errors;   /* thread */
};
volatile struct ctrl_stats ctrl_stats;

static void rx_isr(const struct device *dev, void *user_data)
{
    uint8_t byte;
    bool queued = false;

    ARG_UNUSED(user_data);
    uart_irq_update(dev);
    if (uart_err_check(dev) & UART_ERROR_OVERRUN) {
        ctrl_stats.overruns++;
    }
    while (uart_irq_rx_ready(dev) > 0 && uart_fifo_read(dev, &byte, 1) == 1) {
        if (epoch_ring_put(&rx, byte)) {
            queued = true;
        } else {
            ctrl_stats.ring_drops++;
        }
    }
    if (queued) {
        k_sem_give(&rx_sem);
    }
}

static uint64_t rand64(void *ctx)
{
    ARG_UNUSED(ctx);
    uint64_t v = 0;
    /* The STM32 RNG driver reports seed and clock errors; a failure is counted and retried rather than replaced by a constant. */
    while (entropy_get_entropy(rng, (uint8_t *)&v, sizeof(v)) != 0) {
        ctrl_stats.rng_errors++;
        k_msleep(1);
    }
    return v;
}

static uint32_t new_era(void *ctx)
{
    uint32_t e;
    do {
        e = (uint32_t)rand64(ctx);
    } while (e == 0);
    return e;
}

static void send(void *ctx, const struct epoch_msg *m)
{
    ARG_UNUSED(ctx);
    uint8_t frame[EPOCH_WIRE_MAX_FRAME];
    size_t n = epoch_wire_encode(m, frame, sizeof(frame));
    for (size_t i = 0; i < n; i++) {
        uart_poll_out(uart, frame[i]);
    }
}

static void barrier(void *ctx)
{
    ARG_UNUSED(ctx);
    __DSB();
}

static const struct epoch_ctrl_config cfg = {
    .journal = {JOURNAL_MAGIC_VERSION, epoch_crc32_words},
    .t_pending_ms = 2000,
    .version_min = EPOCH_WIRE_VERSION,
    .version_max = EPOCH_WIRE_VERSION,
    .capabilities = 0,
};
static uint32_t boot_csr;

#ifdef CONFIG_EPOCH_FAULT_TEST
/* Reset injection: a test-only endpoint arms one reset at (checkpoint, txid). Just before the reset, the "checkpoint reached" marker goes to R1 (SRAM1 .noinit, qualified by the retention probe), the store not under test, so a journal fault cannot hide or forge it. Codes 9 to 12 arm a reset at a cold-initialization store of the next boot, kept in R1 across the preparatory reset; code 13 forges an inconsistent slot pair. */
static __noinit struct fault_record marker, arm_record;
static struct {
    bool armed;
    uint32_t run, checkpoint, txid;
} arm;
static struct fault_cold cold_arm;

static bool checkpoint(void *ctx, enum epoch_checkpoint cp, uint32_t txid)
{
    ARG_UNUSED(ctx);
    if (!arm.armed || (uint32_t)cp != arm.checkpoint || txid != arm.txid) {
        return false;
    }
    arm.armed = false;
    fault_record_write(&marker, FAULT_MARKER_MAGIC, arm.run, arm.checkpoint, txid, barrier, NULL);
    return true; /* the controller stops here and main resets */
}

/* uart_poll_out returns once a byte is in TDR, so with the USART1 FIFO disabled (no fifo-enable in the devicetree) two bytes, about 0.17 ms at 115200 baud, are still on their way, and a reset cuts them. Before a test reset the firmware therefore waits for USART_ISR.TC, which the hardware sets when TDR and the shift register are empty after a stop bit; the driver's uart_irq_tx_complete reads the same flag but is defined only inside the UART interrupt. */
#define TX_LIMIT_US 1000u
static void tx_put(void *ctx, uint8_t byte)
{
    ARG_UNUSED(ctx);
    uart_poll_out(uart, byte);
}
static bool tx_sent(void *ctx)
{
    ARG_UNUSED(ctx);
    return (((USART_TypeDef *)DT_REG_ADDR(UART_NODE))->ISR & USART_ISR_TC) != 0u;
}
static void tx_wait(void *ctx, uint32_t us)
{
    ARG_UNUSED(ctx);
    k_busy_wait(us);
}
static const struct fault_tx tx = {NULL, tx_put, tx_sent, tx_wait};

/* Sends the last reply completely, then resets. */
static FUNC_NORETURN void reply_and_reset(const struct epoch_msg *r)
{
    (void)fault_send_complete(&tx, r, TX_LIMIT_US);
    sys_reboot(SYS_REBOOT_COLD);
}

static bool cold_store_hook(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(slot);
    if (!fault_cold_hit(&cold_arm, index)) {
        return false;
    }
    if (cold_arm.arm.txid == FAULT_NEXT_WARM) {
        TAMP->BKP30R = R2_ERA_MARKER; /* the next boot is judged warm, so only the core's era guard protects it */
    }
    fault_record_write(&marker, FAULT_MARKER_MAGIC, cold_arm.arm.run, index, cold_arm.arm.txid, barrier, NULL);
    return true; /* epoch_ctrl_boot returns EPOCH_CTRL_RESET and main resets */
}

/* Runs before epoch_ctrl_boot: takes and clears an armed cold-initialization reset, and reports it with an unsolicited TEST_DIAG_REPLY carrying this boot's reset cause, which identifies the preparatory reset. */
static void fault_boot(uint32_t csr)
{
    fault_cold_take(&arm_record, &cold_arm, barrier, NULL);
    if (cold_arm.armed) {
        struct epoch_msg r = {0};
        r.type = EPOCH_MSG_TEST_DIAG_REPLY;
        r.run = cold_arm.arm.run;
        r.checkpoint = cold_arm.arm.checkpoint;
        r.txid = cold_arm.arm.txid;
        r.reset_cause = csr;
        (void)fault_send_complete(&tx, &r, TX_LIMIT_US); /* on a timeout the host sees a damaged frame and the trial stays INCONCLUSIVE */
        RCC->CSR |= RCC_CSR_RMVF; /* so the boot after the injected reset reports only that reset */
    }
}

/* Handles the test messages before the controller sees them; returns true when m was one. */
static bool fault_test(const struct epoch_msg *m)
{
    struct epoch_msg r = {0};
    if (m->type == EPOCH_MSG_TEST_ARM) {
        enum fault_request q = fault_decode(m->checkpoint, m->txid);
        if (q == FAULT_IGNORE) {
            return true;
        }
        marker.magic = 0; /* a marker from an earlier trial can never match this one */
        __DSB();
        r = *m;
        r.type = EPOCH_MSG_TEST_ARM_ACK;
        if (q == FAULT_COLD) {
            fault_record_write(&arm_record, FAULT_ARM_MAGIC, m->run, m->checkpoint, m->txid, barrier, NULL);
            TAMP->BKP30R = 0; /* the next boot is judged cold and runs cold initialization, where the arm fires */
            __DSB();
            reply_and_reset(&r); /* preparatory reset */
        }
        if (q == FAULT_FORGE) {
            if (!fault_forge_pair(&cfg.journal, retained.slots, retained.era, m->txid, barrier, NULL)) {
                return true; /* no valid record to copy: nothing forged, no acknowledgement */
            }
            fault_record_write(&marker, FAULT_MARKER_MAGIC, m->run, m->checkpoint, m->txid, barrier, NULL);
            reply_and_reset(&r); /* warm reset: TAMP_BKP30R stays, so the core alone meets the pair at boot */
        }
        arm.armed = true;
        arm.run = m->run;
        arm.checkpoint = m->checkpoint;
        arm.txid = m->txid;
    } else if (m->type == EPOCH_MSG_TEST_DIAG) {
        r.type = EPOCH_MSG_TEST_DIAG_REPLY;
        r.marker_valid = fault_record_valid(&marker, FAULT_MARKER_MAGIC);
        r.run = marker.run;
        r.checkpoint = marker.checkpoint;
        r.txid = marker.txid;
        r.reset_cause = boot_csr;
    } else {
        return m->type >= EPOCH_MSG_TEST_ARM; /* a reply type: ignored */
    }
    send(NULL, &r);
    return true;
}
#else
#define checkpoint NULL
#define cold_store_hook NULL
static inline void fault_boot(uint32_t csr)
{
    ARG_UNUSED(csr);
}
static bool fault_test(const struct epoch_msg *m)
{
    ARG_UNUSED(m);
    return false;
}
#endif

static const struct epoch_ctrl_env env = {NULL, rand64, new_era, send, barrier, checkpoint, cold_store_hook};
static struct epoch_ctrl ctrl;
static struct epoch_wire_parser parser;

int main(void)
{
    const uint32_t csr = boot_csr = RCC->CSR; /* first, before anything can change it; reported raw in ANNOUNCE */

    if (!device_is_ready(uart) || !device_is_ready(rng) || !epoch_ring_init(&rx, rx_buf, RX_CAPACITY) ||
        uart_irq_callback_user_data_set(uart, rx_isr, NULL) != 0) {
        return -1;
    }
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN; /* TAMP registers; backup-domain write access is enabled by the backup SRAM driver */
    (void)RCC->APB3ENR;

    /* Cold when power was lost (BOR covers power-on) or when cold initialization never completed since the last backup-domain reset: the marker is written only after epoch_ctrl_boot returns, so a reset inside cold initialization repeats it. */
    const bool cold = (csr & RCC_CSR_BORRSTF) != 0u || TAMP->BKP30R != R2_ERA_MARKER;
    if (cold) {
        /* Withdraw the marker before cold initialization touches a slot, so a reset anywhere inside it leads to cold initialization again on the next boot, whatever the reset flags say. */
        TAMP->BKP30R = 0;
        __DSB();
    }
    epoch_wire_parser_init(&parser);
    uart_irq_rx_enable(uart); /* bytes arriving from here on wait in the ring, so a PROBE answering ANNOUNCE is not lost */
    fault_boot(csr);
    if (epoch_ctrl_boot(&ctrl, &cfg, &env, &retained, cold, csr, k_uptime_get_32()) == EPOCH_CTRL_RESET && IS_ENABLED(CONFIG_EPOCH_FAULT_TEST)) {
        sys_reboot(SYS_REBOOT_COLD); /* a test hook stopped cold initialization: no ANNOUNCE was sent, and neither the completed-boot marker nor the flag clearing may follow */
    }
    TAMP->BKP30R = R2_ERA_MARKER;
    __DSB();
    RCC->CSR |= RCC_CSR_RMVF; /* after boot used them, so the next ANNOUNCE reports only its own reset */

    for (;;) {
        k_sem_take(&rx_sem, K_MSEC(TICK_MS));
        uint8_t byte;
        while (epoch_ring_get(&rx, &byte)) {
            struct epoch_msg m;
            enum epoch_wire_status s = epoch_wire_feed(&parser, byte, &m);
            if (s == EPOCH_WIRE_OK) {
                if (!fault_test(&m) && epoch_ctrl_receive(&ctrl, &m, k_uptime_get_32()) == EPOCH_CTRL_RESET) {
                    sys_reboot(SYS_REBOOT_COLD); /* NVIC_SystemReset, after a DSB */
                }
            } else if (s != EPOCH_WIRE_NONE) {
                ctrl_stats.wire_errors++;
            }
        }
        epoch_ctrl_tick(&ctrl, k_uptime_get_32());
    }
}
