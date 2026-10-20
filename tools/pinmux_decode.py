#!/usr/bin/env python3
"""按 pinctrl-zx279133.c 的算法，算出所有 pin 的 mux 值，找 mux==0 的（功能模式）"""

# 驱动里的表
BANK_PINS = [15, 10, 18, 13, 10, 4]
MUX_RANGES = [
    [(5, 3), (1, 2), (2, 1), (2, 2), (2, 1), (3, 2)],   # bank0
    [(2, 3), (3, 2), (3, 3), (2, 2)],                   # bank1
    [(2, 2), (7, 1), (2, 2), (1, 3), (6, 1)],           # bank2
    [(6, 3), (7, 2)],                                   # bank3
    [(2, 2), (8, 1)],                                   # bank4
    [(4, 2)],                                           # bank5
]
# gpio_mux: {start, count, value}
GPIO_MUX = [
    (0, 5, 3), (5, 3, 1), (8, 2, 3), (10, 2, 1),
    (12, 2, 3), (14, 1, 1), (15, 2, 3), (17, 2, 1),
    (19, 4, 3), (23, 1, 1), (24, 1, 2), (25, 2, 3),
    (27, 7, 1), (34, 2, 2), (36, 1, 3), (37, 6, 1),
    (43, 6, 3), (49, 2, 2), (51, 1, 1), (52, 4, 2),
    (56, 2, 2), (58, 8, 1), (66, 1, 3), (67, 1, 2),
    (68, 2, 3),
]

BANK_VALS = [0x001995ff, 0x0010dd75, 0x000fefff, 0x000000fd, 0x000020ec, 0x0aabaaaa]

NAMES = []
for b, n in enumerate(BANK_PINS):
    for i in range(n):
        NAMES.append(f"p{b}-{i}")

# 建立 pin_data
pin_data = []
idx = 0
for bank in range(6):
    shift = 0
    for count, width in MUX_RANGES[bank]:
        for e in range(count):
            pin_data.append({
                "bank": bank,
                "offset": bank * 4,
                "shift": shift + e * width,
                "width": width,
            })
            idx += 1
        shift += count * width

# gpio_mux 数组
gpio_mux = [0] * len(pin_data)
for start, count, val in GPIO_MUX:
    for p in range(start, start + count):
        gpio_mux[p] = val

print(f"{'pin':8s} {'bank':5s} {'shift':5s} {'width':5s} {'mux':4s} {'gpio':4s} {'status'}")
print("-" * 55)
func_pins = []
for i, d in enumerate(pin_data):
    v = BANK_VALS[d["bank"]]
    mux = (v >> d["shift"]) & ((1 << d["width"]) - 1)
    tag = ""
    if mux == 0:
        tag = "FUNC <-"
        func_pins.append(NAMES[i])
    elif mux == gpio_mux[i]:
        tag = "gpio"
    print(f"{NAMES[i]:8s} {d['bank']:<5d} {d['shift']:<5d} {d['width']:<5d} "
          f"{mux:<4d} {gpio_mux[i]:<4d} {tag}")

print()
print(f"mux==0 (功能模式) 的引脚共 {len(func_pins)} 个：")
print("  " + ", ".join(func_pins))
