#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 zte_xgpon.ko 反汇编里还原 HAL 寄存器读改写序列。

用法: hal_extract.py <objdump.txt> <func> [func2 ...]
或   hal_extract.py <objdump.txt> --all   (只打印所有 Hal_* 调用点汇总)
"""
import re
import sys

SYM = {}

RD = {"Hal_ReadXmacReg", "Hal_ReadGmacReg", "Hal_ReadPonGreg"}
WR = {"Hal_WriteXmacReg", "Hal_WriteGmacReg", "Hal_WritePonGreg"}

INS = re.compile(r"^\s+([0-9a-f]+):\t(.+)$")


def parse(path):
    funcs = {}
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^([0-9a-f]{16}|[0-9a-f]{8,}) <(\S+)>:", line)
        if m:
            cur = m.group(2)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        m = INS.match(line.rstrip("\n"))
        if not m:
            continue
        addr = int(m.group(1), 16)
        text = m.group(2).split("//")[0].strip()
        parts = text.split(None, 1)
        if not parts:
            continue
        mn = parts[0]
        ops = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
        ops = [o.split("<")[0].strip() for o in ops]
        funcs[cur].append((addr, mn, ops))
        # 同时记录 bl 的符号名
        if mn in ("bl", "b") and "<" in text:
            SYM[addr] = text.split("<", 1)[1].rstrip(">")
    return funcs


def target_of(op):
    m = re.match(r"^([0-9a-f]+)$", op)
    return int(op, 16) if m else None


def movimm(insns, idx, reg):
    """从 idx 往前找 reg 的立即数赋值(mov/movk 组合)。返回 (value, start_idx) 或 None"""
    val = None
    hi = 0
    start = None
    for j in range(idx, max(idx - 8, -1), -1):
        _, mn, ops = insns[j]
        if not ops or ops[0] != reg:
            # movk 的写法是 "movk x20, #0x41, lsl #16"
            continue
        if mn in ("mov", "movz") and len(ops) == 2 and ops[1].startswith("#"):
            val = int(ops[1][1:], 0)
            start = j
            break
        if mn == "movk" and len(ops) >= 3:
            sh = 0
            if "lsl" in " ".join(ops):
                sh = int(re.search(r"lsl #(\d+)", " ".join(ops)).group(1))
            hi = int(ops[1][1:].split()[0], 0) << sh
            # 继续往前找 mov
            for k in range(j - 1, max(j - 6, -1), -1):
                _, mn2, o2 = insns[k]
                if o2 and o2[0] == reg and mn2 in ("mov", "movz") and o2[1].startswith("#"):
                    return (int(o2[1][1:], 0) | hi, k)
            return (hi, j)
    if val is not None:
        return (val, start)
    return None


def analyze(funcs, name, out):
    insns = funcs.get(name)
    if not insns:
        out.write("!! 未找到函数 %s\n" % name)
        return
    out.write("\n### %s\n" % name)
    # 建立 地址→索引
    amap = {a: i for i, (a, _, _) in enumerate(insns)}

    for i, (addr, mn, ops) in enumerate(insns):
        if mn != "bl" or not ops:
            continue
        tgt = target_of(ops[0])
        if tgt is None:
            continue
        # 反汇编里 bl 的目标是符号名(已去掉<>)，也可能是地址
        callee = SYM.get(addr)
        if callee is None:
            # 目标地址 → 函数名
            for n, lst in funcs.items():
                if lst and lst[0][0] == tgt:
                    callee = n
                    break
        if callee is None:
            continue
        if callee not in RD and callee not in WR:
            continue
        # 找 w0
        r = movimm(insns, i - 1, "w0")
        if r is None:
            # 可能是 mov w0, w19 之类
            for j in range(i - 1, max(i - 8, -1), -1):
                _, m2, o2 = insns[j]
                if m2 == "mov" and len(o2) == 2 and o2[0] == "w0" and re.match(r"^w\d+$", o2[1]):
                    rr = movimm(insns, j - 1, o2[1])
                    if rr:
                        r = rr
                    break
        reg = ("0x%x" % r[0]) if r else "?"
        if callee in RD:
            out.write("  [%04x] READ  %-7s  %s\n" % (addr, reg, callee))
        else:
            # 找回写值: 只扫"上一次 READ 调用之后"到本 WRITE 之间的 w1 操作
            j0 = 0
            for j in range(i - 1, max(i - 40, 0), -1):
                a2, m2, o2 = insns[j]
                if m2 == "bl" and SYM.get(a2) in RD:
                    j0 = j
                    break
            mods = []
            for j in range(j0 + 1, i):
                _, m2, o2 = insns[j]
                if len(o2) >= 3 and o2[0] == "w1" and m2 in ("orr", "and", "bic", "eor"):
                    mods.append("%s %s" % (m2, o2[2]))
                if m2 == "mov" and len(o2) == 2 and o2[0] == "w1" and o2[1].startswith("#"):
                    mods.append("= %s" % o2[1])
            out.write("  [%04x] WRITE %-7s  %-40s %s\n" % (addr, reg, " ; ".join(mods) or "(原值)", callee))
    out.write("\n")


def main():
    path = sys.argv[1]
    funcs = parse(path)
    out = sys.stdout
    if len(sys.argv) > 2 and sys.argv[2] == "--all":
        names = sorted(n for n in funcs if n.startswith(("fi_", "zte_api")))
        tot = 0
        for n in names:
            c = sum(1 for a, m, o in funcs[n] if m == "bl")
            tot += c
        out.write("funcs=%d bl=%d\n" % (len(names), tot))
        return
    for n in sys.argv[2:]:
        analyze(funcs, n, out)


main()
