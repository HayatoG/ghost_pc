"""Synthetic VU1 call records for the replay harness (same format as PS2X_VU1_RECORD).

Random micro programs drawn from the valid upper/lower opcode tables, random VF/VI state and VU data
(including denormals, infinities and NaNs), so the A/B check also covers paths a game capture may miss:
DIV/SQRT/RSQRT, the EFU, flag instructions, short branches, MFIR/MTIR, RNEXT and the like.

usage: python -I gen_synthetic.py <out.bin> [calls] [seed]
"""
import random
import struct
import sys

STATE_SIZE = 664          # sizeof(VU1State) on x64 MSVC; the harness rejects a mismatch
CODE_SIZE = 0x4000
DATA_SIZE = 0x4000

UPPER_OPS = list(range(0x00, 0x30))
UPPER_SPECIAL = list(range(0x00, 0x20)) + list(range(0x20, 0x2B)) + [0x2C, 0x2D, 0x2E, 0x2F, 0x30]
LOWER_SPECIAL = [0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
                 0x40, 0x41, 0x42, 0x43, 0x64, 0x68, 0x69,
                 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D]
LOWER_PLAIN = [0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
               0x18, 0x1A, 0x1B, 0x1C]
LOWER_BRANCH = [0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F]


def rnd_float_bits(r):
    roll = r.random()
    if roll < 0.04:
        return r.choice([0x00000001, 0x80000010, 0x007FFFFF])        # denormals
    if roll < 0.07:
        return r.choice([0x7F800000, 0xFF800000, 0x7FC00000, 0x7F7FFFFF, 0xFF7FFFFF])
    if roll < 0.10:
        return r.choice([0x00000000, 0x80000000])
    if roll < 0.20:
        return r.getrandbits(32)                                      # raw integers / colors
    if roll < 0.25:
        return struct.unpack('<I', struct.pack('<f', r.choice([1e30, -1e30, 1e-30, -1e-30, 3e38])))[0]
    return struct.unpack('<I', struct.pack('<f', r.uniform(-1000.0, 1000.0)))[0]


def upper_word(r):
    dest = r.randrange(16) or 0xF
    ft, fs, fd = r.randrange(32), r.randrange(32), r.randrange(32)
    if r.random() < 0.6:
        op = r.choice(UPPER_OPS)
        return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op
    code = r.choice(UPPER_SPECIAL)
    return (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | 0x3C | (code & 3)


def lower_word(r, pair_index, pairs):
    dest = r.randrange(16) or 0xF
    ft, fs = r.randrange(32), r.randrange(32)
    roll = r.random()
    if roll < 0.08:
        return 0x8000033C                                             # NOP
    if roll < 0.13 and pair_index + 4 < pairs:
        op = r.choice(LOWER_BRANCH)
        imm = r.randrange(1, 3)                                       # short forward branch only
        return (op << 25) | (r.randrange(16) << 16) | (r.randrange(16) << 11) | (imm & 0x7FF)
    if roll < 0.45:
        op = r.choice(LOWER_PLAIN)
        if op in (0x00, 0x01, 0x04, 0x05):
            vi_base = r.randrange(16)
            return (op << 25) | (dest << 21) | (ft << 16) | (vi_base << 11) | r.randrange(0x7FF)
        return (op << 25) | (r.getrandbits(25) & 0x1FFFFFF)
    if roll < 0.55:
        funct = r.choice([0x30, 0x31, 0x32, 0x34, 0x35])
        return (0x40 << 25) | (dest << 21) | (r.randrange(16) << 16) | (r.randrange(16) << 11) | (r.randrange(16) << 6) | funct
    code = r.choice(LOWER_SPECIAL)
    return (0x40 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | 0x3C | (code & 3)


def make_code(r, pairs):
    code = bytearray(CODE_SIZE)
    for index in range(pairs):
        upper = upper_word(r)
        if index == pairs - 2:
            upper |= 0x40000000                                       # E bit, delay slot follows
            lower = 0x8000033C
        elif r.random() < 0.05:
            upper |= 0x80000000                                       # I bit: lower is an immediate
            lower = rnd_float_bits(r)
        else:
            lower = lower_word(r, index, pairs)
        struct.pack_into('<II', code, index * 8, lower, upper)
    return bytes(code)


def make_state(r):
    state = bytearray(STATE_SIZE)
    for reg in range(32):
        for lane in range(4):
            struct.pack_into('<I', state, (reg * 4 + lane) * 4, rnd_float_bits(r))
    for reg in range(16):
        struct.pack_into('<i', state, 512 + reg * 4, r.randrange(0, 0x400))
    for lane in range(4):
        struct.pack_into('<I', state, 576 + lane * 4, rnd_float_bits(r))
    struct.pack_into('<fffI', state, 592, r.uniform(-4, 4), r.uniform(-4, 4), r.uniform(-4, 4), 0x3F800000 | r.getrandbits(23))
    return bytes(state)


def main():
    out = sys.argv[1]
    calls = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    r = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1234)
    with open(out, 'wb') as f:
        for index in range(calls):
            code = make_code(r, r.randrange(16, 200)) if index % 10 == 0 else None
            data = bytes(b for _ in range(DATA_SIZE // 4) for b in struct.pack('<I', rnd_float_bits(r)))
            header = struct.pack('<10I', 0x31525556, 1, 0, 1 if code else 0, 0, r.randrange(0x400), r.randrange(0x400),
                                 65536, CODE_SIZE, DATA_SIZE)
            f.write(header + struct.pack('<QI', 0, STATE_SIZE) + make_state(r))
            if code:
                f.write(code)
            f.write(data)


if __name__ == '__main__':
    main()
