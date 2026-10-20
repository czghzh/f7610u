#!/usr/bin/env python3
"""Extract every Hal_WriteXmacReg(idx, val) / Hal_WriteGmacReg call from
zte_xgpon.ko disassembly, with a tiny constant-propagation tracker for
w0 (idx) and w1 (val).

Usage: extract_xmac_writes.py /tmp/xgpon.dis
Output: one line per call site:
    <func> <addr> <idx|?> <val|?>
"""
import re
import sys

LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]{8})\s*\t(.+)$")
FUNC = re.compile(r"^([0-9a-f]+) <(.+)>:$")

IMM_RE = re.compile(r"#(-?(?:0x)?[0-9a-fA-F]+)")
SHIFT_RE = re.compile(r"lsl #(\d+)")

CALLS = ("Hal_WriteXmacReg", "Hal_WriteGmacReg")

BRANCHES = ("b\t", "b.", "cbz", "cbnz", "tbz", "tbnz", "ret", "br\t")


def imm_of(op):
    m = IMM_RE.search(op)
    if not m:
        return None
    s = m.group(1)
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    try:
        v = int(s, 16) if s.lower().startswith("0x") else int(s, 10)
    except ValueError:
        return None
    return -v if neg else v


def shift_of(op):
    m = SHIFT_RE.search(op)
    return int(m.group(1)) if m else 0


def main(path):
    regs = {}          # 'w0' -> value or None
    func = "?"
    out = []

    def clear_caller_saved():
        for i in range(19):
            regs.pop("w%d" % i, None)

    for raw in open(path, errors="replace"):
        raw = raw.rstrip("\n")
        mf = FUNC.match(raw.strip())
        if mf:
            func = mf.group(2)
            regs = {}
            continue

        m = LINE.match(raw)
        if not m:
            continue
        addr = int(m.group(1), 16)
        op = m.group(3).strip()
        parts = op.split("\t")
        mn = parts[0].strip()
        ops = parts[1] if len(parts) > 1 else ""
        operands = [x.strip() for x in ops.split(",") if x.strip()]

        if mn in ("mov", "movz", "movn") and len(operands) == 2:
            dst = operands[0]
            if dst.startswith("w"):
                if operands[1].startswith("#"):
                    v = imm_of(operands[1])
                    if mn == "movn" and v is not None:
                        v = (~v) & 0xffffffff
                    regs[dst] = v << shift_of(operands[1]) if mn == "movz" and v is not None else v
                elif operands[1].startswith("w"):
                    regs[dst] = regs.get(operands[1])
                else:
                    regs[dst] = None
            continue

        if mn == "movk" and len(operands) == 2:
            dst = operands[0]
            if dst.startswith("w"):
                base = regs.get(dst)
                v = imm_of(operands[1])
                sh = shift_of(operands[1])
                if base is None or v is None:
                    regs[dst] = None
                else:
                    mask = 0xffff << sh
                    regs[dst] = (base & ~mask) | ((v & 0xffff) << sh)
            continue

        if mn in ("orr", "add", "and", "sub", "eor") and len(operands) == 3:
            dst, a, b = operands
            if dst.startswith("w"):
                av = 0 if a in ("wzr", "xzr") else regs.get(a)
                bv = imm_of(b) if b.startswith("#") else (0 if b in ("wzr", "xzr") else regs.get(b))
                if av is None or bv is None:
                    regs[dst] = None
                else:
                    if mn == "orr":
                        regs[dst] = (av | bv) & 0xffffffff
                    elif mn == "add":
                        regs[dst] = (av + bv) & 0xffffffff
                    elif mn == "and":
                        regs[dst] = (av & bv) & 0xffffffff
                    elif mn == "sub":
                        regs[dst] = (av - bv) & 0xffffffff
                    else:
                        regs[dst] = (av ^ bv) & 0xffffffff
            continue

        if mn in ("uxth", "ubfx", "lsr", "lsl", "and") and len(operands) >= 2:
            dst = operands[0]
            if dst.startswith("w"):
                v = regs.get(operands[1]) if operands[1].startswith("w") else None
                if mn == "uxth":
                    regs[dst] = None if v is None else v & 0xffff
                elif mn == "lsr" and v is not None:
                    s = imm_of(operands[2]) if len(operands) > 2 else None
                    regs[dst] = None if s is None else v >> s
                elif mn == "lsl" and v is not None:
                    s = imm_of(operands[2]) if len(operands) > 2 else None
                    regs[dst] = None if s is None else (v << s) & 0xffffffff
                else:
                    regs[dst] = None
            continue

        if mn == "bl":
            target = ops.strip()
            for c in CALLS:
                if c in target:
                    w0 = regs.get("w0")
                    w1 = regs.get("w1")
                    out.append((func, addr, c, w0, w1))
            clear_caller_saved()
            continue

        if any(op.startswith(b) for b in BRANCHES):
            regs = {k: v for k, v in regs.items() if k.startswith("x") or k in ("w19", "w20", "w21", "w22", "w23", "w24", "w25", "w26", "w27", "w28")}
            continue

        # any other instruction writing w0..w18 -> unknown (be conservative)
        if operands and operands[0].startswith("w"):
            regs[operands[0]] = None

    for func, addr, c, w0, w1 in out:
        i = "0x%05x" % w0 if w0 is not None else "?"
        v = "0x%08x" % w1 if w1 is not None else "?"
        print("%s\t%05x\t%s\t%s\t%s" % (func, addr, c, i, v))


main(sys.argv[1])
