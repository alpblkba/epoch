#!/usr/bin/env python3
"""Byte-for-byte UART echo check for the polling and interrupt-driven echo firmware.

Without --length it sends the nine fixed vectors. With --length it streams a deterministic payload of that many bytes, draining the echo between paced chunks, and compares the whole stream; this is the correctness mode. With --length and --measure it writes and reads concurrently and reports timestamps measured on the host; this is the throughput mode, and its integrity check is the same. A read may return fewer bytes than asked, so the echo is accumulated until it is complete or no byte arrives for --timeout seconds. Exit status 0 means everything echoed exactly, 1 means an echo or I/O failure, 2 means the port could not be opened or the arguments were invalid.
"""

import argparse
import errno
import hashlib
import os
import sys
import threading
import time

PASS, NO_RESPONSE, SHORT, MISMATCH, EXTRA = "PASS", "NO_RESPONSE", "SHORT", "MISMATCH", "EXTRA"


def test_vectors():
    """Deterministic vectors: ASCII, boundary bytes, alternating bits, a mixed payload and all 256 values."""
    lcg, mixed = 0x1234, bytearray()
    for _ in range(64):
        lcg = (lcg * 1103515245 + 12345) & 0x7FFFFFFF
        mixed.append((lcg >> 16) & 0xFF)
    return [
        ("ascii", b"EPOCH UART echo 0123456789"),
        ("0x00", b"\x00"),
        ("0x01", b"\x01"),
        ("0x7F", b"\x7f"),
        ("0x80", b"\x80"),
        ("0xFF", b"\xff"),
        ("alternating", bytes([0x55, 0xAA] * 8)),
        ("mixed64", bytes(mixed)),
        ("all256", bytes(range(256))),
    ]


def stream_payload(length, seed=1):
    """All 256 values, runs of 0x00 and 0xFF, alternating 0x55/0xAA, then SHA-256 blocks of the seed; the same arguments give the same bytes on every Python."""
    out = bytearray(bytes(range(256)) + b"\x00" * 32 + b"\xff" * 32 + bytes([0x55, 0xAA]) * 32)
    block = 0
    while len(out) < length:
        out += hashlib.sha256(b"%d:%d" % (seed, block)).digest()
        block += 1
    return bytes(out[:length])


def compare(sent, received):
    """Returns (status, detail); the detail names the first differing offset and a window of expected and actual bytes."""
    if received == sent:
        return PASS, ""
    if not received:
        return NO_RESPONSE, f"0 of {len(sent)} bytes before timeout"
    n = min(len(sent), len(received))
    first = next((i for i in range(n) if sent[i] != received[i]), None)
    if first is not None:
        lo = max(0, first - 4)
        return MISMATCH, f"first difference at byte {first}: sent[{lo}:]={sent[lo:lo + 8].hex(' ')} got[{lo}:]={received[lo:lo + 8].hex(' ')}"
    if len(received) < len(sent):
        return SHORT, f"{len(received)} of {len(sent)} bytes before timeout, missing from byte {len(received)}: {sent[len(received):len(received) + 8].hex(' ')}"
    return EXTRA, f"{len(received) - len(sent)} extra bytes: {received[len(sent):len(sent) + 8].hex(' ')}"


def read_exact(port, n, timeout):
    """Reads until n bytes have arrived or no byte has arrived for timeout seconds."""
    buf, deadline = bytearray(), time.monotonic() + timeout
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        port.timeout = min(remaining, 0.1)
        got = port.read(n - len(buf))
        if got:
            buf += got
            deadline = time.monotonic() + timeout
    return bytes(buf)


def write_all(port, data):
    """Writes every byte even if the port accepts only part of a write; a write that accepts nothing is an I/O error. Returns when the OS has accepted the bytes, not when they are on the wire."""
    while data:
        n = port.write(data)
        if not n:
            raise OSError(errno.EIO, "serial write accepted no bytes")
        data = data[n:]


def run_vector(port, data, chunk, pace, timeout, quiet):
    """Sends data in chunks, reads the echo, then checks that nothing more arrives during the quiet window."""
    for i in range(0, len(data), chunk):
        write_all(port, data[i:i + chunk])
        port.flush()
        if pace:
            time.sleep(pace)
    got = read_exact(port, len(data), timeout)
    status, detail = compare(data, got)
    if status == PASS:
        time.sleep(quiet)
        extra = port.read(port.in_waiting) if port.in_waiting else b""
        if extra:
            status, detail = compare(data, got + extra)
    return status, detail


def run_stream(port, data, chunk, pace, timeout, quiet):
    """Correctness mode: sends a chunk, waits for the OS to drain it (flush), sleeps, then collects whatever echo has arrived, so the host's receive buffers never fill. The flush and sleep both stretch each step, so the offered rate is below chunk / pace and is not measured here. Returns (status, detail, received, seconds until the echo was complete or timed out)."""
    got, start = bytearray(), time.monotonic()
    for i in range(0, len(data), chunk):
        write_all(port, data[i:i + chunk])
        port.flush()
        if pace:
            time.sleep(pace)
        if port.in_waiting:
            got += port.read(port.in_waiting)
    got += read_exact(port, len(data) - len(got), timeout)
    elapsed = time.monotonic() - start
    time.sleep(quiet)
    if port.in_waiting:
        got += port.read(port.in_waiting)
    status, detail = compare(data, bytes(got))
    return status, detail, bytes(got), elapsed


READ_TIMEOUT = 0.05  # seconds each reader read() waits for data
READ_BLOCK_LIMIT = 1.0  # seconds one read() may block before the reader counts as stuck; independent of baud rate, chunk and idle timeout
EXTRA_KEEP = 8  # excess bytes kept for the EXTRA report; capture stops there


def run_measured(port, data, chunk, pace, timeout, quiet):
    """Throughput mode: a reader thread collects the echo while this thread writes, so neither direction waits on the other.

    Termination: the reader ends itself once writing has ended and either every byte has arrived and the line stayed quiet for quiet seconds, or no byte has arrived for timeout seconds. timeout is an idle limit, so a stream that keeps delivering bytes may take any total time. The calling thread waits for the reader with no total deadline, but watches a heartbeat the reader updates after every read; if one read blocks longer than READ_BLOCK_LIMIT, the reader is told to stop, its read is cancelled where the backend supports cancel_read, and the run fails with ETIMEDOUT. Once more bytes have arrived than were sent, the reader keeps at most EXTRA_KEEP of the excess, stops the writer and ends, so a device that never stops sending fails promptly as EXTRA instead of refreshing the idle clock forever.

    Ownership: the reader alone reads the port and owns the received bytes, receive timestamps and its error list until it has ended; this thread alone writes the port and owns the write counters, and reads the reader's state only after join. The stop event is set by either side on failure, the done event by this thread when writing ends, and the heartbeat is shared under a lock. A writer error is re-raised in preference to a reader error, and a reader that could not be stopped is added as a note, so cleanup never hides the original failure. Returns (status, detail, received, stats)."""
    got, rx, errors, limit = bytearray(), {}, [], len(data) + EXTRA_KEEP
    done, stop, lock = threading.Event(), threading.Event(), threading.Lock()
    port.timeout = READ_TIMEOUT  # set once here: reconfiguring the port while the reader runs could race with it
    t0 = time.monotonic()
    w = {"submitted": 0, "end": t0}
    beat = [t0]

    def reader():
        try:
            while not stop.is_set():
                b = port.read(min(port.in_waiting or 1, limit - len(got)))
                now = time.monotonic()
                with lock:
                    beat[0] = now
                if b:
                    rx.setdefault("first", now)
                    rx["last"] = now
                    got.extend(b[:limit - len(got)])  # a backend may return more than asked
                    if len(got) > len(data):
                        stop.set()  # EXTRA is certain; the writer stops too
                        return
                elif done.is_set() and now - max(rx.get("last", t0), w["end"]) > (quiet if len(got) >= len(data) else timeout):
                    return
        except Exception as exc:
            errors.append(exc)
            stop.set()  # tells the writer to stop sending

    t = threading.Thread(target=reader, name="echo-reader", daemon=True)
    t.start()
    writer_error = None
    try:
        for i in range(0, len(data), chunk):
            if stop.is_set():
                break
            write_all(port, data[i:i + chunk])
            w["submitted"] += len(data[i:i + chunk])
            if pace:
                time.sleep(pace)
        w["end"] = time.monotonic()
    except Exception as exc:
        writer_error = exc
        stop.set()
    done.set()
    stalled = False
    while t.is_alive():
        t.join(READ_TIMEOUT)
        with lock:
            stalled = time.monotonic() - beat[0] > READ_BLOCK_LIMIT
        if stalled and t.is_alive():
            stop.set()
            if hasattr(port, "cancel_read"):
                port.cancel_read()
            t.join(READ_BLOCK_LIMIT)
            break
    stuck = t.is_alive()
    first = writer_error or (errors[0] if errors else None)
    if first is not None:
        if stuck:
            first.add_note("the reader thread was still blocked in read() after cancellation")
        raise first
    if stuck:
        raise OSError(errno.ETIMEDOUT, f"a read() blocked longer than {READ_BLOCK_LIMIT} s and could not be cancelled")
    if stalled:
        raise OSError(errno.ETIMEDOUT, f"a read() blocked longer than {READ_BLOCK_LIMIT} s; it was cancelled")
    stats = {"submitted": w["submitted"], "received": len(got), "write_s": w["end"] - t0,
             "first_rx_s": rx["first"] - t0 if rx else None, "last_rx_s": rx["last"] - t0 if rx else None}
    status, detail = compare(data, bytes(got))
    if len(got) > len(data):
        detail += f"; capture stopped after {len(got) - len(data)} excess bytes, so more may have followed"
    return status, detail, bytes(got), stats


def open_error(exc):
    """Maps a pyserial open failure to a distinct message."""
    code = getattr(exc, "errno", None)
    text = str(exc)
    if code == errno.ENOENT or "No such file" in text:
        return "serial device unavailable"
    if code == errno.EACCES or "Permission denied" in text:
        return "permission denied"
    if code in (errno.EBUSY, errno.EAGAIN) or "exclusively lock" in text or "Resource busy" in text:
        return "port already in use"
    return "open failed"


def rate(n, s):
    return f"{n / s:.0f} B/s" if s else "n/a"


def stream(port, args):
    data = stream_payload(args.length, args.seed)
    if args.measure:
        status, detail, got, st = run_measured(port, data, args.chunk, args.pace, args.timeout, args.quiet)
    else:
        status, detail, got, elapsed = run_stream(port, data, args.chunk, args.pace, args.timeout, args.quiet)
    mode = "measure" if args.measure else "correctness"
    print(f"{mode} stream {len(data)} bytes seed {args.seed} sha256 {hashlib.sha256(data).hexdigest()}, chunk {args.chunk} pace {args.pace} s timeout {args.timeout} s")
    print(f"received {len(got)} bytes sha256 {hashlib.sha256(got).hexdigest()}, missing {max(0, len(data) - len(got))}")
    if args.measure:
        print(f"host write() accepted {st['submitted']} bytes in {st['write_s']:.3f} s ({rate(st['submitted'], st['write_s'])}; OS and USB buffering, not line rate)")
        if st["last_rx_s"] is not None:
            print(f"first echo byte at {st['first_rx_s']:.3f} s, last at {st['last_rx_s']:.3f} s after the first write; "
                  f"completed echo throughput {rate(len(got), st['last_rx_s'])} (ideal 8N1 payload limit {args.baud / 10:.0f} B/s, not a measurement)")
    else:
        print(f"elapsed {elapsed:.2f} s including pacing and flush waits; echo rate {rate(len(got), elapsed)} is not a throughput measurement")
    print(f"SUMMARY {'PASS' if status == PASS else 'FAIL'}: {status} {detail}".rstrip())
    if args.save:  # after the verdict, so a save failure cannot hide it
        ns = time.time_ns()
        name = os.path.join(args.save, f"received_{mode}_{args.length}_seed{args.seed}_{time.strftime('%Y-%m-%dT%H-%M-%S', time.gmtime(ns // 10**9))}.{ns % 10**9:09d}Z.bin")
        with open(name, "xb") as f:  # never overwrite earlier evidence
            f.write(got)
        print(f"saved {name}")
    return 0 if status == PASS else 1


def vectors(port, args):
    failures = 0
    for name, data in test_vectors():
        status, detail = run_vector(port, data, args.chunk, args.pace, args.timeout, args.quiet)
        failures += status != PASS
        print(f"{status:<11} {name:<12} {len(data):>4} bytes {detail}".rstrip())
    total = len(test_vectors())
    print(f"SUMMARY {'PASS' if failures == 0 else 'FAIL'}: {total - failures} of {total} vectors echoed exactly")
    return 0 if failures == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True, help="serial device, for example /dev/cu.usbmodem21303")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=2.0, help="seconds without a received byte before the echo counts as short")
    ap.add_argument("--settle", type=float, default=0.5, help="seconds to wait after opening before draining input")
    ap.add_argument("--chunk", type=int, default=1, help="bytes per write")
    ap.add_argument("--pace", type=float, default=0.002, help="seconds between writes")
    ap.add_argument("--quiet", type=float, default=0.1, help="seconds to watch for unexpected extra bytes")
    ap.add_argument("--length", type=int, help="stream this many bytes instead of the nine vectors")
    ap.add_argument("--seed", type=int, default=1, help="stream payload seed")
    ap.add_argument("--save", help="existing directory for the received stream, written to a new file")
    ap.add_argument("--measure", action="store_true", help="with --length: write and read concurrently and report measured timestamps")
    args = ap.parse_args(argv)
    if args.baud <= 0 or args.timeout <= 0 or args.settle < 0 or args.chunk < 1 or args.pace < 0 or args.quiet < 0:
        ap.error("baud, timeout and chunk must be positive; settle, pace and quiet must not be negative")
    if args.length is not None and args.length < 1:
        ap.error("length must be positive")
    if args.save and (args.length is None or not os.path.isdir(args.save)):
        ap.error("save needs --length and an existing directory")
    if args.measure and args.length is None:
        ap.error("measure needs --length")

    import serial  # imported here so the pure functions above stay testable without pyserial

    try:
        port = serial.Serial(args.port, args.baud, bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                             timeout=0.1, write_timeout=args.timeout, xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL open {args.port}: {open_error(exc)} ({exc})")
        return 2

    try:
        time.sleep(args.settle)
        pending = port.in_waiting  # sampled before the clear; bytes arriving in between are cleared but not counted
        port.reset_input_buffer()
        print(f"port {args.port} {args.baud} 8N1, no flow control, exclusive; input cleared at start ({pending} bytes were pending just before the clear)")
        return vectors(port, args) if args.length is None else stream(port, args)
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL I/O error during test: {exc}")
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
