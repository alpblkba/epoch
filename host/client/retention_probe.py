"""Retained-memory probe runner: writes the R1 and R2 probe records, triggers one reset per trial and classifies what survived.

Each trial: I <run> <gen>, check both records read back VALID, trigger the reset, wait for the next BOOT report and compare. A trial counts only when the reported RCC_CSR flags match the requested reset kind; otherwise it is INCONCLUSIVE. Every trial is appended to <evidence>/trials.jsonl with the raw lines before the next one starts. Exit 0 when every counted trial passed for both backends, 1 otherwise, 2 on an argument or open error.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
MAX_TRIALS = 50
# RCC_CSR reset flags, stm32u585xx.h
FLAGS = {"OBL": 1 << 25, "PIN": 1 << 26, "BOR": 1 << 27, "SFT": 1 << 28, "IWDG": 1 << 29, "WWDG": 1 << 30, "LPWR": 1 << 31}
# Flag that must be set and flags that must be clear for each reset kind. An internal reset also drives NRST, so PIN may accompany SFT and IWDG.
EXPECT = {
    "soft": ("SFT", ("BOR", "IWDG", "WWDG", "LPWR", "OBL")),
    "wdg": ("IWDG", ("BOR", "SFT", "WWDG", "LPWR", "OBL")),
    "pin": ("PIN", ("BOR", "SFT", "IWDG", "WWDG", "LPWR", "OBL")),
    "cold": ("BOR", ("SFT", "IWDG", "WWDG", "LPWR", "OBL")),
}
RECORD = re.compile(r"^(R1|R2) (\w+)(?: run=(\d+) gen=(\d+) raw=([0-9a-f,]+)| (.*))$")


def flags(csr):
    return [name for name, bit in FLAGS.items() if csr & bit]


def parse_boot(lines):
    """Returns (csr, optr, {backend: (status, run, gen, raw)}) from a BOOT report, or None if it is incomplete."""
    boot = next((l for l in lines if l.startswith("BOOT ")), None)
    if boot is None or "READY" not in lines:
        return None
    fields = dict(kv.split("=", 1) for kv in boot.split()[1:])
    recs = {}
    for l in lines[lines.index(boot):]:
        m = RECORD.match(l)
        if m:
            recs[m[1]] = (m[2], int(m[3]) if m[3] else None, int(m[4]) if m[4] else None, m[5] or m[6])
    return int(fields["csr"], 16), int(fields["optr"], 16), recs


def classify_record(rec, run, gen):
    if rec is None:
        return "INCONCLUSIVE", "no record line"
    status, r, g, _ = rec
    if status == "VALID" and (r, g) == (run, gen):
        return "PASS", ""
    if status == "VALID":
        return "INCONCLUSIVE", f"valid record from run {r} gen {g}, not this trial's write"
    if status == "NOT_READ":
        return "INCONCLUSIVE", "backup-domain marker absent, R2 not read"
    return "FAIL", status


def classify(kind, csr, recs, run, gen):
    must, must_not = EXPECT[kind]
    seen = flags(csr)
    if must not in seen or any(f in seen for f in must_not):
        return {b: ("INCONCLUSIVE", f"reset flags {seen} do not match {kind}") for b in ("R1", "R2")}
    return {b: classify_record(recs.get(b), run, gen) for b in ("R1", "R2")}


class Probe:
    def __init__(self, port, path):
        self.port, self.path = port, path

    def reopen(self, timeout=30.0):
        """After a power cycle the VCP re-enumerates; waits for the device node and opens it again."""
        import serial

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.port = serial.Serial(self.path, 115200, timeout=0.1, write_timeout=2.0, exclusive=True)
                return
            except (serial.SerialException, OSError):
                time.sleep(0.5)
        raise OSError(f"{self.path} did not come back within {timeout} s")

    def lines_until(self, done, timeout):
        """Reads lines until done(lines) or timeout seconds pass without completion; returns the lines."""
        lines, buf, deadline = [], b"", time.monotonic() + timeout
        while time.monotonic() < deadline:
            buf += self.port.read(self.port.in_waiting or 1)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                lines.append(raw.decode("ascii", "replace").strip())
                if done(lines):
                    return lines
        return lines

    def command(self, text, done, timeout=2.0):
        self.port.write(text.encode("ascii") + b"\n")
        self.port.flush()
        return self.lines_until(done, timeout)


def pin_reset(log):
    """Pulses NRST through the ST-LINK; HOTPLUG avoids a connect-time reset of its own."""
    r = subprocess.run([CLI, "-c", "port=SWD", "mode=HOTPLUG", "-hardRst"], capture_output=True, text=True, timeout=60)
    with open(log, "a") as f:
        f.write(r.stdout + r.stderr + f"\nrc={r.returncode}\n")
    return r.returncode


def trial(probe, kind, run, gen, evidence):
    t = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "kind": kind, "run": run, "gen": gen}
    probe.port.reset_input_buffer()
    t["write_lines"] = probe.command(f"I {run} {gen}", lambda ls: sum(l.startswith(("R1 ", "R2 ")) for l in ls) >= 2)
    before = {m[1]: m[2] for m in map(RECORD.match, t["write_lines"]) if m}
    if before.get("R1") != "VALID" or before.get("R2") != "VALID":
        t["result"] = {"R1": ("INCONCLUSIVE", "write not read back VALID"), "R2": ("INCONCLUSIVE", "write not read back VALID")}
        return t
    if kind in ("soft", "wdg"):
        t["trigger_lines"] = probe.command("S" if kind == "soft" else "W", lambda ls: ls and ls[-1].startswith("OK "), 1.0)
    elif kind == "pin":
        t["trigger_rc"] = pin_reset(os.path.join(evidence, "pin_reset_cli.log"))
    else:
        probe.port.close()
        input(f"cold trial run {run} gen {gen}: unplug the board USB, wait 10 s, plug it back in, then press Enter ")
        probe.reopen()
        t["boot_lines"] = probe.command("B", lambda ls: "READY" in ls, 5.0)  # the power-up BOOT line went out before the port reopened
    if "boot_lines" not in t:
        t["boot_lines"] = probe.lines_until(lambda ls: "READY" in ls, 10.0)
    parsed = parse_boot(t["boot_lines"])
    if parsed is None:
        t["result"] = {"R1": ("INCONCLUSIVE", "no complete BOOT report"), "R2": ("INCONCLUSIVE", "no complete BOOT report")}
        return t
    csr, optr, recs = parsed
    t.update(csr=f"{csr:08x}", optr=f"{optr:08x}", reset_flags=flags(csr), records={k: list(v) for k, v in recs.items()})
    t["result"] = classify(kind, csr, recs, run, gen)
    return t


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--kind", required=True, choices=sorted(EXPECT))
    ap.add_argument("--trials", type=int, default=1)
    ap.add_argument("--run", type=int, required=True, help="run identifier written into every record")
    ap.add_argument("--evidence", required=True, help="existing directory; trials.jsonl is appended")
    args = ap.parse_args(argv)
    if not 1 <= args.trials <= MAX_TRIALS or not os.path.isdir(args.evidence) or not 0 < args.run < 2**32:
        ap.error(f"trials must be 1..{MAX_TRIALS}, run 1..2^32-1, evidence an existing directory")

    import serial

    try:
        port = serial.Serial(args.port, 115200, timeout=0.1, write_timeout=2.0, exclusive=True)
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL open {args.port}: {exc}")
        return 2
    counts, misses = {}, 0
    try:
        probe = Probe(port, args.port)
        for i in range(args.trials):
            t = trial(probe, args.kind, args.run, i + 1, args.evidence)
            with open(os.path.join(args.evidence, "trials.jsonl"), "a") as f:
                f.write(json.dumps(t) + "\n")
                f.flush()
                os.fsync(f.fileno())
            for b, (res, why) in t["result"].items():
                counts[(b, res)] = counts.get((b, res), 0) + 1
            print(f"trial {i + 1} {args.kind} flags={t.get('reset_flags')} R1={t['result']['R1'][0]} R2={t['result']['R2'][0]} "
                  f"{t['result']['R1'][1]} {t['result']['R2'][1]}".rstrip())
            misses = misses + 1 if "boot_lines" in t and t.get("csr") is None else 0
            if misses >= 2:
                print("STOP: two consecutive trials without a BOOT report")
                break
    except (serial.SerialException, OSError) as exc:
        print(f"FAIL I/O error: {exc}")
        return 1
    finally:
        (probe.port if "probe" in locals() else port).close()
    for b in ("R1", "R2"):
        print(f"SUMMARY {b} {args.kind}: " + ", ".join(f"{r} {counts.get((b, r), 0)}" for r in ("PASS", "FAIL", "INCONCLUSIVE")))
    return 0 if all(counts.get((b, r), 0) == 0 for b in ("R1", "R2") for r in ("FAIL", "INCONCLUSIVE")) else 1


if __name__ == "__main__":
    sys.exit(main())
