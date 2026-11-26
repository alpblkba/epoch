"""Forced cold-initialization interruption and inconsistent-pair trials against the fault build (fault.conf), using the independent Python codec.

These are reset-injection experiments with the backup domain powered throughout; they are not physical power-loss retention experiments. A cold trial commits two or three consecutive records of one session so that the slot chosen by --layout (0 by default) holds the newest and the other slot its predecessor, arms TEST_ARM with a cold-initialization store code (9 to 12) and a mode (0: the next boot is judged cold by the firmware marker, 1: judged warm, so only the era guard protects it). The firmware acknowledges, resets (preparatory reset), takes the arm at the next boot and reports it with an unsolicited TEST_DIAG_REPLY, runs cold initialization, writes the checkpoint marker at the armed store and resets again (injected reset); the boot after it sends the ANNOUNCE whose raw snapshot is judged. A forge trial (code 13, mode 0: seq + 2, mode 1: equal seq) copies the newest record into the other slot and resets warm; the next ANNOUNCE must report retention invalid with the raw inconsistent pair, and a new session must commit from counter 1 and survive an F3 reset under a new era.

Each trial is judged from its own frames, raw received bytes included: acknowledgement, preparatory and injected reset identified apart by their reset causes, the marker, the raw snapshot against the store or the forge, and a Python classification of the committed requests. A trial with a rejected frame or a boot that no commanded reset explains, inside the trial or since the one before, is INCONCLUSIVE. The run stops at the first trial that is not PASS, and with --verifier also when the independent verifier (verify_cold_init_trials.py, run on the file after every record) does not pass it. Every record is appended to <evidence>/cold_init_trials.jsonl and synced to disk before the next trial starts.

With --layout 1 the plan holds only the cold trials, as the forge trials do not depend on the layout.

With --corrupt the plan holds only corrupt trials, for the corrupted-slot image (corrupt.conf), where forge mode 1 changes bit 0 of the counter word of the newest slot instead of writing an equal-seq copy. The commits place the newest record in the slot --layout names; the next ANNOUNCE must show that slot with w0 intact and a failing CRC beside its intact predecessor, report retention invalid and classify every committed request UNKNOWN, and the board must recover as after a forge trial. Never run the standard plan on that image, or its forge mode 1 trials would see a corrupted slot.

A trial interrupted by a protocol, decoding or serial transport failure is not judged: its frames, binds, the TEST_ARM writes already started and the reason go to their own interrupted_<UTC>.json, synced to disk, so cold_init_trials.jsonl holds only complete records, and the run stops. Any other exception is recorded the same way and re-raised. Nothing is retried.

Usage: python3 cold_init_trials.py --port P --evidence DIR --run N (--pilot | --trials N) [--layout 0|1] [--verifier PATH] [--corrupt]
Exit 0 when every trial passed, 1 otherwise, 2 on an argument or open error.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import termios
import time
import zlib

from ep0_adversarial import Link, first
from verify_trials import newest

MAGIC = 0x45504301
SFTRSTF, BORRSTF = 1 << 28, 1 << 27
CUTS, FORGE, F3, CORRUPT = (9, 10, 11, 12), 13, 4, 1
WARM = 1


def words(ann):
    return [ann[f"slot{i}"] for i in range(16)]


def valid(slot, era):
    return slot[0] == MAGIC and slot[1] == era and zlib.crc32(struct.pack("<7I", *slot[:7])) == slot[7]


def session(slot):
    return slot[3] << 32 | slot[2]


def classify(retention_valid, ws, era, s, txid):
    """The rule of epoch_classify in core/src/journal.c, written again here so the verdict does not depend on the C code."""
    n = newest(ws, era, MAGIC) if retention_valid else None
    if n is None or n == "inconsistent":
        return "UNKNOWN"
    w = ws[8 * n:8 * n + 8]
    if session(w) != s:
        return "UNKNOWN"
    return "COMPLETED" if w[4] == txid else "NOT_EXECUTED" if w[4] < txid else "UNKNOWN"


def boot_ids(events):
    """Boot IDs in the order the ANNOUNCEs of events show them, each change counted once."""
    ids = [e["msg"]["boot_id"] for e in events if e["dir"] == "rx" and e["msg"] and e["msg"]["type"] == "ANNOUNCE"]
    return [b for i, b in enumerate(ids) if i == 0 or b != ids[i - 1]]


def resets(t, entry_expected):
    """Checks the acknowledgement, the preparatory reset report and the single boot after the injected reset; returns (ANNOUNCE, '') or (None, reason)."""
    acks = [m for m in t["after"] if m["type"] == "TEST_ARM_ACK"]
    entries = [m for m in t["after"] if m["type"] == "TEST_DIAG_REPLY"]
    boots = [m for m in t["after"] if m["type"] == "ANNOUNCE" and not m["nonce_echo"]]
    if any(e["dir"] == "rx" and e["status"] != "OK" for e in t.get("events", ())):
        return None, "a frame of the trial was rejected by the parser"
    if "events" in t:  # one new boot for the injected reset, one more for a forge trial's F3 reset, and no other
        ids = boot_ids(t["events"])
        if ids[:1] != [t["boot_before"]] or len(set(ids)) != len(ids) or len(ids) != (2 if entry_expected else 3):
            return None, "a boot not explained by the commanded resets"
    if len(acks) != 1 or (acks[0]["run"], acks[0]["checkpoint"], acks[0]["txid"]) != (t["run"], t["code"], t["mode"]):
        return None, "TEST_ARM not acknowledged once with the commanded values"
    if entry_expected:
        e = entries[0] if len(entries) == 1 else None
        if not e or e["marker_valid"] or (e["run"], e["checkpoint"], e["txid"]) != (t["run"], t["code"], t["mode"]) or not e["reset_cause"] & SFTRSTF or e["reset_cause"] & BORRSTF:
            return None, "preparatory reset not reported once with SFTRSTF and the arm"
    elif entries:
        return None, "unexpected TEST_DIAG_REPLY"
    if len(boots) != 1:
        return None, f"{len(boots)} boots after the injected reset, expected 1"
    a = boots[0]
    if not a["reset_cause"] & SFTRSTF or a["reset_cause"] & BORRSTF or a["boot_id"] == t["boot_before"]:
        return None, "injected reset not shown by SFTRSTF and a new boot ID"
    d = t["diag"]
    if not d or not d["marker_valid"] or (d["run"], d["checkpoint"], d["txid"]) != (t["run"], t["code"], t["mode"]):
        return None, "checkpoint marker does not name the commanded store"
    return a, ""


def judge_cold(t):
    a, why = resets(t, True)
    if not a:
        return "INCONCLUSIVE", why
    ws, era, cut, n = words(a), a["era"], t["code"], t["layout"]
    s0, s1 = ws[:8], ws[8:]
    old = s1[1]
    rn, ro = [MAGIC] + ws[8 * n + 1:8 * n + 8], [MAGIC] + ws[8 * (1 - n) + 1:8 * (1 - n) + 8]
    if not (old and valid(rn, old) and valid(ro, old) and (rn[6] - ro[6]) % 2**32 == 1 and session(rn) == session(ro) == t["session"]
            and rn[4] == t["txids"][-1] and ro[4] == t["txids"][-2] and rn[5] == t["counters"][-1]):
        return "INCONCLUSIVE", f"slot {n} did not hold the newest commit one step above the previous record of the session"
    if (s0[0], s1[0]) != {9: (MAGIC, MAGIC), 10: (0, MAGIC), 11: (0, 0), 12: (0, 0)}[cut]:
        return "COUNTED_FAIL", "slot w0 words do not match the interrupted store"
    if (cut < 12 and era != 0) or (cut == 12 and era in (0, old)):
        return "COUNTED_FAIL", f"snapshot era {era:08x} does not match the interrupted store"
    if a["retention_valid"] != (cut == 12 and t["mode"] == WARM):
        return "COUNTED_FAIL", "retention_valid does not match the store and mode"
    t["classes"] = [classify(a["retention_valid"], ws, era, t["session"], tx) for tx in t["txids"]]
    for tx, o in zip(t["txids"], t["classes"]):
        if o != "UNKNOWN":
            return "COUNTED_FAIL", f"txid {tx} classified {o}"
    return "COUNTED_PASS", ""


def judge_forge(t):
    a, why = resets(t, False)
    if not a:
        return "INCONCLUSIVE", why
    ws, era = words(a), a["era"]
    s0, s1 = ws[:8], ws[8:]
    if s0[:6] != s1[:6] or session(s0) != t["session"] or s0[4] != t["txids"][-1] or s0[5] != t["counters"][-1]:
        return "INCONCLUSIVE", "the forged pair is not a copy of the last commit"
    if not (valid(s0, era) and valid(s1, era)):
        return "COUNTED_FAIL", "a forged slot is not valid on its own"
    if newest(ws, era, MAGIC) != "inconsistent" or (s0[6] - s1[6]) % 2**32 not in ({2, 2**32 - 2} if t["mode"] == 0 else {0}):
        return "COUNTED_FAIL", "the pair does not break the one-step rule as the mode requires"
    t["classes"] = [classify(a["retention_valid"], ws, era, t["session"], t["txids"][-1])]
    if a["retention_valid"] or t["classes"] != ["UNKNOWN"]:
        return "COUNTED_FAIL", "inconsistent pair accepted as valid retention"
    why = recovered(t, era)
    return ("COUNTED_FAIL", why) if why else ("COUNTED_PASS", "")


def judge_corrupt(t):
    a, why = resets(t, False)
    if not a:
        return "INCONCLUSIVE", why
    ws, era, n = words(a), a["era"], t["layout"]
    rn, ro = ws[8 * n:8 * n + 8], ws[8 * (1 - n):8 * (1 - n) + 8]
    fixed = rn[:5] + [rn[5] ^ 1] + rn[6:]
    if not (rn[0] == MAGIC and valid(fixed, era) and valid(ro, era) and (rn[6] - ro[6]) % 2**32 == 1 and session(rn) == session(ro) == t["session"]
            and rn[4] == t["txids"][-1] and ro[4] == t["txids"][-2] and fixed[5] == t["counters"][-1]):
        return "INCONCLUSIVE", f"slot {n} is not the newest commit with bit 0 of its counter word changed beside its predecessor"
    t["classes"] = [classify(a["retention_valid"], ws, era, t["session"], tx) for tx in t["txids"]]
    if a["retention_valid"] or t["classes"] != ["UNKNOWN"] * len(t["txids"]):
        return "COUNTED_FAIL", "corrupted slot accepted as valid retention"
    why = recovered(t, era)
    return ("COUNTED_FAIL", why) if why else ("COUNTED_PASS", "")


def recovered(t, era):
    """'' when the session after a forged or corrupted pair committed from counter 1 and kept that commit over an F3 reset under an era other than era, otherwise the failure."""
    r = t["recovery"]
    if r["counter"] != 1:
        return f"first commit after recovery reported counter {r['counter']}, expected 1"
    b, d = r["announce"], r["diag"]
    if not b or not b["retention_valid"] or b["era"] in (0, era) or not d or not d["marker_valid"] or (d["run"], d["checkpoint"], d["txid"]) != (r["run"], F3, r["txid"]):
        return "no valid retention under a new era after the F3 reset"
    rs = words(b)
    n = newest(rs, b["era"], MAGIC)
    if n is None or n == "inconsistent" or session(rs[8 * n:8 * n + 8]) != r["session"] or rs[8 * n + 4] != r["txid"] or rs[8 * n + 5] != 2:
        return "the commit after recovery did not survive the F3 reset"
    return ""


class Board:
    """Drives the trials; live is the slot holding the newest valid record of the live journal (None for no valid slot), tracked from each boot snapshot and every publish since."""

    def __init__(self, port, log, run):
        self.port, self.log, self.run, self.live = port, log, run, None
        self.last, self.binds, self.arm_sent = None, [], []

    def link(self):
        self.last = Link(self.port, self.log)  # kept so a trial that stops on an exception still records its frames
        self.binds, self.arm_sent = [], []
        return self.last

    def next_run(self):
        self.run += 1
        return self.run

    def bind(self, l):
        boot, s = l.bind()
        self.binds.append({"boot": boot, "session": s})
        self.publish()  # START writes SESSION_OPEN
        return boot, s

    def publish(self):
        self.live = 0 if self.live is None else 1 - self.live

    def increment(self, l, s, txid):
        l.send(type="INCREMENT", session=s, txid=txid)
        r = first(l.recv(1.0, lambda g: any(m["type"] == "EP1_REPLY" for m in g)), "EP1_REPLY")
        assert r and r["txid"] == txid and r["ep1_status"] == 0, f"EP1_REPLY {r}"
        self.publish()
        return r["counter"]

    def arm(self, l, run, checkpoint, txid):
        self.arm_sent.append({"run": run, "checkpoint": checkpoint, "txid": txid})  # before the write: a failed write may still have reached the board
        l.send(type="TEST_ARM", run=run, checkpoint=checkpoint, txid=txid)

    def diag(self, l, run):
        l.send(type="TEST_DIAG", run=run)
        return first(l.recv(1.0, lambda g: any(m["type"] == "TEST_DIAG_REPLY" for m in g)), "TEST_DIAG_REPLY")

    def f3_reset(self, l, s, txid):
        """Commits txid with a reset armed at F3; returns (run, boot ANNOUNCE, marker) and resynchronizes live from the snapshot."""
        run = self.next_run()
        self.arm(l, run, F3, txid)
        assert first(l.recv(1.0, lambda g: any(m["type"] == "TEST_ARM_ACK" for m in g)), "TEST_ARM_ACK"), "F3 TEST_ARM not acknowledged"
        l.send(type="INCREMENT", session=s, txid=txid)
        boots = [m for m in l.recv(2.0) if m["type"] == "ANNOUNCE" and not m["nonce_echo"]]
        a = boots[0] if len(boots) == 1 else None
        if a:
            n = newest(words(a), a["era"], MAGIC) if a["retention_valid"] else None
            self.live = None if n == "inconsistent" else n
        self.bind(l)
        return run, a, self.diag(l, run)

    def sync(self):
        l = self.link()
        _, s = self.bind(l)
        run, a, d = self.f3_reset(l, s, 1)
        assert a and a["retention_valid"] and d and d["marker_valid"] and d["run"] == run, "sync F3 reset not confirmed"
        return l.events

    def cold(self, cut, mode, layout):
        l = self.link()
        boot, s = self.bind(l)
        txids = [1, 2] if self.live == layout else [1, 2, 3]  # every commit alternates the slot, so the last lands in slot layout
        counters = [self.increment(l, s, t) for t in txids]
        t = {"kind": "cold", "run": self.next_run(), "code": cut, "mode": mode, "layout": layout, "session": s, "txids": txids, "counters": counters, "boot_before": boot}
        self.arm(l, t["run"], cut, mode)
        t["after"] = l.recv(2.0)
        self.live = None  # cold initialization left no valid slot
        self.bind(l)
        t["diag"] = self.diag(l, t["run"])
        t["events"] = l.events
        return t

    def forge(self, mode):
        l = self.link()
        boot, s = self.bind(l)
        counters = [self.increment(l, s, 1)]
        t = {"kind": "forge", "run": self.next_run(), "code": FORGE, "mode": mode, "session": s, "txids": [1], "counters": counters, "boot_before": boot}
        self.arm(l, t["run"], FORGE, mode)
        t["after"] = l.recv(2.0)
        self.recover(l, t)
        return t

    def corrupt(self, layout):
        l = self.link()
        boot, s = self.bind(l)
        txids = [1, 2] if self.live == layout else [1, 2, 3]  # as in cold: the last commit lands in slot layout
        counters = [self.increment(l, s, t) for t in txids]
        t = {"kind": "corrupt", "run": self.next_run(), "code": FORGE, "mode": CORRUPT, "layout": layout, "session": s, "txids": txids, "counters": counters, "boot_before": boot}
        self.arm(l, t["run"], FORGE, CORRUPT)
        t["after"] = l.recv(2.0)
        self.recover(l, t)
        return t

    def recover(self, l, t):
        """After a forged or corrupted pair: the marker, then a new session that commits txid 1 and txid 2 under an F3 reset."""
        self.live = None
        _, s2 = self.bind(l)
        t["diag"] = self.diag(l, t["run"])
        counter = self.increment(l, s2, 1)
        run, a, d = self.f3_reset(l, s2, 2)
        t["recovery"] = {"counter": counter, "session": s2, "txid": 2, "run": run, "announce": a, "diag": d}
        t["events"] = l.events


def plan(pilot, trials, layout=0, corrupt=False):
    cases = [("corrupt", FORGE, CORRUPT)] if corrupt else [("cold", cut, mode) for cut in CUTS for mode in (0, 1)] + [("forge", FORGE, mode) for mode in (0, 1)] * (layout == 0)
    return cases if pilot else [c for c in cases for _ in range(trials)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--evidence", required=True, help="existing directory; cold_init_trials.jsonl is appended")
    ap.add_argument("--run", type=int, required=True, help="run number before the first TEST_ARM of this invocation")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--pilot", action="store_true", help="one trial per store, mode and forge mode")
    g.add_argument("--trials", type=int, help="trials per store, mode and forge mode")
    ap.add_argument("--layout", type=int, choices=(0, 1), default=0, help="slot holding the newest commit when a cold trial is armed")
    ap.add_argument("--verifier", help="verify_cold_init_trials.py, run on the evidence after every record")
    ap.add_argument("--corrupt", action="store_true", help="corrupt trials only, for the corrupted-slot image (corrupt.conf)")
    args = ap.parse_args(argv)
    if not os.path.isdir(args.evidence):
        ap.error("evidence must be an existing directory")
    import serial

    try:
        port = serial.Serial(args.port, 115200, timeout=0.02, write_timeout=2.0, exclusive=True)
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL open {args.port}: {exc}")
        return 2
    path = os.path.join(args.evidence, "cold_init_trials.jsonl")
    with port, open(path, "a") as out:
        port.reset_input_buffer()
        b = Board(port, out, args.run)

        def record(t):
            out.write(json.dumps(t, default=str) + "\n")
            out.flush()
            os.fsync(out.fileno())
            if not args.verifier:
                return True
            v = subprocess.run([sys.executable, "-I", args.verifier, path, "--run", str(args.run), *(["--pilot"] if args.pilot else ["--trials", str(args.trials)]), "--layout", str(args.layout), "--format", "v3", "--partial", *(["--corrupt"] if args.corrupt else [])], capture_output=True, text=True)
            if v.returncode:
                print("INCONCLUSIVE independent verifier:", v.stdout.strip().splitlines()[-2:], v.stderr.strip()[-300:])
            return v.returncode == 0

        def attempt(index, kind, code, mode, fn):
            """Runs one trial. An interrupted trial is written with every captured frame to its own file and fsynced; only protocol, decoding and transport failures return None, anything else is re-raised."""
            try:
                return fn()
            except BaseException as exc:
                f = os.path.join(args.evidence, f"interrupted_{time.strftime('%Y-%m-%dT%H-%M-%SZ', time.gmtime())}.json")
                with open(f, "x") as fh:
                    json.dump({"index": index, "kind": kind, "code": code, "mode": mode, "layout": args.layout, "verdict": "INCONCLUSIVE", "reason": repr(exc), "last_run": b.run,
                               "binds": b.binds, "arm_sent": b.arm_sent, "events": b.last.events if b.last else []}, fh, default=str)
                    fh.flush()
                    os.fsync(fh.fileno())
                d = os.open(args.evidence, os.O_RDONLY)
                os.fsync(d)
                os.close(d)
                print(f"INCONCLUSIVE {kind} code={code} mode={mode} interrupted by {exc!r}, TEST_ARM written for {[a['run'] for a in b.arm_sent]}, evidence {f}")
                if not isinstance(exc, (AssertionError, KeyError, TypeError, OSError, termios.error)):  # OSError includes serial.SerialException
                    raise
                return None

        def judged(t):
            t["verdict"], t["why"] = {"cold": judge_cold, "forge": judge_forge, "corrupt": judge_corrupt}[t["kind"]](t)
            return t

        events = attempt(0, "sync", None, None, b.sync)
        if events is None:
            return 1
        sync = {"kind": "sync", "events": events}
        if not record(sync):
            return 1
        prev = boot_ids(sync["events"])[-1]
        for index, (kind, code, mode) in enumerate(plan(args.pilot, args.trials, args.layout, args.corrupt), 1):
            t = attempt(index, kind, code, mode, lambda: judged(b.cold(code, mode, args.layout) if kind == "cold" else b.forge(mode) if kind == "forge" else b.corrupt(args.layout)))
            if t is None:
                return 1
            if t["verdict"] == "COUNTED_PASS" and t["boot_before"] != prev:
                t["verdict"], t["why"] = "INCONCLUSIVE", "the board reset between trials"
            verified = record(t)
            print(f"{t['verdict']} {kind} code={code} mode={mode} run={t.get('run')} {t['why']}".rstrip())
            if t["verdict"] != "COUNTED_PASS" or not verified:
                return 1
            prev = boot_ids(t["events"])[-1]
    return 0


if __name__ == "__main__":
    sys.exit(main())
