#!/usr/bin/env python3
"""把设备当前 PON MAC 区 dump 下来，与原厂 O5 快照做差分。"""
import json, subprocess, sys

DEV = ["sshpass", "-p", "Zte521", "ssh", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "root@192.168.1.1"]

BASE = 0x17058000
NWORDS = 0x6000 // 4
CH = 256


def dev(cmd):
    r = subprocess.run(DEV + [cmd], capture_output=True, text=True, timeout=180)
    return r.stdout


def dump():
    out = {}
    for off in range(0, NWORDS, CH):
        pa = BASE + off * 4
        txt = dev("/tmp/zxr dump 0x%x %d" % (pa, CH))
        n = 0
        for line in txt.splitlines():
            if ":" not in line:
                continue
            head, rest = line.split(":", 1)
            try:
                base = int(head.strip(), 16)
            except ValueError:
                continue
            for i, tok in enumerate(rest.split()):
                try:
                    out[base + i * 4] = int(tok, 16)
                    n += 1
                except ValueError:
                    break
        if n < CH:
            print("warn: chunk 0x%x got %d/%d" % (pa, n, CH), file=sys.stderr)
    return out


def main():
    cur = dump()
    print("dumped %d registers" % len(cur), file=sys.stderr)

    snap = json.load(open("/home/wen/WorkBuddy/zx/f7610u/work/fw_o5_live.json"))
    ref = {}
    for k, v in snap.items():
        try:
            ref[int(k, 16)] = int(v)
        except (ValueError, TypeError):
            pass
    print("snapshot %d registers" % len(ref), file=sys.stderr)

    diffs = []
    missing = []
    for a in sorted(ref):
        if a not in cur:
            missing.append(a)
        elif cur[a] != ref[a]:
            diffs.append((a, cur[a], ref[a]))

    print("\n=== DIFF (addr, device_now, factory_O5) ===")
    for a, c, r in diffs:
        print("0x%08x  dev=0x%08x  o5=0x%08x" % (a, c, r))
    print("\ntotal diff: %d / %d snapshot regs (missing %d)"
          % (len(diffs), len(ref), len(missing)))

    json.dump({"0x%08x" % k: v for k, v in cur.items()},
              open("/home/wen/WorkBuddy/zx/f7610u/work/dev_o4_now.json", "w"),
              indent=0)


if __name__ == "__main__":
    main()
