# Architecture

EPOCH investigates request and outcome handling across controller resets. The development architecture separates a portable C11 protocol core from the host transport, an embedded platform adapter and deterministic fault-injection tools. This document describes the design under investigation; the initial public source set currently contains the transport primitives and UART echo variants rather than the full IPC implementation.

## System boundary

The host sends framed commands over a byte transport to a microcontroller. The controller can lose volatile state, reboot before a reply is delivered or retain a committed effect after the host times out. The protocol must not infer that a request failed merely because its response was lost.

The control endpoint (EP0) negotiates boot and session identity. The transaction endpoint (EP1) handles non-idempotent effects, initially a retained counter increment. A request carries a session identifier and transaction identifier (`txid`); a controller reboot invalidates the former session. The host may classify an unresolved transaction as `COMPLETED`, `NOT_EXECUTED` or `UNKNOWN` only when the available evidence supports that result.

## Transport and concurrency

The STM32U585 UART receive interrupt is the producer of the portable single-producer single-consumer ring. A Zephyr thread is the consumer and performs UART transmission outside interrupt context. The ring uses C11 atomic indices and acquire/release ordering; its debugger-visible counters are not a substitute for protocol-level reliability.

The development wire format uses explicit little-endian fields, a CRC-32/ISO-HDLC checksum and COBS framing with zero delimiters. Its numeric values and negotiation details remain provisional, so the current public source set does not expose it as a stable wire API.

## Sessions and retained effects

A session distinguishes messages from the current controller boot from stale requests or replies. A pending negotiation can coexist with an active session. The transaction fence rejects older or duplicate identifiers without executing an additional effect in the same session.

The retained journal uses two eight-word slots. Publication invalidates a target slot, writes the record and commits it by writing the validity marker last, with target-specific memory barriers. Recovery examines the slot contents and their sequence relationship. Correct outcome classification depends on reset type, retained-memory behavior and the ability to distinguish cold initialization from an interrupted publication. Cold-start and inconsistent-slot edge cases remain part of the ongoing correctness work.

## Simulation and fault injection

A deterministic simulator models two lossy links, message loss, duplication, delay, reordering and named controller reset checkpoints. Experimental host policies compare naive retries, session-aware uncertainty, reboot-aware recovery and journal-backed classification. A separate oracle should determine the actual retained effect independently of the host's outcome label; the physical experiment runner and its recorded acceptance predicates remain subject to further review.

## Public source organization

- [`core/`](../core/README.md) contains the currently published portable ring and CRC-32 primitives.
- [`host/`](../host/README.md) contains the currently published UART testing tools.
- [`firmware/stm32u585/`](../firmware/stm32u585/README.md) contains the polling and interrupt-driven echo applications.
- [`tests/`](../tests/README.md) contains native tests for published primitives.
- [`scripts/`](../scripts/README.md) contains the public terminology check.

These modules are an initial reproducible source subset. The uncommitted session, journal and fault-injection implementation is not a public dependency of this subset.
