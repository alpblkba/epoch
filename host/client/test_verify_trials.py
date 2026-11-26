"""Tests for verify_trials.py on synthetic records; no board or archive is needed."""

import contextlib
import io
import json
import os
import struct
import tempfile
import unittest
import zlib

import verify_trials as v

MAGIC = 0x45504301
ERA = 0x5794D19B
SESSION = "fa7d948c29b965b7"


def record(policy="b3", checkpoint="f3", outcome=None, **over):
    executed = checkpoint == "f3"
    c0 = 100
    t = {"utc_ms": 1, "policy": policy, "checkpoint": checkpoint, "run": 7, "verdict": "COUNTED_PASS", "why": "",
         "outcome": outcome or v.expected_outcome(policy, executed), "c0": c0, "c2": c0 + 1 + executed + (policy == "b0"),
         "retry_counter": c0 + executed + (policy == "b0") if policy == "b0" else 0, "b0_retries": 0,
         "marker": {"valid": 1, "run": 7, "checkpoint": v.CHECKPOINTS.index(checkpoint), "txid": 3}, "reset_cause": "14004400",
         "armed_txid": 3, "boot_before": "01", "boot_after": "02", "session_before": SESSION, "session_after": "9302fe77f2c0498d",
         "retention_valid": 1, "foreign_replies": 0}
    t.update(over)
    return t


def slot(txid, counter, seq):
    w = [MAGIC, ERA, int(SESSION[8:], 16), int(SESSION[:8], 16), txid, counter, seq]
    return w + [zlib.crc32(struct.pack("<7I", *w))]


def log(run, words, result=None):
    lines = [f"104 commanded run={run} checkpoint=f3 txid=3",
             f"123 rx ANNOUNCE boot_id=02 retention_valid=1 version=1..1 era={ERA:08x} slots=" + ",".join(f"{x:08x}" for x in words)]
    return "\n".join(lines + ([result] if result else [])) + "\n"


class Verify(unittest.TestCase):
    def run_main(self, records, logs=()):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        path = os.path.join(d.name, "t.jsonl")
        with open(path, "w") as f:
            f.writelines(json.dumps(r) + "\n" for r in records)
        args = [path]
        for i, text in enumerate(logs):
            p = os.path.join(d.name, f"{i}.log")
            with open(p, "w") as f:
                f.write(text)
            args += ["--log", p]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = v.main(args)
        return rc, out.getvalue()

    def test_valid_records_pass(self):
        for policy in v.POLICIES:
            for cp in ("f1", "f2b", "f3"):
                self.assertEqual(v.judge(record(policy, cp)), ("COUNTED_PASS", ""), (policy, cp))

    def test_wrong_b1_b2_outcomes_fail(self):
        for policy in ("b1", "b2"):
            for cp, outcome in (("f1", "NONE"), ("f1", "REJECTED"), ("f3", "NONE"), ("f3", "COMPLETED"), ("f3", "NOT_EXECUTED")):
                self.assertEqual(v.judge(record(policy, cp, outcome))[0], "COUNTED_FAIL", (policy, cp, outcome))

    def test_counter_session_and_reset_evidence(self):
        self.assertEqual(v.judge(record(c2=103))[0], "COUNTED_FAIL")  # duplicate execution
        self.assertEqual(v.judge(record(foreign_replies=1))[0], "COUNTED_FAIL")
        self.assertEqual(v.judge(record(session_after=SESSION))[0], "COUNTED_FAIL")
        self.assertEqual(v.judge(record("b0", retry_counter=5))[0], "COUNTED_FAIL")
        self.assertEqual(v.judge(record(reset_cause="04004400"))[0], "INCONCLUSIVE")  # pin reset only
        for cause in ("1c004400", "34004400", "94004400", "54004400", "16004400"):  # software reset with brown-out, IWDG, LPWR, WWDG or OBL
            self.assertEqual(v.judge(record(reset_cause=cause))[0], "INCONCLUSIVE", cause)
        self.assertEqual(v.judge(record(boot_after="01"))[0], "INCONCLUSIVE")
        self.assertEqual(v.judge(record(marker={"valid": 1, "run": 7, "checkpoint": 0, "txid": 3}))[0], "INCONCLUSIVE")
        self.assertEqual(v.judge(record("b1", "f1", c0=2**32 - 1, c2=0))[0], "COUNTED_PASS")  # wraparound

    def test_stored_verdict_is_not_trusted(self):
        rc, out = self.run_main([record("b2", "f1", "NONE")])
        self.assertEqual(rc, 1)
        self.assertIn("stored COUNTED_PASS, recomputed COUNTED_FAIL", out)

    def test_duplicate_run_is_inconclusive(self):
        rc, out = self.run_main([record(), record()])
        self.assertEqual(rc, 1)
        self.assertIn("inconclusive=2", out)

    def test_snapshot_cross_check(self):
        good = slot(3, 101, 9) + slot(2, 100, 8)
        self.assertEqual(self.run_main([record()], [log(7, good)])[0], 0)
        stale = slot(2, 100, 9) + slot(1, 99, 8)  # f3 committed, but the journal does not hold t=3
        rc, out = self.run_main([record()], [log(7, stale)])
        self.assertEqual(rc, 1)
        self.assertIn("journal lacks the committed trial request", out)
        rc, out = self.run_main([record("b3", "f1")], [log(7, good)])  # f1 precedes the commit, yet t=3 is there
        self.assertIn("checkpoint precedes the commit", out)
        torn = [0] + slot(3, 101, 9)[1:] + slot(2, 100, 8)
        self.assertEqual(self.run_main([record("b3", "f2c")], [log(7, torn)])[0], 0)

    def test_policy_or_checkpoint_outside_the_campaign(self):
        self.assertEqual(v.judge(dict(record(), policy="b4"))[0], "NOT_RUN")
        self.assertEqual(v.judge(dict(record(), checkpoint="f4"))[0], "NOT_RUN")

    def test_corrupted_slot_is_not_skipped(self):
        good = slot(3, 101, 9) + slot(2, 100, 8)
        for word in range(16):  # either slot changed after it committed: the other slot cannot be read as the newest
            bad = list(good)
            bad[word] ^= 0x100 if word % 8 == 0 else 1
            self.assertEqual(v.newest(bad, ERA, MAGIC), "inconsistent")
            rc, out = self.run_main([record()], [log(7, bad)])
            self.assertEqual(rc, 1)
            self.assertIn("post-reset journal has inconsistent valid slot", out)
        cleared = [0] + good[1:]  # w0 at 0 is an empty slot, so the predecessor alone is valid
        self.assertEqual(v.newest(cleared, ERA, MAGIC), 1)

    def test_rejected_frame_is_inconclusive(self):
        good = slot(3, 101, 9) + slot(2, 100, 8)
        trial_line = "900 TRIAL run=7 f3 COUNTED_PASS outcome=COMPLETED c0=100 c2=102 \n"
        self.assertEqual(self.run_main([record()], [log(7, good) + trial_line])[0], 0)
        for where in ("before", "after"):  # inside the trial's lines, or after its commanded line with no TRIAL line
            text = log(7, good) + ("850 rx_error ERR_CRC\n" + trial_line if where == "before" else "850 rx_error ERR_CRC\n")
            rc, out = self.run_main([record()], [text])
            self.assertEqual(rc, 1)
            self.assertIn("rejected frame in the log of this trial", out)
        rc, out = self.run_main([record()], ["5 rx_error ERR_COBS\n"])
        self.assertEqual(rc, 1)
        self.assertIn("1 rejected frame(s) outside any trial", out)
        rc, out = self.run_main([record(rx_errors=2)])
        self.assertEqual(rc, 1)
        self.assertIn("rejected frame during the trial", out)
        rc, out = self.run_main([record(rx_errors=1, c2=103)])  # a duplicate execution stays a failure
        self.assertIn("oracle count", out)

    def test_log_result_count_must_match_records(self):
        text = log(7, slot(3, 101, 9) + slot(2, 100, 8), "RESULT b3 f3 COUNTED_PASS=2 COUNTED_FAIL=0 INCONCLUSIVE=0 NOT_RUN=0 io_error=0")
        rc, out = self.run_main([record()], [text])
        self.assertEqual(rc, 1)
        self.assertIn("reports 2 trials, records hold 1", out)

    def test_campaigns_sharing_a_cell_are_counted_apart(self):
        a, b = record(), record(run=8, marker={"valid": 1, "run": 8, "checkpoint": 4, "txid": 3})
        words = slot(3, 101, 9) + slot(2, 100, 8)
        result = "RESULT b3 f3 COUNTED_PASS=1 COUNTED_FAIL=0 INCONCLUSIVE=0 NOT_RUN=0 io_error=0"
        self.assertEqual(self.run_main([a, b], [log(7, words, result), log(8, words, result)])[0], 0)


if __name__ == "__main__":
    unittest.main()
