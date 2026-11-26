"""Adversarial session-control (EP0) and transaction (EP1) checks against the board controller, using the independent Python codec (epoch_wire.py), so every case also checks interoperability with the C codec in the firmware.

Each case states the controller response it expects from core/src/controller.c and records every frame sent and received in <evidence>/adversarial.jsonl. A case is PASS, FAIL or OBSERVED (behavior recorded without a verdict). Exit 0 when no case failed, 1 otherwise, 2 on an argument or open error.
"""

import argparse
import json
import os
import secrets
import struct
import sys
import time
import zlib

import epoch_wire as w

T_PENDING_S = 2.0  # firmware cfg.t_pending_ms


class Link:
    def __init__(self, port, log):
        self.port, self.log, self.parser, self.events = port, log, w.Parser(), []

    def raw(self, data, note="raw"):
        self.events.append({"t": round(time.monotonic(), 4), "dir": "tx", "note": note, "hex": data.hex()})
        self.port.write(data)
        self.port.flush()

    def send(self, **msg):
        self.raw(w.encode(msg), msg["type"])

    def recv(self, timeout=0.3, until=None):
        """Collects decoded frames for timeout seconds, or until until(msgs) holds."""
        got, deadline = [], time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.port.read(self.port.in_waiting or 1)
            if data:
                self.events.append({"t": round(time.monotonic(), 4), "dir": "rxraw", "hex": data.hex()})
            for status, msg in self.parser.feed(data):
                self.events.append({"t": round(time.monotonic(), 4), "dir": "rx", "status": status, "msg": msg})
                if msg:
                    got.append(msg)
            if until and until(got):
                break
        return got

    def bind(self):
        """Full handshake as a fresh host; returns (boot_id, session) or raises AssertionError."""
        nonce = secrets.randbits(64)
        self.send(type="PROBE", nonce=nonce)
        ann = first(self.recv(1.0, lambda g: any(m["type"] == "ANNOUNCE" for m in g)), "ANNOUNCE")
        assert ann and ann["nonce_echo"] and ann["nonce"] == nonce, f"ANNOUNCE {ann}"
        self.send(type="HELLO", challenge=ann["challenge"], nonce=nonce, version=1)
        ack = first(self.recv(1.0, lambda g: any(m["type"] == "HELLO_ACK" for m in g)), "HELLO_ACK")
        assert ack and ack["nonce"] == nonce, f"HELLO_ACK {ack}"
        self.send(type="START", session=ack["session"])
        sa = first(self.recv(1.0, lambda g: any(m["type"] == "START_ACK" for m in g)), "START_ACK")
        assert sa and sa["session"] == ack["session"], f"START_ACK {sa}"
        return ann["boot_id"], ack["session"]


def first(msgs, kind):
    return next((m for m in msgs if m["type"] == kind), None)


def kinds(msgs):
    return [m["type"] for m in msgs]


def resealed(frame_fn):
    """Builds a frame from a raw body produced by frame_fn(body_without_crc) with a valid CRC."""
    body = frame_fn()
    body += struct.pack("<I", zlib.crc32(body))
    return b"\x00" + w.cobs_encode(body) + b"\x00"


def case_stale_ping(l):
    boot, _ = l.bind()
    l.send(type="PING", session=secrets.randbits(64), counter=1)
    ns = first(l.recv(), "NO_SESSION")
    return ns is not None and ns["boot_id"] == boot, f"reply {ns}"


def case_no_session_rate_cap(l):
    l.bind()
    time.sleep(0.06)  # outside the previous NO_SESSION window
    for i in range(5):
        l.send(type="PING", session=secrets.randbits(64), counter=i)
    got = l.recv(0.04)  # five frames take about 2 ms each, so all fall inside one 50 ms window
    return kinds(got).count("NO_SESSION") == 1, f"replies {kinds(got)}"


def case_stale_start(l):
    l.bind()
    l.send(type="START", session=secrets.randbits(64))
    return kinds(l.recv()) == ["NO_SESSION"], ""


def case_wrong_challenge_hello(l):
    l.bind()
    l.send(type="HELLO", challenge=secrets.randbits(64), nonce=secrets.randbits(64), version=1)
    got = l.recv()
    ann = first(got, "ANNOUNCE")
    return kinds(got) == ["ANNOUNCE"] and not ann["nonce_echo"], f"replies {kinds(got)}"


def case_busy_and_retransmit(l):
    """Host A holds a pending negotiation; host B gets BUSY for PROBE and HELLO; A's retransmitted HELLO gets the same session; A completes."""
    na, nb = secrets.randbits(64), secrets.randbits(64)
    l.send(type="PROBE", nonce=na)
    ann = first(l.recv(1.0, lambda g: "ANNOUNCE" in kinds(g)), "ANNOUNCE")
    l.send(type="HELLO", challenge=ann["challenge"], nonce=na, version=1)
    ack = first(l.recv(1.0, lambda g: "HELLO_ACK" in kinds(g)), "HELLO_ACK")
    l.send(type="PROBE", nonce=nb)
    b1 = l.recv()
    l.send(type="HELLO", challenge=ann["challenge"], nonce=nb, version=1)
    b2 = l.recv()
    l.send(type="HELLO", challenge=ann["challenge"], nonce=na, version=1)
    again = first(l.recv(), "HELLO_ACK")
    l.send(type="START", session=ack["session"])
    sa = l.recv()
    busy_ok = kinds(b1) == ["BUSY"] and kinds(b2) == ["BUSY"] and 0 < b1[0]["busy_remaining_ms"] <= T_PENDING_S * 1000
    ok = busy_ok and again and again["session"] == ack["session"] and kinds(sa) == ["START_ACK"]
    return ok, f"B probe {kinds(b1)} B hello {kinds(b2)} retransmit {again and again['session'] == ack['session']} start {kinds(sa)}"


def case_pending_expiry(l):
    nonce = secrets.randbits(64)
    l.send(type="PROBE", nonce=nonce)
    ann = first(l.recv(1.0, lambda g: "ANNOUNCE" in kinds(g)), "ANNOUNCE")
    l.send(type="HELLO", challenge=ann["challenge"], nonce=nonce, version=1)
    ack = first(l.recv(1.0, lambda g: "HELLO_ACK" in kinds(g)), "HELLO_ACK")
    time.sleep(T_PENDING_S + 0.2)
    l.send(type="START", session=ack["session"])
    return kinds(l.recv()) == ["NO_SESSION"], ""


def case_corrupt_crc(l):
    _, s = l.bind()
    f = bytearray(w.encode({"type": "PING", "session": s, "counter": 7}))
    f[-3] = f[-3] ^ 0x10 or 0x01
    l.raw(bytes(f), "PING bad CRC")
    silent = l.recv()
    l.send(type="PING", session=s, counter=8)
    after = l.recv()
    return silent == [] and kinds(after) == ["PONG"] and after[0]["counter"] == 8, f"silent {kinds(silent)} after {kinds(after)}"


def case_overflow_resync(l):
    _, s = l.bind()
    l.raw(b"\x41" * 400 + b"\x00", "400 garbage bytes")
    silent = l.recv()
    l.send(type="PING", session=s, counter=9)
    after = l.recv()
    return silent == [] and kinds(after) == ["PONG"], f"silent {kinds(silent)} after {kinds(after)}"


def case_truncated_frame(l):
    _, s = l.bind()
    f = w.encode({"type": "PING", "session": s, "counter": 10})
    l.raw(f[:len(f) // 2], "half PING")
    l.send(type="PING", session=s, counter=11)
    got = l.recv()
    return kinds(got) == ["PONG"] and got[0]["counter"] == 11, f"replies {kinds(got)}"


def case_wire_version(l):
    _, s = l.bind()
    l.raw(resealed(lambda: bytes([2, w.CODE["PING"], 0, 0]) + struct.pack("<QI", s, 12)), "PING wire version 2")
    silent = l.recv()
    l.raw(resealed(lambda: bytes([1, w.CODE["PING"], 0, 1]) + struct.pack("<QI", s, 13)), "PING reserved byte 1")
    silent += l.recv()
    l.raw(resealed(lambda: bytes([1, 0x7F, 0, 0]) + struct.pack("<QI", s, 14)), "unknown type 0x7F")
    silent += l.recv()
    return silent == [], f"replies {kinds(silent)}"


def case_increment_fence(l):
    _, s = l.bind()
    l.send(type="INCREMENT", session=s, txid=1)
    r1 = first(l.recv(1.0, lambda g: "EP1_REPLY" in kinds(g)), "EP1_REPLY")
    l.send(type="INCREMENT", session=s, txid=1)
    r2 = first(l.recv(), "EP1_REPLY")
    l.send(type="INCREMENT", session=s, txid=0)
    r3 = first(l.recv(), "EP1_REPLY")
    l.send(type="INCREMENT", session=s, txid=2)
    r4 = first(l.recv(1.0, lambda g: "EP1_REPLY" in kinds(g)), "EP1_REPLY")
    ok = (r1 and r1["ep1_status"] == 0 and r2 and r2["ep1_status"] == 1 and r2["counter"] == r1["counter"]
          and r3 and r3["ep1_status"] == 1 and r4 and r4["ep1_status"] == 0 and r4["counter"] == r1["counter"] + 1)
    return ok, f"txid1 {r1} replay {r2} txid0 {r3} txid2 {r4}"


def case_old_session_after_rebind(l):
    _, s1 = l.bind()
    _, s2 = l.bind()
    l.send(type="PING", session=s1, counter=1)
    a = l.recv()
    time.sleep(0.06)
    l.send(type="INCREMENT", session=s1, txid=1)
    b = l.recv()
    l.send(type="PING", session=s2, counter=2)
    c = l.recv()
    return s1 != s2 and kinds(a) == ["NO_SESSION"] and kinds(b) == ["NO_SESSION"] and kinds(c) == ["PONG"], f"{kinds(a)} {kinds(b)} {kinds(c)}"


def case_unsupported_version(l):
    """HELLO with a version above the announced range is handled like a HELLO for the wrong challenge: ANNOUNCE, no HELLO_ACK. A supported HELLO on the new challenge then binds."""
    nonce = secrets.randbits(64)
    l.send(type="PROBE", nonce=nonce)
    ann = first(l.recv(1.0, lambda g: "ANNOUNCE" in kinds(g)), "ANNOUNCE")
    l.send(type="HELLO", challenge=ann["challenge"], nonce=nonce, version=ann["version_max"] + 1)
    got = l.recv()
    if first(got, "HELLO_ACK"):
        l.send(type="START", session=first(got, "HELLO_ACK")["session"])  # leave no pending negotiation behind
        l.recv()
        return False, f"announced {ann['version_min']}..{ann['version_max']}, unsupported HELLO answered with {kinds(got)}"
    again = first(got, "ANNOUNCE")
    if again is None:
        return False, f"unsupported HELLO answered with {kinds(got)}"
    l.send(type="HELLO", challenge=again["challenge"], nonce=nonce, version=ann["version_max"])
    ack = l.recv()
    if first(ack, "HELLO_ACK"):
        l.send(type="START", session=first(ack, "HELLO_ACK")["session"])
        l.recv()
    return first(ack, "HELLO_ACK") is not None, f"unsupported HELLO answered with {kinds(got)}, supported HELLO with {kinds(ack)}"

CASES = [case_stale_ping, case_no_session_rate_cap, case_stale_start, case_wrong_challenge_hello, case_busy_and_retransmit, case_pending_expiry,
         case_corrupt_crc, case_overflow_resync, case_truncated_frame, case_wire_version, case_increment_fence, case_old_session_after_rebind,
         case_unsupported_version]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--evidence", required=True, help="existing directory; adversarial.jsonl is appended")
    args = ap.parse_args(argv)
    if not os.path.isdir(args.evidence):
        ap.error("evidence must be an existing directory")
    import serial

    try:
        port = serial.Serial(args.port, 115200, timeout=0.02, write_timeout=2.0, exclusive=True)
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL open {args.port}: {exc}")
        return 2
    failed = 0
    with port, open(os.path.join(args.evidence, "adversarial.jsonl"), "a") as out:
        port.reset_input_buffer()
        for case in CASES:
            link = Link(port, out)
            try:
                ok, detail = case(link)
            except (AssertionError, TypeError, KeyError) as exc:
                ok, detail = False, f"exception {exc!r}"
            verdict = "OBSERVED" if ok is None else "PASS" if ok else "FAIL"
            failed += verdict == "FAIL"
            print(f"{verdict} {case.__name__[5:]} {detail}")
            out.write(json.dumps({"case": case.__name__[5:], "verdict": verdict, "detail": detail, "events": link.events}, default=str) + "\n")
            out.flush()
            os.fsync(out.fileno())
            link.recv(0.1)  # drain stray replies before the next case
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
