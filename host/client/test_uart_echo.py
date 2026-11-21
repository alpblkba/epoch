"""Tests for uart_echo_test.py against a fake serial transport. They check the harness logic only and say nothing about the physical UART."""

import contextlib
import errno
import hashlib
import io
import os
import sys
import threading
import time
import types
import unittest
from unittest import mock

import uart_echo_test as u


class FakeSerial:
    """Echoes writes through an optional transform; read returns what is buffered and, when nothing is, waits briefly like a pyserial read that times out. max_read and max_write model partial reads and short writes. A lock makes one reader thread and one writer thread safe."""

    def __init__(self, transform=lambda b: b, max_read=None, max_write=None):
        self.transform, self.rx, self.timeout = transform, bytearray(), None
        self.max_read, self.max_write, self.closed = max_read, max_write, False
        self.lock = threading.Lock()

    def write(self, data):
        data = bytes(data)[:self.max_write]
        echo = self.transform(data)
        with self.lock:
            self.rx += echo
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        with self.lock:
            self.rx.clear()

    def close(self):
        self.closed = True

    @property
    def in_waiting(self):
        with self.lock:
            return len(self.rx)

    def read(self, n):
        with self.lock:
            out = bytes(self.rx[:min(n, self.max_read or n)])
            del self.rx[:len(out)]
        if not out:
            time.sleep(min(self.timeout or 0, 0.005))
        return out


def nth(n, fn):
    """Per-write transform that applies fn to the nth write only, counting from 1."""
    seen = {"n": 0}

    def t(b):
        seen["n"] += 1
        return fn(b) if seen["n"] == n else b

    return t


def swap(n):
    """Holds back the echo of write n and releases it after write n + 1."""
    seen = {"n": 0, "held": b""}

    def t(b):
        seen["n"] += 1
        if seen["n"] == n:
            seen["held"] = b
            return b""
        return b + seen["held"] if seen["n"] == n + 1 else b

    return t


def run(transform, data=b"\x00\x01\x7f\x80\xff", chunk=1):
    return u.run_vector(FakeSerial(transform), data, chunk, 0, 0.05, 0)


def stream(port, data, chunk=1):
    return u.run_stream(port, data, chunk, 0, 0.05, 0)


def measured(port, data, chunk=1, timeout=0.2):
    return u.run_measured(port, data, chunk, 0, timeout, 0.02)


class SlowEcho(FakeSerial):
    """Echoes in bursts of `burst` bytes, one burst every `period` seconds: steady progress whose total duration can far exceed the idle timeout. `stop_after` ends delivery after that many bytes, which models a genuine stall."""

    def __init__(self, burst=10, period=0.05, stop_after=None, **kw):
        super().__init__(**kw)
        self.burst, self.period, self.stop_after, self.sent_back, self.next_at = burst, period, stop_after, 0, 0.0

    def read(self, n):
        wait = self.next_at - time.monotonic()
        if wait > 0 or (self.stop_after is not None and self.sent_back >= self.stop_after):
            time.sleep(max(0.0, min(wait, self.timeout or 0)) if wait > 0 else min(self.timeout or 0, 0.005))
            return b""
        with self.lock:
            out = bytes(self.rx[:min(n, self.burst)])
            del self.rx[:len(out)]
        if out:
            self.sent_back += len(out)
            self.next_at = time.monotonic() + self.period
        else:
            time.sleep(min(self.timeout or 0, 0.005))
        return out


class BlockingRead(FakeSerial):
    """A backend whose read ignores its timeout and blocks until cancel_read is called."""

    def __init__(self, **kw):
        super().__init__(**kw)
        self.cancelled = threading.Event()

    def read(self, n):
        self.cancelled.wait(10)
        return b""

    def cancel_read(self):
        self.cancelled.set()


def readers_alive():
    return [t for t in threading.enumerate() if t.name == "echo-reader" and t.is_alive()]


def fake_serial_module(port):
    fake = types.ModuleType("serial")
    fake.Serial = lambda *a, **k: port
    fake.SerialException = OSError
    fake.EIGHTBITS, fake.PARITY_NONE, fake.STOPBITS_ONE = 8, "N", 1
    return fake


class EchoHarness(unittest.TestCase):
    def test_vectors_cover_required_bytes(self):
        allbytes = b"".join(d for _, d in u.test_vectors())
        for b in (0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55, 0xAA):
            self.assertIn(b, allbytes)
        self.assertEqual(dict(u.test_vectors())["all256"], bytes(range(256)))
        self.assertEqual(u.test_vectors(), u.test_vectors())
        self.assertEqual(sum(len(d) for _, d in u.test_vectors()), 367)

    def test_exact_echo_passes(self):
        for chunk in (1, 3, 64):
            self.assertEqual(run(lambda b: b, chunk=chunk)[0], u.PASS)

    def test_silence_is_no_response(self):
        self.assertEqual(run(lambda b: b"")[0], u.NO_RESPONSE)

    def test_missing_byte(self):
        status, detail = run(nth(5, lambda b: b""))
        self.assertEqual(status, u.SHORT)  # tail missing
        self.assertIn("4 of 5", detail)
        status, detail = run(nth(3, lambda b: b""))
        self.assertEqual(status, u.MISMATCH)  # later bytes shift into the gap
        self.assertIn("byte 2", detail)

    def test_corrupted_byte_is_mismatch(self):
        status, detail = run(lambda b: b"\x7e" if b == b"\x7f" else b)
        self.assertEqual(status, u.MISMATCH)
        self.assertIn("byte 2", detail)

    def test_extra_byte_is_extra(self):
        self.assertEqual(run(lambda b: b + b"\x00" if b == b"\xff" else b)[0], u.EXTRA)

    def test_doubled_bytes_never_pass(self):
        self.assertNotEqual(run(lambda b: b + b)[0], u.PASS)

    def test_open_errors_are_distinct(self):
        self.assertEqual(u.open_error(OSError(errno.ENOENT, "No such file or directory")), "serial device unavailable")
        self.assertEqual(u.open_error(OSError(errno.EACCES, "Permission denied")), "permission denied")
        self.assertEqual(u.open_error(Exception("Could not exclusively lock port /dev/x: [Errno 35]")), "port already in use")

    def test_bad_arguments_exit_2(self):
        for extra in (["--chunk", "0"], ["--measure"], ["--length", "0"], ["--length", "-5"], ["--save", "/nonexistent-dir", "--length", "10"], ["--save", "."]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    u.main(["--port", "/dev/null"] + extra)
                self.assertEqual(cm.exception.code, 2)


class StreamHarness(unittest.TestCase):
    def test_payload_is_deterministic_and_covers_bytes(self):
        data = u.stream_payload(100000)
        self.assertEqual(len(data), 100000)
        self.assertEqual(data, u.stream_payload(100000))
        self.assertEqual(data[:256], bytes(range(256)))
        self.assertIn(b"\x00" * 32, data)
        self.assertIn(b"\xff" * 32, data)
        self.assertIn(bytes([0x55, 0xAA]) * 32, data)
        self.assertEqual(set(data[512:]), set(range(256)))  # the pseudorandom part alone reaches every value
        self.assertNotEqual(data, u.stream_payload(100000, seed=2))
        # Pinned so a recorded hardware run can be re-derived from length and seed.
        self.assertEqual(hashlib.sha256(data).hexdigest(), "0e614e5976b223f1c29d539aaf74fe87185fec1c549e2870210eba05d6d1214d")

    def test_empty_and_one_byte(self):
        self.assertEqual(u.stream_payload(0), b"")
        self.assertEqual(u.stream_payload(1), b"\x00")
        self.assertEqual(stream(FakeSerial(), b"")[0], u.PASS)
        self.assertEqual(stream(FakeSerial(), b"\x00")[0], u.PASS)

    def test_long_stream_with_partial_reads_and_short_writes(self):
        data = u.stream_payload(100000)
        for port, chunk in ((FakeSerial(), 64), (FakeSerial(max_read=7), 64), (FakeSerial(max_write=5), 64), (FakeSerial(max_read=1, max_write=3), 1024)):
            status, detail, got, _ = stream(port, data, chunk)
            self.assertEqual((status, detail, got), (u.PASS, "", data))

    def test_write_that_accepts_nothing_is_an_error(self):
        with self.assertRaises(OSError):
            u.write_all(FakeSerial(max_write=0), b"abc")

    def test_stream_failures(self):
        data = u.stream_payload(1000)
        cases = [
            (lambda b: b"", u.NO_RESPONSE, "0 of 1000"),
            (nth(1000, lambda b: b""), u.SHORT, "999 of 1000"),
            (nth(501, lambda b: b""), u.MISMATCH, "byte 500"),
            (nth(501, lambda b: bytes([b[0] ^ 0x01])), u.MISMATCH, "byte 500"),
            (nth(1000, lambda b: b + b"\x00"), u.EXTRA, "1 extra"),
            (swap(3), u.MISMATCH, "byte 2"),  # bytes 2 and 3 differ, so the reordering is visible
        ]
        for transform, status, text in cases:
            with self.subTest(status=status, text=text):
                got_status, detail, got, _ = stream(FakeSerial(transform), data)
                self.assertEqual(got_status, status)
                self.assertIn(text, detail)
                self.assertNotEqual(got, data)

    def test_port_closed_after_io_failure(self):
        class Broken(FakeSerial):
            def write(self, data):
                raise OSError(errno.EIO, "device gone")

        port = Broken()
        fake = types.ModuleType("serial")
        fake.Serial = lambda *a, **k: port
        fake.SerialException = OSError
        fake.EIGHTBITS, fake.PARITY_NONE, fake.STOPBITS_ONE = 8, "N", 1
        out = io.StringIO()
        with mock.patch.dict(sys.modules, {"serial": fake}), contextlib.redirect_stdout(out):
            self.assertEqual(u.main(["--port", "x", "--settle", "0", "--length", "10"]), 1)
        self.assertTrue(port.closed)
        self.assertIn("FAIL I/O error", out.getvalue())

    def test_startup_bytes_are_cleared_and_reported_as_observed(self):
        port, out = FakeSerial(), io.StringIO()
        port.rx += b"boot junk"  # 9 bytes pending when the port opens
        with mock.patch.dict(sys.modules, {"serial": fake_serial_module(port)}), contextlib.redirect_stdout(out):
            rc = u.main(["--port", "x", "--settle", "0", "--quiet", "0", "--timeout", "0.1", "--pace", "0"])
        self.assertEqual(rc, 0)  # the junk was cleared, so it cannot contaminate the first vector
        self.assertIn("9 bytes were pending", out.getvalue())
        self.assertNotIn("discarded", out.getvalue())
        self.assertIn("SUMMARY PASS: 9 of 9", out.getvalue())

    def test_main_stream_pass_and_fail_exit_codes(self):
        for transform, code, word in ((lambda b: b, 0, "SUMMARY PASS"), (nth(10, lambda b: b""), 1, "SUMMARY FAIL")):
            port = FakeSerial(transform)
            fake = types.ModuleType("serial")
            fake.Serial = lambda *a, **k: port
            fake.SerialException = OSError
            fake.EIGHTBITS, fake.PARITY_NONE, fake.STOPBITS_ONE = 8, "N", 1
            out = io.StringIO()
            with mock.patch.dict(sys.modules, {"serial": fake}), contextlib.redirect_stdout(out):
                rc = u.main(["--port", "x", "--settle", "0", "--quiet", "0", "--timeout", "0.05", "--length", "300", "--chunk", "1", "--pace", "0"])
            self.assertEqual(rc, code)
            self.assertIn(word, out.getvalue())
            self.assertTrue(port.closed)



class MeasuredHarness(unittest.TestCase):
    def tearDown(self):
        self.assertEqual(readers_alive(), [])  # every path joins its reader thread

    def test_long_stream_concurrent_with_partial_io(self):
        data = u.stream_payload(100000)
        for port in (FakeSerial(), FakeSerial(max_read=7), FakeSerial(max_write=5), FakeSerial(max_read=1, max_write=3)):
            status, detail, got, st = measured(port, data, 64)
            self.assertEqual((status, detail, got), (u.PASS, "", data))
            self.assertEqual((st["submitted"], st["received"]), (100000, 100000))
            self.assertTrue(0 <= st["first_rx_s"] <= st["last_rx_s"])
            self.assertGreaterEqual(st["write_s"], 0)

    def test_failures_are_detected(self):
        data = u.stream_payload(1000)
        cases = [
            (nth(1000, lambda b: b""), u.SHORT, "999 of 1000"),
            (nth(501, lambda b: b""), u.MISMATCH, "byte 500"),
            (nth(501, lambda b: bytes([b[0] ^ 0x01])), u.MISMATCH, "byte 500"),
            (nth(1000, lambda b: b + b"\x00"), u.EXTRA, "1 extra"),
            (swap(3), u.MISMATCH, "byte 2"),
        ]
        for transform, status, text in cases:
            with self.subTest(status=status, text=text):
                got_status, detail, got, st = measured(FakeSerial(transform), data)
                self.assertEqual(got_status, status)
                self.assertIn(text, detail)
                self.assertEqual(st["submitted"], 1000)
                self.assertEqual(st["received"], len(got))

    def test_reader_timeout_is_bounded(self):
        start = time.monotonic()
        status, detail, got, st = measured(FakeSerial(lambda b: b""), b"abc", timeout=0.2)
        self.assertEqual((status, got, st["first_rx_s"]), (u.NO_RESPONSE, b"", None))
        self.assertLess(time.monotonic() - start, 1.5)

    def test_writer_timeout_stops_reader(self):
        class Stalled(FakeSerial):
            def write(self, data):
                raise OSError(errno.ETIMEDOUT, "Write timeout")

        with self.assertRaises(OSError):
            measured(Stalled(), b"abc")

    def test_early_close_during_read(self):
        class Closing(FakeSerial):
            def read(self, n):
                if self.in_waiting:
                    raise OSError(errno.EIO, "device disconnected")
                return super().read(n)

        with self.assertRaises(OSError):
            measured(Closing(), u.stream_payload(500))

    def test_main_measure_exit_codes_and_report(self):
        for transform, code, word in ((lambda b: b, 0, "SUMMARY PASS"), (nth(10, lambda b: b""), 1, "SUMMARY FAIL")):
            port, out = FakeSerial(transform), io.StringIO()
            with mock.patch.dict(sys.modules, {"serial": fake_serial_module(port)}), contextlib.redirect_stdout(out):
                rc = u.main(["--port", "x", "--settle", "0", "--quiet", "0.02", "--timeout", "0.2", "--length", "300", "--chunk", "16", "--pace", "0", "--measure"])
            self.assertEqual(rc, code)
            self.assertIn(word, out.getvalue())
            self.assertIn("host write() accepted 300 bytes", out.getvalue())
            self.assertTrue(port.closed)
        self.assertIn("completed echo throughput", out.getvalue())

    def test_save_twice_in_one_second_keeps_both(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            for _ in range(2):
                port, out = FakeSerial(), io.StringIO()
                with mock.patch.dict(sys.modules, {"serial": fake_serial_module(port)}), contextlib.redirect_stdout(out):
                    rc = u.main(["--port", "x", "--settle", "0", "--quiet", "0.02", "--timeout", "0.2", "--length", "50", "--chunk", "50", "--pace", "0", "--measure", "--save", d])
                self.assertEqual(rc, 0)
                self.assertLess(out.getvalue().index("SUMMARY PASS"), out.getvalue().index("saved "))
            with mock.patch("time.time_ns", return_value=1_700_000_000_123_456_789):  # same name twice: the second save must fail, not overwrite
                for expected in (0, 1):
                    port, out = FakeSerial(), io.StringIO()
                    with mock.patch.dict(sys.modules, {"serial": fake_serial_module(port)}), contextlib.redirect_stdout(out):
                        rc = u.main(["--port", "x", "--settle", "0", "--quiet", "0.02", "--timeout", "0.2", "--length", "50", "--chunk", "50", "--pace", "0", "--save", d])
                    self.assertEqual(rc, expected)
                    self.assertIn("SUMMARY PASS", out.getvalue())
            self.assertEqual(len(os.listdir(d)), 3)

    def test_slow_healthy_stream_longer_than_idle_timeout_passes(self):
        # 400 bytes at 10 bytes per 50 ms take about 2 s against a 200 ms idle timeout, so the idle timeout must measure the gap between bytes, not the whole run.
        data, port = u.stream_payload(400), SlowEcho(burst=10, period=0.05)
        start = time.monotonic()
        status, detail, got, st = u.run_measured(port, data, 400, 0, 0.2, 0.02)
        self.assertEqual((status, detail, got, st["received"]), (u.PASS, "", data, 400))
        self.assertGreater(time.monotonic() - start, 1.0)  # far longer than the idle timeout
        self.assertGreater(st["last_rx_s"], 1.0)
        self.assertFalse(port.closed)

    def test_genuine_stall_fails_within_idle_bound(self):
        data, port = u.stream_payload(400), SlowEcho(burst=10, period=0.05, stop_after=100)
        start = time.monotonic()
        status, detail, got, st = u.run_measured(port, data, 400, 0, 0.2, 0.02)
        self.assertEqual((status, len(got)), (u.SHORT, 100))
        self.assertIn("100 of 400", detail)
        self.assertLess(time.monotonic() - start, 0.6 + 1.5)  # about 0.5 s of delivery plus 0.2 s idle, with margin

    def test_blocked_read_is_cancelled_and_reported(self):
        port = BlockingRead()
        start = time.monotonic()
        with self.assertRaises(OSError) as cm:
            u.run_measured(port, b"abc", 3, 0, 0.1, 0.02)
        self.assertEqual(cm.exception.errno, errno.ETIMEDOUT)
        self.assertTrue(port.cancelled.is_set())
        self.assertLess(time.monotonic() - start, 5)

    def test_reader_error_stops_writer_early(self):
        class Failing(FakeSerial):
            writes = 0

            def write(self, data):
                Failing.writes += 1
                time.sleep(0.002)
                return super().write(data)

            def read(self, n):
                raise OSError(errno.EIO, "device disconnected")

        with self.assertRaises(OSError) as cm:
            u.run_measured(Failing(), u.stream_payload(5000), 1, 0, 0.2, 0.02)
        self.assertEqual(cm.exception.errno, errno.EIO)  # the reader's error, not a cleanup error
        self.assertLess(Failing.writes, 5000)  # the writer noticed the stop instead of sending everything

    def test_writer_error_is_the_reported_error(self):
        class Stalled(FakeSerial):
            def write(self, data):
                raise OSError(errno.ETIMEDOUT, "Write timeout")

        with self.assertRaises(OSError) as cm:
            measured(Stalled(), b"abc")
        self.assertIn("Write timeout", str(cm.exception))

    def test_continuous_extra_stream_fails_promptly_as_extra(self):
        class Babbler(FakeSerial):  # echoes, then never stops sending
            echoed = False

            def read(self, n):
                with self.lock:
                    out = bytes(self.rx[:n])
                    del self.rx[:n]
                    self.echoed |= bool(out)
                time.sleep(0.001)
                return out or (b"z" * n if self.echoed else b"")

        start = time.monotonic()
        status, detail, got, st = u.run_measured(Babbler(), b"abc", 3, 0, 0.2, 0.02)
        self.assertEqual(status, u.EXTRA)
        self.assertTrue(got.startswith(b"abc"))
        self.assertLessEqual(len(got), 3 + u.EXTRA_KEEP)  # bounded, not every byte the device sent
        self.assertIn("capture stopped", detail)
        self.assertLess(time.monotonic() - start, 1.0)

    def test_oversized_single_read_is_bounded_and_extra(self):
        class Burst(FakeSerial):  # one read returns 20 bytes, more than requested
            def read(self, n):
                with self.lock:
                    have = bool(self.rx)
                    self.rx.clear()
                if have:
                    return b"abc" + bytes(17)
                time.sleep(0.005)
                return b""

        status, detail, got, st = u.run_measured(Burst(), b"abc", 3, 0, 0.2, 0.02)
        self.assertEqual(status, u.EXTRA)
        self.assertEqual(got, b"abc" + bytes(u.EXTRA_KEEP))

    def test_exact_short_message_passes(self):
        status, detail, got, st = u.run_measured(FakeSerial(), b"abc", 1, 0, 0.2, 0.02)
        self.assertEqual((status, got, st["submitted"], st["received"]), (u.PASS, b"abc", 3, 3))

    def test_main_measure_io_failure_closes_port(self):
        class Stalled(FakeSerial):
            def write(self, data):
                raise OSError(errno.ETIMEDOUT, "Write timeout")

        port, out = Stalled(), io.StringIO()
        with mock.patch.dict(sys.modules, {"serial": fake_serial_module(port)}), contextlib.redirect_stdout(out):
            self.assertEqual(u.main(["--port", "x", "--settle", "0", "--length", "10", "--measure"]), 1)
        self.assertTrue(port.closed)
        self.assertIn("FAIL I/O error", out.getvalue())


if __name__ == "__main__":
    unittest.main()
