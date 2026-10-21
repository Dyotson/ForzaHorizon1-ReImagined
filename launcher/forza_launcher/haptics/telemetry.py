"""Strict decoder for the native FH1T v1 bridge, not Forza's later Data Out format."""
from dataclasses import dataclass
import math
import struct

WIRE = struct.Struct('<4sHHQIIQII12fBBBbHH')
assert WIRE.size == 96


@dataclass(frozen=True)
class Frame:
    sequence: int
    flags: int
    sample_ms: int
    car: int
    active: bool
    position: tuple
    velocity: tuple
    acceleration: tuple
    forward: tuple
    brake: int
    throttle: int
    steer: int
    buttons: int


class Decoder:
    def __init__(self, token):
        self.token = token
        self.sequence = None
        self.sample_ms = None

    def decode(self, data):
        if len(data) != WIRE.size:
            return None
        magic, version, size, token, seq, flags, stamp, car, active, *v = WIRE.unpack(data)
        if (magic, version, size, token) != (b'FH1T', 1, WIRE.size, self.token):
            return None
        if flags & ~15 or active not in (0, 1) or v[14] != 0 or v[17] != 0:
            return None  # Handbrake is intentionally unavailable in v1.
        if active and (not car or flags & 3 != 3):
            return None
        if any(not math.isfinite(f) or abs(f) > 1e6 for f in v[:12]):
            return None
        if self.sequence is not None and not 0 < ((seq-self.sequence) & 0xffffffff) < 0x80000000:
            return None
        if self.sample_ms is not None and stamp < self.sample_ms:
            return None
        self.sequence, self.sample_ms = seq, stamp
        return Frame(seq, flags, stamp, car, bool(active), tuple(v[:3]), tuple(v[3:6]),
                     tuple(v[6:9]), tuple(v[9:12]), v[12], v[13], v[15], v[16])
