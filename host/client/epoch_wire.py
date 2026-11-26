"""Independent Python reference for the EPOCH UART wire format defined in core/include/epoch/wire.h; shares no code with core/src/wire.c and is used for golden vectors and by the host tools."""

import struct
import zlib

VERSION = 1
TYPES = ["PROBE", "ANNOUNCE", "HELLO", "HELLO_ACK", "START", "START_ACK", "PING", "PONG", "NO_SESSION", "BUSY", "INCREMENT", "EP1_REPLY", "TEST_ARM", "TEST_ARM_ACK", "TEST_DIAG", "TEST_DIAG_REPLY"]
CODE = {name: i + 1 for i, name in enumerate(TYPES)}
NAME = {v: k for k, v in CODE.items()}
# struct layouts of the fields between header and CRC, little-endian
LAYOUT = {
    "PROBE": ("<Q", ["nonce"]),
    "ANNOUNCE": ("<QQQIIIII16I", ["boot_id", "challenge", "nonce", "reset_cause", "version_min", "version_max", "capabilities", "era"] + [f"slot{i}" for i in range(16)]),
    "HELLO": ("<QQI", ["challenge", "nonce", "version"]),
    "HELLO_ACK": ("<QQQ", ["challenge", "nonce", "session"]),
    "START": ("<Q", ["session"]),
    "START_ACK": ("<Q", ["session"]),
    "PING": ("<QI", ["session", "counter"]),
    "PONG": ("<QI", ["session", "counter"]),
    "NO_SESSION": ("<QQ", ["boot_id", "challenge"]),
    "BUSY": ("<I", ["busy_remaining_ms"]),
    "INCREMENT": ("<QI", ["session", "txid"]),
    "EP1_REPLY": ("<QIBI", ["session", "txid", "ep1_status", "counter"]),
    "TEST_ARM": ("<III", ["run", "checkpoint", "txid"]),
    "TEST_ARM_ACK": ("<III", ["run", "checkpoint", "txid"]),
    "TEST_DIAG": ("<I", ["run"]),
    "TEST_DIAG_REPLY": ("<BIIII", ["marker_valid", "run", "checkpoint", "txid", "reset_cause"]),
}
FLAG_NONCE_ECHO, FLAG_RETENTION = 1, 2
MAX_BODY = 128


def cobs_encode(data):
    out, block = bytearray(), bytearray()
    for b in data:
        if b == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out += b"\xff" + block
                block = bytearray()
    return bytes(out + bytes([len(block) + 1]) + block)


def cobs_decode(data):
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            raise ValueError("ERR_COBS")
        out += data[i + 1:i + code]
        i += code
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode(msg):
    """msg: dict with 'type' and the fields of LAYOUT[type]; ANNOUNCE also takes nonce_echo and retention_valid. Returns the frame with both delimiters."""
    fmt, names = LAYOUT[msg["type"]]
    flags = 0
    if msg["type"] == "ANNOUNCE":
        flags = (FLAG_NONCE_ECHO if msg.get("nonce_echo") else 0) | (FLAG_RETENTION if msg.get("retention_valid") else 0)
    values = [msg.get(n, 0) if n != "nonce" or msg["type"] != "ANNOUNCE" or msg.get("nonce_echo") else 0 for n in names]
    body = bytes([VERSION, CODE[msg["type"]], flags, 0]) + struct.pack(fmt, *values)
    body += struct.pack("<I", zlib.crc32(body))
    return b"\x00" + cobs_encode(body) + b"\x00"


def decode_body(body):
    """Returns a dict, or raises ValueError naming the C status for the same failure."""
    if len(body) < 8:
        raise ValueError("ERR_LENGTH")
    if struct.unpack("<I", body[-4:])[0] != zlib.crc32(body[:-4]):
        raise ValueError("ERR_CRC")
    if body[0] != VERSION:
        raise ValueError("ERR_VERSION")
    if body[1] not in NAME:
        raise ValueError("ERR_TYPE")
    name = NAME[body[1]]
    allowed = FLAG_NONCE_ECHO | FLAG_RETENTION if name == "ANNOUNCE" else 0
    if body[3] or body[2] & ~allowed:
        raise ValueError("ERR_FIELD")
    fmt, names = LAYOUT[name]
    if len(body) != 8 + struct.calcsize(fmt):
        raise ValueError("ERR_LENGTH")
    msg = dict(zip(names, struct.unpack(fmt, body[4:-4])), type=name)
    if (name == "EP1_REPLY" and msg["ep1_status"] > 1) or (name == "TEST_DIAG_REPLY" and msg["marker_valid"] > 1):
        raise ValueError("ERR_FIELD")
    if name == "ANNOUNCE":
        msg["nonce_echo"], msg["retention_valid"] = bool(body[2] & FLAG_NONCE_ECHO), bool(body[2] & FLAG_RETENTION)
        if not msg["nonce_echo"] and msg["nonce"]:
            raise ValueError("ERR_FIELD")
    return msg


class Parser:
    """Byte-stream parser with the C parser's rules: every 0x00 ends a frame, empty frames are ignored, overlong frames are discarded."""

    def __init__(self):
        self.buf, self.discarding = bytearray(), False

    def feed(self, data):
        """Returns a list of (status, msg_or_None) for every frame that ended in data."""
        out = []
        for b in data:
            if b:
                if not self.discarding:
                    if len(self.buf) < MAX_BODY + 1:
                        self.buf.append(b)
                    else:
                        self.discarding = True
                continue
            if self.discarding:
                out.append(("ERR_OVERFLOW", None))
            elif self.buf:
                try:
                    out.append(("OK", decode_body(cobs_decode(bytes(self.buf)))))
                except ValueError as exc:
                    out.append((str(exc), None))
            self.buf, self.discarding = bytearray(), False
        return out
