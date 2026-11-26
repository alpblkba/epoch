"""Tests for the cold_init_trials.py verdicts on frames modeled from the firmware; no board is needed."""

import json
import struct
import unittest
import zlib

import cold_init_trials as c

MAGIC, E0, E1, S = c.MAGIC, 0x5794D19B, 0x0BADCAFE, 0xF27741366E83E7AB


def slot(era, session, txid, counter, seq):
    w = [MAGIC, era, session & 0xFFFFFFFF, session >> 32, txid, counter, seq]
    return w + [zlib.crc32(struct.pack("<7I", *w))]


def announce(ws, era, retention_valid, boot_id=2, cause=c.SFTRSTF):
    a = {"type": "ANNOUNCE", "nonce_echo": False, "retention_valid": retention_valid, "era": era, "boot_id": boot_id, "reset_cause": cause}
    a.update({f"slot{i}": x for i, x in enumerate(ws)})
    return a


def reply(kind, run, code, mode, valid=False, cause=c.SFTRSTF):
    return {"type": kind, "run": run, "checkpoint": code, "txid": mode, "marker_valid": valid, "reset_cause": cause}


def cold_trial(cut, mode, old_order=False, layout=0):
    """t=2 (newest) in slot layout and t=1 in the other slot; then cold initialization up to the store cut, as the firmware does it, or with the era kept until the end as before the fix."""
    s0, s1, era = slot(E0, S, 2, 3136, 2), slot(E0, S, 1, 3135, 1), E0
    if layout:
        s0, s1 = s1, s0
    if cut >= 9 and not old_order:
        era = 0
    if cut >= 10:
        s0[0] = 0
    if cut >= 11:
        s1[0] = 0
    if cut >= 12:
        era = E1
    ws = s0 + s1
    judged_cold = mode == 0
    n = c.newest(ws, era, MAGIC) if era else "inconsistent"
    retention_valid = not judged_cold and n != "inconsistent"
    return {"kind": "cold", "run": 50, "code": cut, "mode": mode, "layout": layout, "session": S, "txids": [1, 2], "counters": [3135, 3136], "boot_before": 1,
            "after": [reply("TEST_ARM_ACK", 50, cut, mode), reply("TEST_DIAG_REPLY", 50, cut, mode), announce(ws, era, retention_valid)],
            "diag": reply("TEST_DIAG_REPLY", 50, cut, mode, valid=True)}


def forge_trial(mode, seq_step=None):
    s1 = slot(E0, S, 1, 1, 7)
    s0 = slot(E0, S, 1, 1, 7 + (seq_step if seq_step is not None else 2 if mode == 0 else 0))
    after = slot(E1, 0x77, 2, 2, 2) + slot(E1, 0x77, 1, 1, 1)
    return {"kind": "forge", "run": 60, "code": c.FORGE, "mode": mode, "session": S, "txids": [1], "counters": [1], "boot_before": 1,
            "after": [reply("TEST_ARM_ACK", 60, c.FORGE, mode), announce(s0 + s1, E0, False)],
            "diag": reply("TEST_DIAG_REPLY", 60, c.FORGE, mode, valid=True),
            "recovery": {"counter": 1, "session": 0x77, "txid": 2, "run": 61, "announce": announce(after, E1, True, boot_id=3),
                         "diag": reply("TEST_DIAG_REPLY", 61, c.F3, 2, valid=True)}}


class Verdicts(unittest.TestCase):
    def test_every_store_and_mode_passes(self):
        for cut in c.CUTS:
            for mode in (0, 1):
                self.assertEqual(c.judge_cold(cold_trial(cut, mode)), ("COUNTED_PASS", ""), (cut, mode))

    def test_slot_1_newest(self):
        for cut in c.CUTS:
            for mode in (0, 1):
                self.assertEqual(c.judge_cold(cold_trial(cut, mode, layout=1)), ("COUNTED_PASS", ""), (cut, mode))
        t = cold_trial(10, 1, layout=1)
        t["layout"] = 0
        self.assertEqual(c.judge_cold(t), ("INCONCLUSIVE", "slot 0 did not hold the newest commit one step above the previous record of the session"))
        t = cold_trial(10, 1, old_order=True, layout=1)  # the old order keeps the era: caught by the era check, not by a false outcome
        self.assertEqual(c.judge_cold(t)[0], "COUNTED_FAIL")
        self.assertIn("era", c.judge_cold(t)[1])
        a = t["after"][2]
        self.assertEqual(c.classify(True, c.words(a), E0, S, 2), "COMPLETED")  # the newest commit survives the slot 0 store: true, not false

    def test_old_store_order_fails_at_slot_0_when_judged_warm(self):
        v, why = c.judge_cold(cold_trial(10, 1, old_order=True))
        self.assertEqual(v, "COUNTED_FAIL")
        self.assertIn("era", why)
        t = cold_trial(10, 1, old_order=True)
        t["after"][2]["era"] = 0  # even with a correct era field, the old state classifies t=2 NOT_EXECUTED
        t["after"][2]["retention_valid"] = True
        self.assertEqual(c.classify(True, c.words(t["after"][2]), E0, S, 2), "NOT_EXECUTED")

    def test_reset_evidence_is_required(self):
        t = cold_trial(10, 1)
        del t["after"][1]
        self.assertEqual(c.judge_cold(t), ("INCONCLUSIVE", "preparatory reset not reported once with SFTRSTF and the arm"))
        t = cold_trial(10, 1)
        t["after"].append(announce(c.words(t["after"][2]), 0, False, boot_id=3))
        self.assertEqual(c.judge_cold(t)[1], "2 boots after the injected reset, expected 1")
        t = cold_trial(10, 1)
        t["after"][2]["reset_cause"] = c.SFTRSTF | c.BORRSTF
        self.assertEqual(c.judge_cold(t)[0], "INCONCLUSIVE")
        t = cold_trial(10, 1)
        t["diag"]["checkpoint"] = 11
        self.assertEqual(c.judge_cold(t)[1], "checkpoint marker does not name the commanded store")
        t = cold_trial(10, 1)
        t["after"][0]["txid"] = 0
        self.assertEqual(c.judge_cold(t)[0], "INCONCLUSIVE")
        for judge, t in ((c.judge_cold, cold_trial(10, 1)), (c.judge_forge, forge_trial(0))):
            t["events"] = [{"dir": "rx", "status": "OK", "msg": {}}, {"dir": "rx", "status": "ERR_CRC", "msg": None}]  # a damaged frame, as when the UART wait timed out
            self.assertEqual(judge(t), ("INCONCLUSIVE", "a frame of the trial was rejected by the parser"))

    def test_unexplained_boot_is_inconclusive(self):
        def ann(boot):
            return {"dir": "rx", "status": "OK", "msg": {"type": "ANNOUNCE", "boot_id": boot}}
        for judge, t, good in ((c.judge_cold, cold_trial(10, 1), [1, 2]), (c.judge_forge, forge_trial(0), [1, 2, 3])):
            for ids in (good[:-1], good + [9], [1, 2, 1], [7] + good[1:]):  # a reset missing, one extra, a boot coming back, a reset before the trial
                t["events"] = [ann(b) for b in ids]
                self.assertEqual(judge(t), ("INCONCLUSIVE", "a boot not explained by the commanded resets"), ids)
            t["events"] = [ann(b) for b in good]
            self.assertEqual(judge(t)[0], "COUNTED_PASS")
        self.assertEqual(c.boot_ids([ann(1), ann(1), {"dir": "tx"}, ann(2), ann(2)]), [1, 2])

    def test_classification_recorded(self):
        t = cold_trial(11, 0)
        c.judge_cold(t)
        self.assertEqual(t["classes"], ["UNKNOWN", "UNKNOWN"])

    def test_precondition_newest_in_slot_0(self):
        t = cold_trial(10, 1)
        t["txids"], t["counters"] = [2, 1], [3136, 3135]  # slot 1 newest: overwriting slot 0 would not expose an older record
        self.assertEqual(c.judge_cold(t)[1], "slot 0 did not hold the newest commit one step above the previous record of the session")

    def test_wrong_store_effect_fails(self):
        t = cold_trial(11, 0)
        t["after"][2]["slot8"] = MAGIC  # slot 1 w0 not cleared
        self.assertEqual(c.judge_cold(t)[0], "COUNTED_FAIL")
        t = cold_trial(12, 1)
        t["after"][2]["retention_valid"] = False
        self.assertEqual(c.judge_cold(t)[0], "COUNTED_FAIL")

    def test_forge_modes_pass(self):
        for mode in (0, 1):
            self.assertEqual(c.judge_forge(forge_trial(mode)), ("COUNTED_PASS", ""), mode)

    def test_forge_failures(self):
        self.assertEqual(c.judge_forge(forge_trial(0, seq_step=1))[1], "the pair does not break the one-step rule as the mode requires")
        t = forge_trial(1)
        t["after"][1]["retention_valid"] = True
        self.assertEqual(c.judge_forge(t)[0], "COUNTED_FAIL")
        t = forge_trial(0)
        t["after"][1]["slot7"] ^= 1
        self.assertEqual(c.judge_forge(t)[1], "a forged slot is not valid on its own")
        t = forge_trial(0)
        t["recovery"]["counter"] = 2
        self.assertEqual(c.judge_forge(t)[0], "COUNTED_FAIL")
        t = forge_trial(0)
        t["recovery"]["announce"]["era"] = E0
        self.assertEqual(c.judge_forge(t)[0], "COUNTED_FAIL")
        t = forge_trial(0)
        t["after"].insert(1, reply("TEST_DIAG_REPLY", 60, c.FORGE, 0))
        self.assertEqual(c.judge_forge(t)[1], "unexpected TEST_DIAG_REPLY")

    def test_corrupt_judge(self):
        """judge_corrupt on model evidence of the corrupted-slot image, then with the board answering as the standard image or accepting the corrupted slot."""
        import test_verify_cold_init_trials as tv
        for layout in (0, 1):
            t = tv.campaign(None, layout, True)[1]
            self.assertEqual(c.judge_corrupt(t), ("COUNTED_PASS", ""), layout)
            self.assertEqual(t["classes"], ["UNKNOWN"] * len(t["txids"]))

        def boot_after(change):
            t = tv.campaign(None, 0, True)[1]
            a = next(m for m in t["after"] if m["type"] == "ANNOUNCE")
            change(a)
            return c.judge_corrupt(t)
        self.assertEqual(boot_after(lambda a: a.update(slot5=a["slot5"] ^ 1))[0], "INCONCLUSIVE")  # intact slot: the standard image or no corruption
        self.assertEqual(boot_after(lambda a: a.update(slot0=0))[0], "INCONCLUSIVE")  # w0 at exactly 0 is an empty slot, the case CRC checking cannot see
        self.assertEqual(boot_after(lambda a: a.update(retention_valid=True)), ("COUNTED_FAIL", "corrupted slot accepted as valid retention"))
        t = tv.campaign(None, 0, True)[1]
        t["recovery"]["counter"] = 4
        self.assertEqual(c.judge_corrupt(t)[0], "COUNTED_FAIL")
        t = tv.campaign(None, 0, True)[1]
        t["recovery"]["announce"]["era"] = next(m for m in t["after"] if m["type"] == "ANNOUNCE")["era"]
        self.assertEqual(c.judge_corrupt(t), ("COUNTED_FAIL", "no valid retention under a new era after the F3 reset"))

    def test_last_commit_lands_in_the_layout_slot(self):
        class Stub:
            """A link that acknowledges everything and notes which slot the runner believes newest when it arms."""
            def __init__(self, board):
                self.board, self.events, self.q, self.armed = board, [], [], None
            def bind(self):
                return 1, 7
            def send(self, **m):
                if m["type"] == "TEST_ARM":
                    self.armed = self.board.live
                self.q = [{"type": "EP1_REPLY", "txid": m["txid"], "ep1_status": 0, "counter": m["txid"]}] if m["type"] == "INCREMENT" else [{"type": "TEST_DIAG_REPLY"}]
            def recv(self, timeout=0.3, until=None):
                q, self.q = self.q, []
                return q
        for start in (None, 0, 1):
            for layout in (0, 1):
                b = c.Board(None, None, 0)
                b.live = start
                stub = Stub(b)
                b.link = lambda: stub
                t = b.cold(9, 0, layout)
                self.assertEqual((stub.armed, t["layout"]), (layout, layout), (start, layout))
                self.assertIn(t["txids"], ([1, 2], [1, 2, 3]))

    def test_plan_counts(self):
        self.assertEqual(len(c.plan(True, None)), 10)
        self.assertEqual(sum(k == "cold" for k, _, _ in c.plan(False, 5)), 40)
        self.assertEqual(sum(k == "forge" for k, _, _ in c.plan(False, 5)), 10)
        self.assertEqual(c.plan(True, None, 1), c.plan(True, None)[:8])
        self.assertEqual(len(c.plan(False, 2, 1)), 16)
        self.assertEqual(c.plan(True, None, 1, True), [("corrupt", c.FORGE, c.CORRUPT)])
        self.assertEqual(len(c.plan(False, 5, 0, True)), 5)


class Port:
    """A serial port that replays model frames: each write must equal the model's next sent frame and releases the bytes the model received after it. fail names a write, a read after a write, or a flush after a write that raises exc instead."""

    def __init__(self, script, fail=None, exc=None):
        self.script, self.fail, self.exc, self.n, self.buf = script, fail, exc, 0, b""

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        if self.fail == ("write", self.n + 1):
            raise self.exc
        tx, rx = self.script[self.n]
        assert data == tx, f"write {self.n + 1}: runner sent {data.hex()}, model {tx.hex()}"
        self.n += 1
        self.buf += rx

    def flush(self):
        if self.fail == ("flush", self.n):
            raise self.exc

    @property
    def in_waiting(self):
        return len(self.buf)

    def read(self, k):
        if self.fail == ("read", self.n):
            raise self.exc
        data, self.buf = self.buf[:k], self.buf[k:]
        return data


class Transport(unittest.TestCase):
    """The runner's main loop against model frames of the layout 1 pilot, with serial failures injected."""

    ARGS = ["--pilot", "--layout", "1"]

    @classmethod
    def setUpClass(cls):
        import test_verify_cold_init_trials as tv
        b = tv.Board()
        cls.load([tv.sync(b)] + [tv.cold(b, code, mode, 1) for _, code, mode in c.plan(True, None, 1)])

    @classmethod
    def load(cls, recs):
        import epoch_wire as w
        cls.script, cls.nonces, cls.arms = [], [], []
        for e in (e for t in recs for e in t["events"]):
            if e["dir"] == "tx":
                cls.script.append((bytes.fromhex(e["hex"]), b""))
                m = next(m for _, m in w.Parser().feed(bytes.fromhex(e["hex"])))
                if m["type"] == "PROBE":
                    cls.nonces.append(m["nonce"])
                if m["type"] == "TEST_ARM":
                    cls.arms.append(len(cls.script))
            elif e["dir"] == "rxraw":
                tx, rx = cls.script[-1]
                cls.script[-1] = (tx, rx + bytes.fromhex(e["hex"]))

    def main(self, fail=None, exc=None, verifier=True):
        import os
        import tempfile
        import types
        from unittest import mock
        import ep0_adversarial as ea
        clock = iter(i / 100 for i in range(10**7))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = tmp.name
        args = ["--port", "x", "--evidence", d, "--run", "0", *self.ARGS] + ["--verifier", os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_cold_init_trials.py")] * verifier
        port = Port(self.script, fail, exc)
        with mock.patch("serial.Serial", lambda *a, **k: port), mock.patch.object(ea, "time", types.SimpleNamespace(monotonic=lambda: next(clock))), \
                mock.patch.object(ea, "secrets", types.SimpleNamespace(randbits=lambda _: next(nonces))), mock.patch("builtins.print"):
            nonces = iter(self.nonces)
            try:
                rc = c.main(args)
            except Exception as e:
                rc = e
        def load(f):
            with open(os.path.join(d, f)) as fh:
                return fh.read()
        recs = [json.loads(l) for l in load("cold_init_trials.jsonl").splitlines()]
        stops = [json.loads(load(f)) for f in os.listdir(d) if f.startswith("interrupted_")]
        return rc, recs, stops, d

    def verify(self, d, *extra):
        import subprocess
        import sys
        import os
        v = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_cold_init_trials.py")
        return subprocess.run([sys.executable, "-I", v, os.path.join(d, "cold_init_trials.jsonl"), "--run", "0", *self.ARGS, *extra], capture_output=True, text=True)

    def test_layout_1_pilot_passes_end_to_end(self):
        rc, recs, stops, d = self.main()
        self.assertEqual((rc, len(recs), stops), (0, 9, []))
        self.assertTrue(all(t["verdict"] == "COUNTED_PASS" and t["layout"] == 1 for t in recs[1:]))
        self.assertEqual(self.verify(d).returncode, 0)

    def test_write_failure_at_arm(self):
        import serial
        k = self.arms[2]  # the TEST_ARM of the second cold trial; arms[0] is the sync F3 reset
        rc, recs, stops, d = self.main(("write", k), serial.SerialException("write failed"))
        self.assertEqual((rc, len(recs), len(stops)), (1, 2, 1))  # sync and the first trial complete, nothing appended for the second
        f = stops[0]
        self.assertEqual((f["index"], f["kind"], f["code"], f["mode"], f["verdict"]), (2, "cold", 9, 1, "INCONCLUSIVE"))
        self.assertIn("SerialException('write failed')", f["reason"])
        self.assertEqual([a["run"] for a in f["arm_sent"]], [f["last_run"]])  # the write had started: the board may have been armed
        self.assertEqual(f["events"][-1]["note"], "TEST_ARM")
        self.assertEqual(len(f["binds"]), 1)
        self.assertEqual(sum(e["dir"] == "tx" and e["note"] == "INCREMENT" for e in f["events"]), 2 if recs[1]["txids"] == [1, 2] else 3)
        self.assertNotEqual(self.verify(d).returncode, 0)  # an incomplete campaign fails without --partial
        self.assertEqual(self.verify(d, "--partial").returncode, 0)

    def test_read_failure_after_arm(self):
        k = self.arms[1]
        rc, recs, stops, _ = self.main(("read", k), OSError(6, "Device not configured"))
        self.assertEqual((rc, len(recs), len(stops)), (1, 1, 1))
        f = stops[0]
        self.assertEqual((f["index"], f["kind"], f["code"], f["mode"]), (1, "cold", 9, 0))
        self.assertEqual(len(f["arm_sent"]), 1)
        self.assertIn("Device not configured", f["reason"])

    def test_flush_failure_before_arm(self):
        import termios
        rc, recs, stops, _ = self.main(("flush", 1), termios.error(5, "Input/output error"), verifier=False)
        self.assertEqual((rc, recs, len(stops)), (1, [], 1))
        f = stops[0]
        self.assertEqual((f["index"], f["kind"], f["arm_sent"], len(f["events"])), (0, "sync", [], 1))

    def test_programming_error_keeps_evidence_and_is_raised(self):
        k = self.arms[1]
        rc, recs, stops, _ = self.main(("read", k), ZeroDivisionError("bug"), verifier=False)
        self.assertIsInstance(rc, ZeroDivisionError)
        self.assertEqual((len(recs), len(stops), stops[0]["kind"]), (1, 1, "cold"))


class CorruptTransport(unittest.TestCase):
    """The runner's main loop with --corrupt against model frames of the corrupted-slot image, layout 1 pilot."""
    ARGS = ["--pilot", "--layout", "1", "--corrupt"]
    main, verify = Transport.main, Transport.verify

    @classmethod
    def setUpClass(cls):
        import test_verify_cold_init_trials as tv
        b = tv.Board()
        Transport.load.__func__(cls, [tv.sync(b), tv.corrupt(b, 1)])

    def test_corrupt_pilot_passes_end_to_end(self):
        rc, recs, stops, d = self.main()
        self.assertEqual((rc, len(recs), stops), (0, 2, []))
        self.assertEqual((recs[1]["kind"], recs[1]["layout"], recs[1]["verdict"]), ("corrupt", 1, "COUNTED_PASS"))
        self.assertEqual(self.verify(d).returncode, 0)


if __name__ == "__main__":
    unittest.main()
