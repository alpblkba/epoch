"""Offline re-evaluation of reset-campaign records written by epoch_client.

Reads trial JSON lines and, optionally, the console logs of the same runs, and recomputes every verdict without trusting the stored one. Each record is checked for a unique run, a known policy and checkpoint, a marker that names the commanded run, checkpoint and txid, a software reset flag, a new boot ID and a new session, an oracle count that matches the checkpoint, no reply for a foreign session, and the outcome the policy must report. With a log, the first ANNOUNCE after each commanded reset is decoded independently of the C code and its journal must hold the trial request exactly when the checkpoint follows the commit. Prints one line per policy and checkpoint and the list of disagreements; exits 1 when any record is not a recomputed pass or disagrees with its stored verdict, 0 otherwise. Read only.

Usage: python3 verify_trials.py trials.jsonl [more.jsonl ...] [--log run.log ...] [--magic 0x45504301]
"""

import argparse
import collections
import json
import re
import struct
import sys
import zlib

POLICIES = ("b0", "b1", "b2", "b3")
CHECKPOINTS = ("f1", "f2a", "f2b", "f2c", "f3")
SFTRSTF = 1 << 28
OTHER_RESET_FLAGS = 0xEA000000  # RCC_CSR LPWRRSTF, WWDGRSTF, IWDGRSTF, BORRSTF, OBLRSTF; PINRSTF accompanies every reset


def expected_outcome(policy, executed):
    """Every armed checkpoint resets before the reply, so B1 and B2 have no evidence, B0 completes through its retry and B3 must match the journal."""
    return {"b0": "COMPLETED", "b1": "UNKNOWN", "b2": "UNKNOWN", "b3": "COMPLETED" if executed else "NOT_EXECUTED"}[policy]


def judge(t):
    """Returns (verdict, reason) for one record, recomputed from its fields."""
    if t.get("policy") not in POLICIES or t.get("checkpoint") not in CHECKPOINTS:
        return "NOT_RUN", "policy or checkpoint outside the campaign"
    m = t.get("marker", {})
    if not m.get("valid") or m.get("run") != t["run"] or m.get("checkpoint") != CHECKPOINTS.index(t["checkpoint"]) or m.get("txid") != t["armed_txid"]:
        return "INCONCLUSIVE", "marker does not match the commanded checkpoint"
    cause = int(t["reset_cause"], 16)
    if not cause & SFTRSTF or cause & OTHER_RESET_FLAGS or t["boot_after"] == t["boot_before"]:
        return "INCONCLUSIVE", "reset cause or boot ID does not show a software reset"
    executed = t["checkpoint"] == "f3"
    delta = (t["c2"] - t["c0"] - 1) % 2**32
    if delta != int(executed) + (t["policy"] == "b0"):
        return "COUNTED_FAIL", f"oracle count {delta} disagrees with the checkpoint"
    if t["foreign_replies"] or t["session_after"] == t["session_before"]:
        return "COUNTED_FAIL", "session isolation evidence"
    if t["policy"] == "b0" and t["retry_counter"] != (t["c2"] - 1) % 2**32:
        return "COUNTED_FAIL", "B0 retry counter is not the one before the oracle"
    want = expected_outcome(t["policy"], executed)
    if t["outcome"] != want:
        return "COUNTED_FAIL", f"outcome {t['outcome']}, expected {want}"
    if t.get("rx_errors", 0):  # recorded since the rejected-frame check; earlier records rely on the log check
        return "INCONCLUSIVE", "rejected frame during the trial"
    return "COUNTED_PASS", ""


def newest(words, era, magic):
    """Index of the newest valid slot by the one-step rule, None when no slot is valid, 'inconsistent' for corrupted retention: two valid slots that break the rule, or an invalid slot whose w0 is not 0."""
    valid = []
    for i in (0, 1):
        w = words[8 * i:8 * i + 8]
        valid.append(w[0] == magic and w[1] == era and zlib.crc32(struct.pack("<7I", *w[:7])) == w[7])
    if any(not v and words[8 * i] != 0 for i, v in enumerate(valid)):
        return "inconsistent"
    if all(valid):
        d = (words[6] - words[14]) % 2**32
        return 0 if d == 1 else 1 if d == 2**32 - 1 else "inconsistent"
    return 0 if valid[0] else 1 if valid[1] else None


def snapshot_check(t, ann, magic):
    """Returns '' when the post-reset journal agrees with the checkpoint, otherwise the disagreement."""
    words = [int(x, 16) for x in ann["slots"].split(",")]
    era = int(ann["era"], 16)
    n = newest(words, era, magic)
    if n is None or n == "inconsistent":
        return f"post-reset journal has {n or 'no'} valid slot"
    w = words[8 * n:8 * n + 8]
    session = f"{w[3]:08x}{w[2]:08x}"
    holds = session == t["session_before"] and w[4] == t["armed_txid"]
    if t["checkpoint"] == "f3":
        return "" if holds and w[5] == (t["c0"] + 1) % 2**32 else "journal lacks the committed trial request"
    return "journal holds the trial request although the checkpoint precedes the commit" if holds else ""


ANNOUNCE = re.compile(r" rx ANNOUNCE .* era=([0-9a-f]{8}) slots=([0-9a-f,]+)")
COMMANDED = re.compile(r" commanded run=(\d+) checkpoint=(\w+) txid=(\d+)")
RESULT = re.compile(r"^RESULT (\w+) (\w+) COUNTED_PASS=(\d+) COUNTED_FAIL=(\d+) INCONCLUSIVE=(\d+) NOT_RUN=(\d+)")
TRIAL = re.compile(r" TRIAL run=(\d+) ")
RX_ERROR = re.compile(r"^\d+ rx_error ")


def read_log(path):
    """Returns ({run: first ANNOUNCE after the commanded reset}, [RESULT tuples], {commanded runs}, {runs with a rejected frame}, rejected frames outside any trial). A trial's lines end with its TRIAL line; a rejected frame belongs to the trial whose TRIAL line follows it, or to the last commanded run when the log ends first."""
    after, results, commanded, current = {}, [], set(), None
    damaged, pending, last_run = set(), 0, None
    with open(path) as f:
        for line in f:
            if RX_ERROR.match(line):
                pending += 1
            elif m := TRIAL.search(line):
                if pending:
                    damaged.add(int(m.group(1)))
                pending = 0
            if m := COMMANDED.search(line):
                current = last_run = int(m.group(1))
                commanded.add(current)
            elif current is not None and (m := ANNOUNCE.search(line)):
                after.setdefault(current, {"era": m.group(1), "slots": m.group(2)})
                current = None
            elif m := RESULT.match(line):
                results.append((m.group(1), m.group(2), sum(int(x) for x in m.groups()[2:])))
    if pending and last_run is not None:
        damaged.add(last_run)
        pending = 0
    return after, results, commanded, damaged, pending


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("jsonl", nargs="+")
    ap.add_argument("--log", nargs="*", default=[])
    ap.add_argument("--magic", type=lambda s: int(s, 0), default=0x45504301)
    args = ap.parse_args(argv)

    records, problems = [], []
    for path in args.jsonl:
        last_utc = None
        with open(path) as f:
            for i, line in enumerate(f, 1):
                t = json.loads(line)
                t["_where"] = f"{path}:{i}"
                if last_utc is not None and t["utc_ms"] < last_utc:
                    problems.append(f"{t['_where']}: timestamp earlier than the previous record")
                last_utc = t["utc_ms"]
                records.append(t)
    snapshots, logs, damaged = {}, [], set()
    for path in args.log:
        a, r, commanded, bad, stray = read_log(path)
        snapshots.update(a)
        damaged |= bad
        logs.append((path, r, commanded))
        if stray:
            problems.append(f"{path}: {stray} rejected frame(s) outside any trial")

    runs = collections.Counter(t["run"] for t in records)
    cells = collections.OrderedDict()
    for t in records:
        verdict, why = judge(t)
        if runs[t["run"]] > 1:
            verdict, why = "INCONCLUSIVE", "run identifier recorded more than once"
        if verdict == "COUNTED_PASS" and t["run"] in snapshots:
            why = snapshot_check(t, snapshots[t["run"]], args.magic)
            verdict = "COUNTED_FAIL" if why else verdict
        if verdict == "COUNTED_PASS" and t["run"] in damaged:
            verdict, why = "INCONCLUSIVE", "rejected frame in the log of this trial"
        c = cells.setdefault((t["policy"], t["checkpoint"]), collections.Counter())
        c["recorded"] += 1
        c[verdict] += 1
        c["stored_pass"] += t["verdict"] == "COUNTED_PASS"
        c["snapshot_checked"] += t["run"] in snapshots
        if verdict != t["verdict"] or verdict != "COUNTED_PASS":
            c["disagreements"] += verdict != t["verdict"]
            problems.append(f"{t['_where']}: run {t['run']} stored {t['verdict']}, recomputed {verdict} {why}".rstrip())
    for path, results, commanded in logs:  # a log is compared only with its own runs, so campaigns sharing a cell stay apart
        held = collections.Counter((t["policy"], t["checkpoint"]) for t in records if t["run"] in commanded)
        for policy, cp, total in results:
            if total != held[(policy, cp)]:
                problems.append(f"{path}: RESULT {policy} {cp} reports {total} trials, records hold {held[(policy, cp)]}")
    missing = sorted(set(snapshots) - set(runs))
    if missing:
        problems.append(f"{len(missing)} run(s) in the logs have no record, first {missing[0]}")

    for (policy, cp), c in cells.items():
        print(f"{policy} {cp} recorded={c['recorded']} pass={c['COUNTED_PASS']} fail={c['COUNTED_FAIL']} inconclusive={c['INCONCLUSIVE']} "
              f"not_run={c['NOT_RUN']} stored_pass={c['stored_pass']} snapshot_checked={c['snapshot_checked']} disagreements={c['disagreements']}")
    for p in problems:
        print(p)
    print(f"{len(records)} record(s), {sum(c['COUNTED_PASS'] for c in cells.values())} recomputed pass, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
