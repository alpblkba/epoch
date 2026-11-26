"""Tests for the Python wire reference; the C codec checks the same golden frames in tests/wire/test_wire.c."""

import unittest

import epoch_wire as w


class Wire(unittest.TestCase):
    def test_roundtrip_every_type_and_no_inner_zero(self):
        for name, (fmt, names) in w.LAYOUT.items():
            msg = {n: (i * 0x0101010101 + 0xFF) & (0xFF if n in ("ep1_status", "marker_valid") else 0xFFFFFFFF) for i, n in enumerate(names)}
            msg["type"] = name
            if name == "EP1_REPLY":
                msg["ep1_status"] = 1
            if name == "TEST_DIAG_REPLY":
                msg["marker_valid"] = 1
            if name == "ANNOUNCE":
                msg["nonce_echo"] = msg["retention_valid"] = True
            frame = w.encode(msg)
            self.assertEqual((frame[0], frame[-1]), (0, 0))
            self.assertNotIn(0, frame[1:-1])
            [(status, got)] = w.Parser().feed(frame)
            self.assertEqual(status, "OK")
            self.assertEqual({k: got[k] for k in msg}, msg)

    def test_golden_and_crc(self):
        self.assertEqual(w.encode({"type": "PING", "session": 0x0102030405060708, "counter": 0}).hex(),
                         "00030107010908070605040302010101010577" "1f667400")
        self.assertEqual(hex(__import__("zlib").crc32(b"123456789")), "0xcbf43926")

    def test_errors_and_resync(self):
        good = w.encode({"type": "PONG", "session": 42, "counter": 9})
        bad = bytearray(good)
        bad[6] ^= 0x5A
        p = w.Parser()
        results = p.feed(bytes(bad) + good[:5] + good + b"A" * 300 + b"\x00" + good)
        self.assertEqual([s for s, _ in results], ["ERR_CRC", "ERR_LENGTH", "OK", "ERR_OVERFLOW", "OK"])

    def test_long_cobs_block(self):
        data = bytes(range(1, 256)) * 2
        self.assertEqual(w.cobs_decode(w.cobs_encode(data)), data)
        self.assertEqual(w.cobs_decode(w.cobs_encode(b"\x00" * 5)), b"\x00" * 5)


if __name__ == "__main__":
    unittest.main()
