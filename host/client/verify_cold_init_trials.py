"""Independent verifier for the evidence written by cold_init_trials.py.

It imports nothing from host/client. Every record and event must match the schema of the evidence format exactly: unknown or missing fields and unknown event kinds fail. Frames are decoded again with its own COBS, CRC-32 and field layouts, taken from the encode switch in core/src/wire.c; in the formats that carry raw received bytes every received frame is decoded from them and must equal what the host recorded. Slots are judged with its own reading of the rules in core/src/journal.c and core/src/controller.c. The stored verdict of a trial is reported and compared, never trusted.

Each trial is checked against what the firmware must have produced: the frames sent, every frame received in order with nothing extra, one new boot for each commanded reset and none otherwise (also between trials), the same snapshot in every ANNOUNCE of one boot, counters one step apart from the journal state at bind, the pre-trial era (the era reported at bind when retention was valid, otherwise a new era never seen before), the slot words, the era and retention flag after the interrupted store, the markers, and the outcome of every committed request, which must equal the classification the host recorded. The record copies of the frames (after, diag, recovery) must equal the frames.

Evidence formats, chosen explicitly with --format:
v1  the v5 pilot of 2026-11-21: no raw received bytes, no recorded host classification; its received frames rest on the host parser of the time.
v2  the v5 extended campaign of 2026-11-21: raw received bytes and host classification in every trial; newest commit in slot 0.
v3  the current runner: v2 plus the layout field of a cold trial (the slot holding the newest commit), checked against --layout.

With --corrupt (format v3 only) the plan is the corrupt trials of the corrupted-slot image: the slot --layout names must hold the last commit with only bit 0 of its counter word changed, w0 intact and the CRC failing, beside its intact predecessor; the boot must report retention invalid, every committed request must classify UNKNOWN, and the board must recover as after a forge trial.

Usage: python3 -I verify_cold_init_trials.py JSONL --run N (--pilot | --trials N) [--layout 0|1] [--format v1|v2|v3] [--partial] [--corrupt]
--run, --pilot, --trials, --layout and --corrupt are the values given to cold_init_trials.py. --partial accepts a file that ends before the plan does (a campaign in progress).
Exit 0 when every expected record is present and every check holds, 1 otherwise, 2 on an argument error.
"""

import argparse
import json
import struct
import sys
import zlib

MAGIC, SFT, BOR = 0x45504301, 1 << 28, 1 << 27
CUTS, FORGE, F3, CORRUPT = (9, 10, 11, 12), 13, 4, 1
SLOTS = tuple(f"slot{i}" for i in range(16))
# type code: name, struct format and names of the fields between the 4-byte header and the CRC, in the order of core/src/wire.c
TYPES = {
    1: ("PROBE", "<Q", ("nonce",)),
    2: ("ANNOUNCE", "<QQQIIIII16I", ("boot_id", "challenge", "nonce", "reset_cause", "version_min", "version_max", "capabilities", "era") + SLOTS),
    3: ("HELLO", "<QQI", ("challenge", "nonce", "version")),
    4: ("HELLO_ACK", "<QQQ", ("challenge", "nonce", "session")),
    5: ("START", "<Q", ("session",)),
    6: ("START_ACK", "<Q", ("session",)),
    7: ("PING", "<QI", ("session", "counter")),
    8: ("PONG", "<QI", ("session", "counter")),
    9: ("NO_SESSION", "<QQ", ("boot_id", "challenge")),
    10: ("BUSY", "<I", ("busy_remaining_ms",)),
    11: ("INCREMENT", "<QI", ("session", "txid")),
    12: ("EP1_REPLY", "<QIBI", ("session", "txid", "ep1_status", "counter")),
    13: ("TEST_ARM", "<III", ("run", "checkpoint", "txid")),
    14: ("TEST_ARM_ACK", "<III", ("run", "checkpoint", "txid")),
    15: ("TEST_DIAG", "<I", ("run",)),
    16: ("TEST_DIAG_REPLY", "<BIIII", ("marker_valid", "run", "checkpoint", "txid", "reset_cause")),
}


EVENT_KEYS = {"tx": {"t", "dir", "note", "hex"}, "rx": {"t", "dir", "status", "msg"}, "rxraw": {"t", "dir", "hex"}}
TRIAL_KEYS = {"kind", "run", "code", "mode", "session", "txids", "counters", "boot_before", "after", "diag", "events", "verdict", "why"}
RECOVERY_KEYS = {"counter", "session", "txid", "run", "announce", "diag"}
FORMATS = {"v1": {"raw": False, "classes": False, "layout": False}, "v2": {"raw": True, "classes": True, "layout": False}, "v3": {"raw": True, "classes": True, "layout": True}}


def record_keys(kind, fmt):
    if kind == "sync":
        return {"kind", "events"}
    keys = TRIAL_KEYS | ({"classes"} if fmt["classes"] else set())
    return keys | {"recovery"} | ({"layout"} if kind == "corrupt" else set()) if kind in ("forge", "corrupt") else keys | ({"layout"} if fmt["layout"] else set())


class Bad(Exception):
    pass


def need(ok, why):
    if not ok:
        raise Bad(why)


def unstuff(data):
    out, i = bytearray(), 0
    while i < len(data):
        n = data[i]
        if n == 0 or i + n > len(data):
            raise ValueError("COBS")
        out += data[i + 1:i + n]
        i += n
        if n != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def decode(frame):
    """One frame without its delimiters to a message dict; raises ValueError for anything a correct sender cannot produce."""
    body = unstuff(frame)
    if len(body) < 8 or struct.unpack("<I", body[-4:])[0] != zlib.crc32(body[:-4]):
        raise ValueError("CRC")
    if body[0] != 1 or body[1] not in TYPES or body[3]:
        raise ValueError("header")
    name, fmt, names = TYPES[body[1]]
    if len(body) != 8 + struct.calcsize(fmt) or body[2] & ~(3 if name == "ANNOUNCE" else 0):
        raise ValueError("length or flags")
    m = dict(zip(names, struct.unpack(fmt, body[4:-4])), type=name)
    if name == "ANNOUNCE":
        m["nonce_echo"], m["retention_valid"] = bool(body[2] & 1), bool(body[2] & 2)
    return m


def split(data):
    """Splits a received byte stream at every 0x00; returns the decoded frames (None for a damaged one) and the bytes after the last delimiter."""
    *parts, rest = data.split(b"\0")
    out = []
    for p in parts:
        if p:
            try:
                out.append(decode(p))
            except ValueError:
                out.append(None)
    return out, rest


def slots(a):
    return [a[k] for k in SLOTS]


def crc_ok(w):
    return zlib.crc32(struct.pack("<7I", *w[:7])) == w[7]


def newest(a):
    """core/src/journal.c epoch_journal_newest under the snapshot era: slot index, None for no valid slot, or 'inconsistent' for two valid slots that break the one-step rule or an invalid slot whose w0 is not 0."""
    ws, era = slots(a), a["era"]
    v = [era != 0 and ws[8 * i] == MAGIC and ws[8 * i + 1] == era and crc_ok(ws[8 * i:8 * i + 8]) for i in (0, 1)]
    if era != 0 and any(not v[i] and ws[8 * i] != 0 for i in (0, 1)):
        return "inconsistent"
    if all(v):
        d = (ws[6] - ws[14]) % 2**32
        return 0 if d == 1 else 1 if d == 2**32 - 1 else "inconsistent"
    return 0 if v[0] else 1 if v[1] else None


def classify(a, session, txid):
    """core/src/journal.c epoch_classify on an ANNOUNCE snapshot."""
    n = newest(a)
    if not a["retention_valid"] or n is None or n == "inconsistent":
        return "UNKNOWN"
    w = slots(a)[8 * n:8 * n + 8]
    if (w[3] << 32 | w[2]) != session:
        return "UNKNOWN"
    return "COMPLETED" if w[4] == txid else "NOT_EXECUTED" if w[4] < txid else "UNKNOWN"


def record_of(session, txid, counter):
    return [session & 0xFFFFFFFF, session >> 32, txid, counter]


def plan(pilot, trials, layout=0, corrupt=False):
    """The runner's plan: with layout 1 only the cold trials, with the newest commit in slot 1; with corrupt only the corrupt trials."""
    cases = [("corrupt", FORGE, CORRUPT)] if corrupt else [("cold", cut, mode) for cut in CUTS for mode in (0, 1)] + [("forge", FORGE, mode) for mode in (0, 1)] * (layout == 0)
    return cases if pilot else [c for c in cases for _ in range(trials)]


class Exchanges:
    """A trial's frames as (sent message, [messages received until the next send])."""

    def __init__(self, ex):
        self.ex, self.i = ex, 0

    def step(self, name, *replies):
        need(self.i < len(self.ex), f"{name} never sent")
        tx, rx = self.ex[self.i]
        self.i += 1
        need(tx is not None and tx["type"] == name, f"expected {name} sent, found {tx and tx['type']}")
        got = [m["type"] if m else "DAMAGED" for m in rx]
        need(got == list(replies), f"after {name} expected {list(replies)}, received {got}")
        return tx, rx


class Verifier:
    def __init__(self, run, fmt, layout):
        self.boot, self.seen, self.snap, self.eras, self.run, self.fmt, self.layout = None, set(), {}, set(), run, fmt, layout

    def frames(self, t, info):
        """Builds the exchanges, checking every received frame against the raw bytes when they were recorded."""
        ex, raw, rx = [], bytearray(), []
        for e in t["events"]:
            need(isinstance(e, dict) and e.get("dir") in EVENT_KEYS and set(e) == EVENT_KEYS[e["dir"]], f"event not in the schema: {str(e)[:80]}")
            if e["dir"] == "rxraw":
                need(self.fmt["raw"], "raw received bytes in a format without them")
            if e["dir"] == "tx":
                try:
                    m = decode(bytes.fromhex(e["hex"]).strip(b"\0"))
                except ValueError:
                    m = None
                need(m is not None and e["note"] == m["type"], "a sent frame does not decode to its note")
                ex.append((m, []))
            elif e["dir"] == "rxraw":
                raw += bytes.fromhex(e["hex"])
            elif e["dir"] == "rx":
                need((e["status"] == "OK") == isinstance(e["msg"], dict), "rx status and message disagree")
                rx.append(e["msg"] if e["status"] == "OK" else None)
                need(ex, "frames received before the first request")
                ex[-1][1].append(rx[-1])
        info["rx"], info["rejected"] = len(rx), sum(m is None for m in rx)
        info["raw"] = any(e["dir"] == "rxraw" for e in t["events"])
        need(ex, "no frames recorded")
        need(info["rejected"] == 0, f"{info['rejected']} received frame(s) rejected by the host parser")
        if info["raw"]:
            got, rest = split(bytes(raw))
            need(None not in got, f"{got.count(None)} damaged frame(s) in the raw bytes")
            need(not rest, f"{len(rest)} byte(s) of an incomplete frame after the last delimiter")
            need(got == rx, "the recorded frames differ from the frames in the raw bytes")
        else:
            need(not self.fmt["raw"], "raw received bytes not recorded")
        return Exchanges(ex)

    def announce(self, a, nonce=None, new_boot=False):
        need(a["nonce_echo"] == (nonce is not None) and a["nonce"] == (nonce or 0), "ANNOUNCE nonce does not match the request")
        b = a["boot_id"]
        if new_boot:
            need(b not in self.seen, "no new boot after the commanded reset")
            need(a["reset_cause"] & SFT and not a["reset_cause"] & BOR, f"reset cause {a['reset_cause']:08x} is not a software reset without brownout")
            self.boot = b
        else:
            need(self.boot is None or b == self.boot, "boot changed without a commanded reset")
            self.boot = b
        self.seen.add(b)
        snap = (a["reset_cause"], a["era"], a["retention_valid"], tuple(slots(a)))
        need(self.snap.setdefault(b, snap) == snap, "two ANNOUNCEs of one boot carry different snapshots")

    def bind(self, x):
        p, (a,) = x.step("PROBE", "ANNOUNCE")
        self.announce(a, nonce=p["nonce"])
        h, (ha,) = x.step("HELLO", "HELLO_ACK")
        need(h["challenge"] == a["challenge"] and ha["challenge"] == h["challenge"] and ha["nonce"] == h["nonce"] == p["nonce"], "handshake values do not match")
        s, (sa,) = x.step("START", "START_ACK")
        need(s["session"] == sa["session"] == ha["session"], "START session does not match HELLO_ACK")
        return a, s["session"]

    def live(self, a):
        """The journal the boot of ANNOUNCE a writes to: (era or None when cold initialization must have made a new one, counter base)."""
        if not a["retention_valid"]:
            return None, 0
        n = newest(a)
        need(n != "inconsistent", "retention reported valid over an inconsistent pair")
        return a["era"], slots(a)[8 * n + 5] if isinstance(n, int) else 0

    def era_ok(self, era, live_era, a):
        if live_era is not None:
            need(era == live_era, f"pre-trial era {era:08x} is not the era {live_era:08x} reported at bind")
        else:
            need(era != 0 and era != a["era"] and era not in self.eras, f"pre-trial era {era:08x} is not a new era after cold initialization")

    def increments(self, x, session, txids, base):
        counters = []
        for i, txid in enumerate(txids):
            q, (r,) = x.step("INCREMENT", "EP1_REPLY")
            need(q["session"] == r["session"] == session and q["txid"] == r["txid"] == txid and r["ep1_status"] == 0, f"INCREMENT {txid} not committed for the session")
            need(r["counter"] == base + i + 1, f"txid {txid} committed counter {r['counter']}, expected {base + i + 1}")
            counters.append(r["counter"])
        return counters

    def arm(self, x, code, mode, *replies):
        q, rx = x.step("TEST_ARM", "TEST_ARM_ACK", *replies)
        self.run += 1
        need((q["run"], q["checkpoint"], q["txid"]) == (self.run, code, mode), f"TEST_ARM sent {q['run']} {q['checkpoint']} {q['txid']}, expected {self.run} {code} {mode}")
        need((rx[0]["run"], rx[0]["checkpoint"], rx[0]["txid"]) == (q["run"], q["checkpoint"], q["txid"]), "TEST_ARM_ACK does not echo the request")
        return q["run"], rx

    def diag(self, x, run, code, mode, cause):
        q, (d,) = x.step("TEST_DIAG", "TEST_DIAG_REPLY")
        need(q["run"] == run and d["marker_valid"] == 1 and (d["run"], d["checkpoint"], d["txid"]) == (run, code, mode), f"marker does not name run {run} checkpoint {code} txid {mode}")
        need(d["reset_cause"] == cause, "marker reset cause differs from the boot ANNOUNCE")
        return d

    def committed(self, b, s, txid, counter, info):
        """The commit of txid by an F3 reset: valid retention, newest slot (s, txid, counter), the other slot its predecessor one seq step below."""
        need(b["retention_valid"], "retention invalid after the F3 reset")
        n = newest(b)
        need(isinstance(n, int), "no newest slot after the F3 reset")
        ws = slots(b)
        need(ws[8 * n + 2:8 * n + 6] == record_of(s, txid, counter) and ws[8 * (1 - n) + 2:8 * (1 - n) + 6] == record_of(s, txid - 1, counter - 1), "slots do not hold the commit and its predecessor")
        info["classes"] = [classify(b, s, txid - 1), classify(b, s, txid)]
        need(info["classes"] == ["UNKNOWN", "COMPLETED"], f"committed txid classified {info['classes']}")

    def f3(self, x, s, txid, base, live_era, a, info, lagging=False):
        run, _ = self.arm(x, F3, txid)
        q, (b,) = x.step("INCREMENT", "ANNOUNCE")
        need(q["session"] == s and q["txid"] == txid, "INCREMENT after the F3 arm differs")
        self.announce(b, new_boot=True)
        self.era_ok(b["era"], live_era, a)
        if lagging and isinstance(newest(b), int):
            # A sync binds with the boot-time snapshot, so commits made later in that boot put the live counter above it, never below
            base = max(base, slots(b)[8 * newest(b) + 5] - 1)
        self.committed(b, s, txid, base + 1, info)
        self.bind(x)
        return run, b, self.diag(x, run, F3, txid, b["reset_cause"])

    def sync(self, t, info):
        x = self.frames(t, info)
        a, s = self.bind(x)
        era, base = self.live(a)
        self.f3(x, s, 1, base, era, a, info, lagging=True)
        return x

    def cold(self, t, info):
        x = self.frames(t, info)
        cut, mode, txids = t["code"], t["mode"], t["txids"]
        a, s = self.bind(x)
        need(a["boot_id"] == t["boot_before"] and s == t["session"], "record boot or session differs from the frames")
        era, base = self.live(a)
        need(txids in ([1, 2], [1, 2, 3]), f"txids {txids}")
        counters = self.increments(x, s, txids, base)
        need(counters == t["counters"], "recorded counters differ from the replies")
        run, after = self.arm(x, cut, mode, "TEST_DIAG_REPLY", "ANNOUNCE")
        _, p, b = after
        need(run == t["run"] and t["after"] == after, "record run or frames differ from the TEST_ARM exchange")
        need(p["marker_valid"] == 0 and (p["run"], p["checkpoint"], p["txid"]) == (run, cut, mode), "preparatory report does not carry the arm")
        need(p["reset_cause"] & SFT and not p["reset_cause"] & BOR, f"preparatory reset cause {p['reset_cause']:08x} is not a software reset without brownout")
        info["prep_cause"] = p["reset_cause"]
        self.announce(b, new_boot=True)
        ws, k, n = slots(b), len(txids), self.layout
        need(not self.fmt["layout"] or t["layout"] == n, f"record layout {t.get('layout')} differs from --layout {n}")
        new, old = ws[8 * n:8 * n + 8], ws[8 * (1 - n):8 * (1 - n) + 8]
        info.update(layout=n, boot_cause=b["reset_cause"], era_before=ws[1], era_after=b["era"], retention_valid=b["retention_valid"], w0=f"{ws[0]:08x},{ws[8]:08x}")
        self.era_ok(ws[1], era, a)
        rn, ro = [MAGIC, ws[1]] + record_of(s, k, counters[-1]) + new[6:], [MAGIC, ws[1]] + record_of(s, k - 1, counters[-2]) + old[6:]
        need(new[1:] == rn[1:] and old[1:] == ro[1:] and crc_ok(rn) and crc_ok(ro), f"slots do not hold the last two commits intact with the newest in slot {n}")
        need((new[6] - old[6]) % 2**32 == 1, f"slot {n} is not one seq step above slot {1 - n}")
        need((ws[0], ws[8]) == {9: (MAGIC, MAGIC), 10: (0, MAGIC), 11: (0, 0), 12: (0, 0)}[cut], "slot w0 words do not match the interrupted store")
        need(b["era"] == 0 if cut < 12 else b["era"] not in self.eras | {0, ws[1]}, f"era {b['era']:08x} does not match the interrupted store")
        need(b["retention_valid"] == (mode == 1 and b["era"] != 0 and newest(b) != "inconsistent"), "retention flag does not match the store and mode")
        info["classes"] = [classify(b, s, txid) for txid in txids]
        need(info["classes"] == ["UNKNOWN"] * k, f"committed requests classified {info['classes']}")
        need(not self.fmt["classes"] or t["classes"] == info["classes"], f"host classification {t.get('classes')} differs from {info['classes']}")
        self.bind(x)
        need(t["diag"] == self.diag(x, run, cut, mode, b["reset_cause"]), "record diag differs from the frames")
        return x

    def forge(self, t, info):
        x = self.frames(t, info)
        mode = t["mode"]
        a, s = self.bind(x)
        need(a["boot_id"] == t["boot_before"] and s == t["session"] and t["txids"] == [1], "record boot, session or txids differ from the frames")
        era, base = self.live(a)
        counters = self.increments(x, s, [1], base)
        need(counters == t["counters"], "recorded counters differ from the replies")
        run, after = self.arm(x, FORGE, mode, "ANNOUNCE")
        b = after[1]
        need(run == t["run"] and t["after"] == after, "record run or frames differ from the TEST_ARM exchange")
        self.announce(b, new_boot=True)
        ws = slots(b)
        info.update(boot_cause=b["reset_cause"], era_before=b["era"], era_after=b["era"], retention_valid=b["retention_valid"], w0=f"{ws[0]:08x},{ws[8]:08x}")
        self.era_ok(b["era"], era, a)
        need(all(ws[8 * i] == MAGIC and ws[8 * i + 1] == b["era"] and crc_ok(ws[8 * i:8 * i + 8]) for i in (0, 1)), "a forged slot is not valid on its own")
        need(ws[2:6] == ws[10:14] == record_of(s, 1, counters[0]), "the pair is not a copy of the commit")
        step = (ws[6] - ws[14]) % 2**32
        need(step in ((2, 2**32 - 2) if mode == 0 else (0,)), f"seq step {step} does not match forge mode {mode}")
        need(newest(b) == "inconsistent" and not b["retention_valid"], "inconsistent pair accepted as valid retention")
        info["classes"] = [classify(b, s, 1)]
        need(info["classes"] == ["UNKNOWN"], f"classification {info['classes']}")
        need(not self.fmt["classes"] or t["classes"] == info["classes"], f"host classification {t.get('classes')} differs from {info['classes']}")
        self.recovery(x, t, run, mode, b, info)
        return x

    def corrupt(self, t, info):
        x = self.frames(t, info)
        txids, n = t["txids"], self.layout
        a, s = self.bind(x)
        need(a["boot_id"] == t["boot_before"] and s == t["session"] and t["layout"] == n, "record boot, session or layout differ from the frames or --layout")
        era, base = self.live(a)
        need(txids in ([1, 2], [1, 2, 3]), f"txids {txids}")
        counters = self.increments(x, s, txids, base)
        need(counters == t["counters"], "recorded counters differ from the replies")
        run, after = self.arm(x, FORGE, CORRUPT, "ANNOUNCE")
        b = after[1]
        need(run == t["run"] and t["after"] == after, "record run or frames differ from the TEST_ARM exchange")
        self.announce(b, new_boot=True)
        ws, k = slots(b), len(txids)
        new, old = ws[8 * n:8 * n + 8], ws[8 * (1 - n):8 * (1 - n) + 8]
        info.update(layout=n, boot_cause=b["reset_cause"], era_before=b["era"], era_after=b["era"], retention_valid=b["retention_valid"], w0=f"{ws[0]:08x},{ws[8]:08x}")
        self.era_ok(b["era"], era, a)
        fixed = new[:5] + [new[5] ^ 1] + new[6:]
        need(new[:2] == [MAGIC, b["era"]] and fixed[2:6] == record_of(s, k, counters[-1]) and crc_ok(fixed) and not crc_ok(new), f"slot {n} is not the newest commit with only bit 0 of its counter word changed")
        need(old[:2] == [MAGIC, b["era"]] and old[2:6] == record_of(s, k - 1, counters[-2]) and crc_ok(old) and (new[6] - old[6]) % 2**32 == 1, f"slot {1 - n} is not the intact predecessor one seq step below")
        need(newest(b) == "inconsistent" and not b["retention_valid"], "corrupted slot accepted as valid retention")
        info["classes"] = [classify(b, s, txid) for txid in txids]
        need(info["classes"] == ["UNKNOWN"] * k, f"committed requests classified {info['classes']}")
        need(t["classes"] == info["classes"], f"host classification {t.get('classes')} differs from {info['classes']}")
        self.recovery(x, t, run, CORRUPT, b, info)
        return x

    def recovery(self, x, t, run, mode, b, info):
        """After the forged or corrupted pair of ANNOUNCE b: the marker, a new session committing from counter 1, and that commit kept by an F3 reset under a new era."""
        a2, s2 = self.bind(x)
        need(t["diag"] == self.diag(x, run, FORGE, mode, b["reset_cause"]), "record diag differs from the frames")
        era2, base2 = self.live(a2)
        c = self.increments(x, s2, [1], base2)
        r = t["recovery"]
        need(set(r) == RECOVERY_KEYS, f"recovery fields {sorted(r)}")
        need(c == [1] and r["counter"] == 1 and r["session"] == s2 and r["txid"] == 2, "recovery session did not commit from counter 1")
        rinfo = {}
        rrun, bb, rd = self.f3(x, s2, 2, 1, era2, a2, rinfo)
        need(rrun == r["run"] and bb["era"] != b["era"], "recovery F3 reset not under a new era")
        need(r["announce"] == bb and r["diag"] == rd, "record recovery frames differ from the frames")
        info["recovery"] = f"counter 1, era {bb['era']:08x}, {rinfo['classes']}"

    def check(self, t, kind, info):
        want = record_keys(kind, self.fmt)
        need(set(t) == want, f"fields missing {sorted(want - set(t))}, unexpected {sorted(set(t) - want)}")
        x = getattr(self, kind)(t, info)
        need(x.i == len(x.ex), f"{len(x.ex) - x.i} unexpected request(s) after the trial")
        for e in t["events"]:
            m = e.get("msg") or {}
            if m.get("type") == "ANNOUNCE":
                self.eras.update({m["era"], m["slot1"], m["slot9"]} - {0})


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("jsonl")
    ap.add_argument("--run", type=int, required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--pilot", action="store_true")
    g.add_argument("--trials", type=int)
    ap.add_argument("--layout", type=int, choices=(0, 1), default=0)
    ap.add_argument("--format", choices=sorted(FORMATS), default="v3")
    ap.add_argument("--partial", action="store_true")
    ap.add_argument("--corrupt", action="store_true")
    args = ap.parse_args(argv)
    if (args.layout or args.corrupt) and not FORMATS[args.format]["layout"]:
        ap.error("--layout 1 and --corrupt need format v3")
    expected = [("sync", None, None)] + plan(args.pilot, args.trials, args.layout, args.corrupt)
    v, failures = Verifier(args.run, FORMATS[args.format], args.layout), 0
    print(f"format {args.format}: {'raw received bytes' if FORMATS[args.format]['raw'] else 'NO raw received bytes, frames as decoded by the host'}, {'host classification required' if FORMATS[args.format]['classes'] else 'NO host classification recorded'}, newest commit in slot {args.layout}")
    with open(args.jsonl) as f:
        recs = [json.loads(line) for line in f if line.strip()]
    for i, t in enumerate(recs):
        info, kind = {}, t.get("kind")
        try:
            need(i < len(expected), "record beyond the plan")
            need(kind == expected[i][0] and (kind == "sync" or (t.get("code"), t.get("mode")) == expected[i][1:]), f"record is {kind} {t.get('code')} {t.get('mode')}, plan expects {expected[i]}")
            v.check(t, kind, info)
            need(kind == "sync" or t.get("verdict") == "COUNTED_PASS", f"stored verdict {t.get('verdict')} disagrees")
            result = "PASS"
        except (Bad, KeyError, TypeError, IndexError, AttributeError, ValueError) as exc:
            result, failures = f"FAIL {exc if isinstance(exc, Bad) else repr(exc)}", failures + 1
        fields = " ".join(f"{k}={v:08x}" if isinstance(v, int) and not isinstance(v, bool) and k.endswith(("cause", "era_before", "era_after")) else f"{k}={v}" for k, v in info.items())
        print(f"{i:02d} {kind} code={t.get('code')} mode={t.get('mode')} run={t.get('run')} stored={t.get('verdict')} independent={result} {fields}".rstrip())
    missing = len(expected) - len(recs)
    if missing > 0 and not args.partial:
        print(f"FAIL {missing} planned record(s) missing")
        failures += 1
    print(f"records {len(recs)} of {len(expected)} planned, {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
