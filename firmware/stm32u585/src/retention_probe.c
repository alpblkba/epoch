/* Retained-memory probe. Reports reset flags, FLASH_OPTR and two probe records, R1 in SRAM1 noinit and R2 in backup SRAM, as text lines on USART1.
 * A boot only observes: records are written solely by the host's I command, so a valid record after a reset was written before it.
 * Commands, one per line: "I <run> <gen>" writes both records, "R" reports again, "B" repeats the boot report (for a host that reconnects after power-up), "S" software reset, "W" independent watchdog reset. A pin reset is driven by the ST-LINK. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/reboot.h>

#include "epoch/crc32.h"

#define UART_NODE DT_CHOSEN(zephyr_console)
#define PROBE_WORDS 12u
#define PROBE_VERSION 1u
#define R1_MAGIC 0x45503152u /* "R1PE" little-endian */
#define R2_MAGIC 0x45503252u
#define R2_MARKER 0xB2C0FFEEu /* TAMP_BKP31R: backup SRAM probe words were written since the last backup-domain reset */

/* w0 magic, w1 version, w2 backend, w3 run, w4 generation, w5..w8 fixed patterns, w9 ~generation, w10 run ^ generation ^ 0xA5A5A5A5, w11 CRC-32 of w0..w10. */
static const uint32_t patterns[4] = {0x00000000u, 0xFFFFFFFFu, 0xAAAAAAAAu, 0x55555555u};

static __noinit uint32_t r1_words[PROBE_WORDS];                  /* R1: SRAM1, not cleared by startup */
static __stm32_backup_sram_section uint32_t r2_words[PROBE_WORDS]; /* R2: backup SRAM */

static const struct device *const uart = DEVICE_DT_GET(UART_NODE);

static void out(const char *s)
{
    while (*s) {
        uart_poll_out(uart, *s++);
    }
}

static void build(uint32_t w[PROBE_WORDS], uint32_t magic, uint32_t backend, uint32_t run, uint32_t gen)
{
    w[0] = magic;
    w[1] = PROBE_VERSION;
    w[2] = backend;
    w[3] = run;
    w[4] = gen;
    memcpy(&w[5], patterns, sizeof(patterns));
    w[9] = ~gen;
    w[10] = run ^ gen ^ 0xA5A5A5A5u;
    w[11] = epoch_crc32_words(w, PROBE_WORDS - 1u);
}

/* Aligned 32-bit stores through volatile, w0 last, so a torn write never shows the magic. */
static void store(volatile uint32_t *dst, const uint32_t w[PROBE_WORDS])
{
    dst[0] = 0;
    __DSB();
    for (unsigned i = 1; i < PROBE_WORDS; i++) {
        dst[i] = w[i];
    }
    __DSB();
    dst[0] = w[0];
    __DSB();
}

static const char *classify(const uint32_t w[PROBE_WORDS], uint32_t magic, uint32_t backend)
{
    uint32_t expect[PROBE_WORDS];
    unsigned zeros = 0;
    for (unsigned i = 0; i < PROBE_WORDS; i++) {
        zeros += w[i] == 0;
    }
    if (zeros == PROBE_WORDS) {
        return "ZERO";
    }
    if (w[0] != magic) {
        return "NO_MAGIC";
    }
    build(expect, magic, backend, w[3], w[4]);
    return memcmp(expect, w, sizeof(expect)) == 0 ? "VALID" : "CORRUPT";
}

static void report_record(const char *name, const volatile uint32_t *src, uint32_t magic, uint32_t backend)
{
    uint32_t w[PROBE_WORDS];
    char line[200];
    for (unsigned i = 0; i < PROBE_WORDS; i++) {
        w[i] = src[i];
    }
    snprintf(line, sizeof(line), "%s %s run=%lu gen=%lu raw=", name, classify(w, magic, backend), (unsigned long)w[3], (unsigned long)w[4]);
    out(line);
    for (unsigned i = 0; i < PROBE_WORDS; i++) {
        snprintf(line, sizeof(line), "%08lx%s", (unsigned long)w[i], i + 1 < PROBE_WORDS ? "," : "\r\n");
        out(line);
    }
}

static bool r2_readable(void)
{
    /* Unwritten words are only a hazard with BKPRAM ECC on (FLASH_OPTR.BKPRAM_ECC = 0 enables it). */
    return TAMP->BKP31R == R2_MARKER || (FLASH->OPTR & FLASH_OPTR_BKPRAM_ECC) != 0u;
}

static void report(void)
{
    report_record("R1", r1_words, R1_MAGIC, 1);
    if (r2_readable()) {
        report_record("R2", r2_words, R2_MAGIC, 2);
    } else {
        /* With BKPRAM ECC enabled, reading words never written since power-up may report ECC errors, so R2 is read only after this probe wrote it. */
        out("R2 NOT_READ marker absent and ECC enabled\r\n");
    }
}

static void watchdog_reset(void)
{
    IWDG->KR = 0xCCCCu; /* start; LSI 32 kHz */
    IWDG->KR = 0x5555u; /* unlock PR and RLR */
    IWDG->PR = 0;       /* divide by 4 */
    IWDG->RLR = 0xFFu;  /* 256 ticks, about 32 ms */
    while (IWDG->SR != 0u) {
    }
    IWDG->KR = 0xAAAAu;
    for (;;) {
    }
}

static size_t read_line(char *buf, size_t cap)
{
    size_t n = 0;
    unsigned char c;
    for (;;) {
        if (uart_poll_in(uart, &c) != 0) {
            continue; /* busy poll: USART1 has no RX FIFO here, so sleeping between polls overruns the next byte */
        }
        if (c == '\n' || c == '\r') {
            if (n) {
                buf[n] = '\0';
                return n;
            }
            continue;
        }
        if (n + 1 < cap) {
            buf[n++] = (char)c;
        }
    }
}

static void boot_report(uint32_t csr, uint32_t optr)
{
    char line[160];
    snprintf(line, sizeof(line), "BOOT probe=%u csr=%08lx optr=%08lx r1_addr=%08lx r2_addr=%08lx bkp31=%08lx\r\n", PROBE_VERSION,
             (unsigned long)csr, (unsigned long)optr, (unsigned long)(uintptr_t)r1_words, (unsigned long)(uintptr_t)r2_words,
             (unsigned long)TAMP->BKP31R);
    out(line);
    report();
    out("READY\r\n");
}

int main(void)
{
    /* Captured before anything else; nothing in this image clears RCC_CSR earlier (CONFIG_HWINFO is off). */
    const uint32_t csr = RCC->CSR;
    const uint32_t optr = FLASH->OPTR;
    RCC->CSR |= RCC_CSR_RMVF; /* flags accumulate otherwise; the next boot then sees only its own reset */

    if (!device_is_ready(uart)) {
        return -1;
    }
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN; /* TAMP registers */
    (void)RCC->APB3ENR;

    boot_report(csr, optr);

    for (;;) {
        char cmd[64];
        read_line(cmd, sizeof(cmd));
        if (cmd[0] == 'I' && cmd[1] == ' ') {
            char *end;
            unsigned long run = strtoul(cmd + 2, &end, 10);
            unsigned long gen = strtoul(end, NULL, 10);
            uint32_t w[PROBE_WORDS];
            build(w, R1_MAGIC, 1, (uint32_t)run, (uint32_t)gen);
            store(r1_words, w);
            build(w, R2_MAGIC, 2, (uint32_t)run, (uint32_t)gen);
            store(r2_words, w);
            TAMP->BKP31R = R2_MARKER; /* after the words, so the marker never vouches for unwritten ECC words */
            __DSB();
            out("OK I\r\n");
            report();
        } else if (strcmp(cmd, "B") == 0) {
            boot_report(csr, optr);
        } else if (strcmp(cmd, "R") == 0) {
            report();
        } else if (strcmp(cmd, "S") == 0) {
            out("OK S\r\n");
            k_msleep(5); /* let the last byte leave the shift register */
            sys_reboot(SYS_REBOOT_COLD);
        } else if (strcmp(cmd, "W") == 0) {
            out("OK W\r\n");
            k_msleep(5);
            watchdog_reset();
        } else {
            out("ERR\r\n");
        }
    }
}
