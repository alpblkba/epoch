"""Tests for check_public_terms.py. Labels are assembled at run time so that this file itself stays clean under the check."""

import os
import tempfile
import unittest

import check_public_terms as c

MILESTONE = "HW" + "-02"
FINDING = "F" + "-4"


class Check(unittest.TestCase):
    def tree(self, files):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        for rel, data in files.items():
            p = os.path.join(d.name, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode())
        return d.name

    def test_technical_identifiers_pass(self):
        text = ("C11 Cortex-M33 STM32U585 B-U585I-IOT02A USART1 UART ISR SPSC MMIO COBS CRC-32 EP0 EP1 EP2 txid B0 B3 F1 F2a F2c F3 F4 "
                "EPOCH_CP_F1 CE8 P1 SB6 /dev/cu.usbmodem21303 0x08000000 core 0 sim.c trace Zephyr v4.4.2 SDK 1.0.1 R1 R2 BKP30R\n")
        root = self.tree({"core/src/a.c": text, "README.md": text})
        self.assertEqual(c.scan(root), [])

    def test_labels_in_text_and_variants_are_found(self):
        spelled = ["hw~02", "HW_~03", "DEV ~01", "doc-~01", "CORE-~01", "SIM~01", "rtl_~01", "GOV-~03", "HW-~01D"]  # "~" keeps this file clean
        variants = [MILESTONE, FINDING] + [v.replace("~", "") for v in spelled]
        root = self.tree({"host/x.py": "\n".join(f"# {v} text" for v in variants) + "\n"})
        self.assertEqual([line for _, line, _ in c.scan(root)], list(range(1, len(variants) + 1)))

    def test_private_citations_are_found(self):
        cites = ["private/" + "self-docs/x.md", "private/" + "tracelog/STATUS.md", "protocol" + ".md", "Proposal v" + "0.5 \"Host state\"",
                 "handbook " + "9.3", "Handbook section " + "8.11", "charter " + "note 3", "TBD in " + "spec.md", "Owner decision " + "C"]
        root = self.tree({"core/x.h": "\n".join(f"/* {v} */" for v in cites) + "\n"})
        self.assertEqual([line for _, line, _ in c.scan(root)], list(range(1, len(cites) + 1)))

    def test_ordinary_words_pass(self):
        text = ("the protocol thread owns tail; wire protocol version 1; a proposal for review; hardware handbook; the charter of a "
                "project; value TBD; owner of head; u_owner; private key; include/private.h; hardware_assumptions\n")
        self.assertEqual(c.scan(self.tree({"core/a.c": text, "README.md": text})), [])

    def test_file_names_are_checked(self):
        name = "firmware/" + "hw" + "02_uart.c"
        root = self.tree({name: "int x;\n", "tests/t.c": "static void test_" + "hw" + "02_echo(void);\n"})
        self.assertEqual(c.scan(root), [(name, 0, name), ("tests/t.c", 1, "static void test_" + "hw" + "02_echo(void);")])

    def test_private_build_and_binary_paths_are_skipped(self):
        bad = f"{MILESTONE}\n"
        root = self.tree({"private/notes.md": bad, "backup/x.md": bad, "CLAUDE.md": bad, "build/x.c": bad, "build-fw/x.c": bad,
                          "host/__pycache__/x.py": bad, "fw/zephyr.bin": bad, "fw/blob.dat": b"\0" + bad.encode()})
        self.assertEqual(c.scan(root), [])

    def test_exit_status(self):
        self.assertEqual(c.main(["x", self.tree({"a.c": "ok\n"})]), 0)
        self.assertEqual(c.main(["x", self.tree({"a.c": f"/* {MILESTONE} */\n"})]), 1)


if __name__ == "__main__":
    unittest.main()
