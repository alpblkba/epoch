# Architecture

EPOCH investigates request and outcome handling across controller resets. The development architecture separates a portable C11 protocol core from the host transport, an embedded platform adapter and deterministic fault-injection tools. This document describes the implementation in this source set and the failure model its tests and experiments cover.

## System boundary

The host sends framed commands over a byte transport to a microcontroller. The controller can lose volatile state, reboot before a reply is delivered or retain a committed effect after the host times out. The protocol must not infer that a request failed merely because its response was lost.

The control endpoint (EP0) negotiates boot and session identity. The transaction endpoint (EP1) handles non-idempotent effects, initially a retained counter increment. A request carries a session identifier and transaction identifier (`txid`); a controller reboot invalidates the former session. The host may classify an unresolved transaction as `COMPLETED`, `NOT_EXECUTED` or `UNKNOWN` only when the available evidence supports that result.

## Transport and concurrency

The STM32U585 UART receive interrupt is the producer of the portable single-producer single-consumer ring. A Zephyr thread is the consumer and performs UART transmission outside interrupt context. The ring uses C11 atomic indices and acquire/release ordering; its debugger-visible counters are not a substitute for protocol-level reliability.

The development wire format uses explicit little-endian fields, a CRC-32/ISO-HDLC checksum and COBS framing with zero delimiters. Its numeric values and negotiation details remain provisional, so it is not a stable wire API.

## Sessions and retained effects

A session distinguishes messages from the current controller boot from stale requests or replies. A pending negotiation can coexist with an active session. The transaction fence rejects older or duplicate identifiers without executing an additional effect in the same session.

The retained journal uses two eight-word slots in backup SRAM, each holding a validity marker, the journal era, the session, `txid`, counter, sequence number and a CRC-32. Publication invalidates the older slot, writes the record and commits it by writing the validity marker last, with memory barriers between the steps. A slot counts only when its marker, era and CRC are valid, and of two valid slots the newer is the one whose sequence number is exactly one above the other's. Any other pair of valid slots is treated as corrupted retention. Publication and cold initialization keep a slot's marker at 0 until the slot is complete, so a slot that fails validation while its marker is not 0 was changed after it committed and is also treated as corrupted retention.

At boot the controller copies the slots as a snapshot for the host, then treats retention as invalid after a brown-out or power-on reset, after an incomplete earlier initialization, or when it finds corrupted retention. It then runs cold initialization: it sets the era to 0, invalidates both slots and writes a fresh random era last, so a reset at any of these stores leaves no slot readable under the old era. The host classifies a request `COMPLETED` only when the newest record of its session carries its `txid`, `NOT_EXECUTED` only when that record carries a lower `txid`, and `UNKNOWN` in every other case, including invalid retention.

## Simulation and fault injection

A deterministic simulator models two lossy links, message loss, duplication, delay, reordering and named controller reset checkpoints. Experimental host policies compare naive retries, session-aware uncertainty, reboot-aware recovery and journal-backed classification. On the board, a checkpoint marker in SRAM1 and the change of the retained counter form an oracle independent of the host's label. The reset-experiment records are checked again by verifiers that decode the raw snapshots and frames themselves.

## Public source organization

- [`core/`](../core/README.md) contains the portable protocol core and its primitives.
- [`host/`](../host/README.md) contains the simulator, the C host client and the Python experiment and verification tools.
- [`firmware/stm32u585/`](../firmware/stm32u585/README.md) contains the echo, retention probe and controller applications.
- [`tests/`](../tests/README.md) contains the native tests.
- [`scripts/`](../scripts/README.md) contains the public terminology check.

## Limitations

- Cold power loss was tested by removing the only supply of one board six times, without a voltage measurement and without characterizing SRAM1. Every other hardware result comes from software, watchdog or pin resets with the board powered, and injected resets fell between word stores rather than inside one.
- A brown-out or power-on reset always starts cold, so outcomes that were committed before a power loss classify `UNKNOWN` even if backup SRAM kept its content.
- The failure model excludes silent memory corruption. A corruption that sets the marker of a committed slot to exactly 0 looks like an interrupted publication, so its request could classify `NOT_EXECUTED` while the older slot is still valid, and a corruption of several words that keeps a valid CRC is not detected.
- Corrupted retention found during a session blocks transaction requests until the next boot.
- The rule for a corrupted committed slot is tested natively and in the simulator only. No hardware trial corrupted a committed slot, and the recovery evidence covers interrupted publication, resets inside cold initialization and forged inconsistent pairs, not a corrupted slot.
- The control endpoint does not report UART receive overruns or ring drops.
- The journal era is a random nonzero 32-bit value that is not checked against the previous era. Cold initialization invalidates both slots before it writes the new era, so a repeated era cannot revive an old record.
- The hardware results come from one board and a finite number of trials per case.
