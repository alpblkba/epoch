"""Tests for retention_probe.py report parsing and trial classification; no hardware."""

import unittest

import retention_probe as p

RAW = ",".join(["45503152"] + ["0"] * 11)
BOOT = ["BOOT probe=1 csr=14000000 optr=1f8ff8aa r1_addr=20001088 r2_addr=40036400 bkp31=b2c0ffee",
        f"R1 VALID run=7 gen=3 raw={RAW}", f"R2 VALID run=7 gen=3 raw={RAW}", "READY"]


class Classify(unittest.TestCase):
    def test_parse(self):
        csr, optr, recs = p.parse_boot(BOOT)
        self.assertEqual((csr, optr, recs["R1"][:3]), (0x14000000, 0x1F8FF8AA, ("VALID", 7, 3)))
        self.assertIsNone(p.parse_boot(BOOT[:-1]))  # no READY: incomplete

    def test_soft_reset_pass_and_stale(self):
        csr, _, recs = p.parse_boot(BOOT)  # SFT and PIN set
        self.assertEqual(p.classify("soft", csr, recs, 7, 3)["R1"][0], "PASS")
        self.assertEqual(p.classify("soft", csr, recs, 7, 4)["R1"][0], "INCONCLUSIVE")  # older generation is not this trial

    def test_wrong_flags_are_inconclusive(self):
        csr, _, recs = p.parse_boot(BOOT)
        self.assertEqual(p.classify("wdg", csr, recs, 7, 3)["R2"][0], "INCONCLUSIVE")
        self.assertEqual(p.classify("pin", csr, recs, 7, 3)["R1"][0], "INCONCLUSIVE")  # SFT present

    def test_lost_and_unread(self):
        lines = BOOT[:1] + ["R1 ZERO run=0 gen=0 raw=" + ",".join(["0"] * 12), "R2 NOT_READ marker absent", "READY"]
        csr, _, recs = p.parse_boot(lines)
        res = p.classify("soft", csr, recs, 7, 3)
        self.assertEqual((res["R1"][0], res["R2"][0]), ("FAIL", "INCONCLUSIVE"))


if __name__ == "__main__":
    unittest.main()
