# F7610U PON 功能实施方案（2026-10-17）

## 一、结论摘要

| 问题 | 答案 |
|---|---|
| 515xg 的 PON 管理页能复用吗 | **能**。页面是纯 JS，数据靠外部命令返回 JSON，换掉数据提供者即可，前端零改动 |
| PON 功能现在什么状态 | **完全空白**：DTS 无节点、内核无 I2C 驱动、无 PON MAC 驱动、无光模块驱动 |
| 完整 GPON（注册上 OLT）能做吗 | **短期不可能**。厂商 PON 栈是闭源 4.1.25 模块，依赖 BSP 符号，6.18 无法加载；重写协议栈 = 人月级 |
| 能做出什么 | ① 光模块 DDM（厂商/SN/温度/电压/偏置/收发光功率/波长）② PON MAC 状态（LOS/同步）③ LuCI 页面 |

## 二、515xg PON 管理页（可复用资产）

路径：`/home/wen/515xg/ponwrt/feeds/pon_userspace/luci-app-pon`（96 KB）

| 文件 | 作用 | 数据依赖 | 移植动作 |
|---|---|---|---|
| `view/pon/status.js` (510 行) | 状态页：线路模式/生命周期/光功率/注册/计数 | `/usr/bin/pondctl status --line X` → JSON | **重写 pondctl**，保持 JSON schema |
| `view/pon/config.js` (315 行) | 配置：线路模式、OMCI/OAM、板卡数据写分区 | UCI `pon` + `/usr/libexec/airoha-pon-data` | 改 UCI schema + 分区读写工具 |
| `view/pon/hardware.js` (117 行) | 板卡身份（SN/厂商/型号） | `/usr/libexec/pon-board-identity` | 改成 ZTE 版本 |
| `view/pon/diagnostics.js` (112 行) | 诊断包收集/重启 | `/usr/libexec/airoha-pon-debug` | 可暂缓 |
| `po/zh_Hans/pon.po` | 中文翻译 | — | 直接复用 |
| `menu.d` / `acl.d` | 菜单 `admin/network/pon` + ACL | — | 直接复用（改依赖） |

`pondctl status` 返回的 JSON schema（status.js 消费）：
```
{ line: {lifecycle, optical_signal, configured_mode, active_mode, mode_pending, ...},
  frontend: {rx_power_dbm, tx_power_dbm, temperature_celsius, ...},
  registration: {mpcp_state/onu_state, ...},
  datapath: {...}, counters: {...} }
```

## 三、厂商 PON 架构（全部闭源）

```
用户态（厂商 CLI / /proc）
   ↓ /dev/ponreg(ioctl) + /proc/gpon_sdk
zx_ponreg.ko   寄存器读写：fpga_read_reg / fpga_write_reg / fpga_read_reg_one_module
gpondrv.ko     GPON 协议：T-CONT / GEM / Ploam / 告警（vermagic 4.1.25）
optical        光模块 DDM：OpticalI2cRead / CalcPonTx/RxOpticalPower /
               GetOpticalTemperature / GetOpticalVoltage / GetOpticalTXCurrent / GetOpticalWaveLength
GN28L96        光模块芯片驱动
```

`gpondrv.ko` 的 vermagic：**`4.1.25 SMP mod_unload aarch64`**，未定义符号含 `CspGetPortInfo`、
`enhanced_security_control_omci` → 绑定厂商 BSP，**6.18 上不可加载**。

## 四、硬件事实（来自厂商运行时快照 + DTS）

| 项 | 值 |
|---|---|
| PON MAC | `/soc/pon@17000000`，compatible `zte,zx279133-pon`，IRQ 6 |
| PON MAC reg | `0x17000000+0x1000000`、`0x10e00000+0x1000`、`0x10e10000+0x10000`、`0x10e20000+0x1000`、`0x14f11000+0x1000` |
| PON SerDes | `/soc/pon_serdes@16000000`（厂商 status=disabled），PHY 驱动源码在手（447 行，未编译） |
| pon_plat | compatible `zxic,pon-plat`（逆向代码 `docs/reverse-engineering/plat_132/recovered/plat_probe.c`） |
| I2C 控制器 | `i2c@14f03000`（i2c0，IRQ 0x2b）、`i2c@14f04000`（i2c1，IRQ 0x2c），compatible `zte,zx-i2c`，驱动名 `zx_i2c`，100 kHz |
| 光模块 | `GN28L96` 模块 + `optical` DDM；原厂**未暴露 i2c-dev**，PON 驱动内部直接访问 |
| 内核现状 | 无 `zte,zx-i2c` 驱动、无 PON 节点、无光模块驱动、无 GPIO 节点 |

## 五、实施方案（分阶段）

### 阶段 1：光模块可见（第一个能落地的成果）
1. 摸清 ZX I2C 控制器寄存器格式（`0x14f03000`）
   - 途径 A：反汇编原厂 kernel1 里的 `OpticalI2cRead`（有 kallsyms 地址 `ffffffc000462548`）
   - 途径 B：写探测程序直接读写控制器（纯读，风险低）
2. 写极简 i2c 驱动 → 扫描 `0x50`/`0x51`，确认光模块应答
3. 读 SFF-8472 A0/A2 页 → 厂商/PN/SN/波长/温度/电压/偏置/收发光功率
4. 写 `pondctl`（输出 515xg schema）→ 装 `luci-app-pon` → 页面出数据

### 阶段 2：PON MAC 状态
- 映射 `0x17000000` 区，读 LOS / 同步 / 链路状态寄存器
- 填进 status.js 的 `line.lifecycle` / `line.optical_signal`

### 阶段 3：完整协议栈（不现实，需单独立项）
- Ploam / GEM / T-CONT / DBA / OMCI / 加密
- 或考虑从 Airoha AN7581 的 `airoha-xpon` 源码借鉴（但硬件寄存器完全不同，参考价值有限）

## 六、待确认（需文哥定）

1. 先做**阶段 1**（光模块 DDM + 页面）？
2. 光模块是否已插上、光纤是否接 OLT？（影响能否验证光功率）

---

## 附：2026-10-17 20:00-20:40 I2C 攻坚实录

### 已完全解出（来自原厂 4.1.25 内核反汇编，`work/k1_vmlinux.bin`）

**I2C 控制器 @ `0x14f03000`（i2c0）/ `0x14f04000`（i2c1）**

| 偏移 | 名称 | 说明 |
|---|---|---|
| 0x00 | CTRL | 读回 `0x01020000`，写 bit0 无效 |
| 0x04 | CMD | `0x01` 空闲 / `0x41` 写 / `0x51` 读(7bit) / `0x53` 读(10bit) |
| 0x0c | ADDR_HI | `(addr>>7)&7`，7-bit 时为 0 |
| 0x10 | ADDR_LO | `addr & 0x7f` |
| 0x14 | SCL_DIV | 分频值，实测 2 |
| 0x18 | ? | 回读 2 |
| 0x1c/0x20 | INT_EN | 回读 0x1f，bit7 写不进（自清除） |
| 0x24 | DATA | 32-bit 数据口，读空 FIFO 返回 0x00/0xff |
| 0x28 | STAT | bit1=NAK，bit2/3=错误，**bit6=1 表示忙**（原厂 `tbnz w0,#6` 继续等） |
| 0x2c | TX_STAT | bit1=1 表示空 |
| 0x30 | RX_STAT | **bit1=1 表示空**（原厂 `tbnz w0,#1` 继续等）；读它有副作用，每次 -4 |

**函数**（真实 VA）：`zx_i2c_read_data` @0x4248d8、`zx_i2c_write_data` @0x4249d8、
`zx_i2c_wait` @0x424690、`zx_i2c_init_ctr` @0x424f28、`zx_i2c_probe` @0x424ca0、
`zx_i2c_xfer` @0x4245d0。真实 VA = `0x84000000` 不适用，**内核**是 `0xffffffc000080000 + Image 偏移`
（原厂 FIT load=0x80080000，gzip，Image 解压后 39,867,200 B）。

**光模块驱动 `optical`**（kallsyms）：`OpticalI2cRead`、`CalcPonTx/RxOpticalPower`、
`GetOpticalTemperature/Voltage/TXCurrent/WaveLength`。
- `OpticalI2cRead(addr8, reg, buf, len)` 用**标准 Linux i2c**：两个 `i2c_msg`（写 reg + 读 len），
  地址传 **8-bit**（`0xA0`=A0页 / `0xA2`=A2页，即 7-bit 0x50/0x51）
- 光模块探测：`OpticalI2cRead(0xA0, 0x5C, &v, 1)`，`(v & 0x30) == 0x20` → 内部校准
- `CalcPonTxOpticalPower` 读 **A2 页 0x66**（TX Power，标准 SFF-8472）

**时钟/复位（LSP CRM @ `0x14f00000`）**

| 外设 | 寄存器 | 说明 |
|---|---|---|
| i2c0 | +0x0c | bit0=pclk gate、bit1=wclk gate、bit4=**reset（1=assert）**、bit9=wclk mux、bits[17:11]=wclk divider |
| i2c1 | +0x10 | 同上 |
| uart0 | +0x24 | 回读 `0x0000080b`（bit11=1，有 divider）|
| mdio0/1 | +0x04/+0x08 | |

**关键**：系统启动时 i2c0 +0x0c = `0x0000021b` → **bit4=1，复位一直被 assert**。
清掉后变 `0x20b`，但控制器仍不产生传输。

**Pinmux @ `0x10e20000`**（bank0）：`p0-6`/`p0-7` = i2c0，实测 mux 已是 **0**（I2C 功能），引脚没问题。

### 卡点

所有软件开关都试过，`wr reg0 -> 0`（无 NAK）但 TX FIFO 计数持续递增、RX 始终空：
- mux bit9 = 0/1
- wclk divider = 0/1/39/99
- CTRL(0x00) 置 bit0
- CRM assert/release、开关时钟
- 输出表寄存器 SCL_DIV=2
- 对比 i2c0/i2c1 均同

**判断**：控制器缺少某个我们还没找到的使能条件（可能是 PON 电源域/模拟部分，或另一处 CRM 位）。
**下一步**：冷启（干净状态）后第一时间用同一模块验证；如仍不通，则继续从
`zx_i2c_probe` 的 clk/reset 框架调用链或官方 bootloader 的器件初始化里找。

### 已交付
- `tftp/f7610u-net-v8.9-i2c.itb`（27,754,032 B，md5 `8e5bbe1e5523b7864b2bd0058606779b`）
  含新驱动 `drivers/i2c/busses/i2c-zx279133.c` + DTS 两个 i2c 节点 + `CONFIG_I2C_CHARDEV=y`
- `work/pon_i2c_test/`（一次性探测模块，源码 + Makefile，可反复加载）
- 内核树改动：`drivers/i2c/busses/{Kconfig,Makefile,i2c-zx279133.c}`、`arch/arm64/boot/dts/zte/zx279131-f7610u.dts`

---

## 附2：2026-10-17 23:00-23:45 I2C 复测（干净启动）+ PON 页面交付

### ★ 重大纠正：U-Boot 不需要冷启动

23:07 实测（用户在 U-Boot 中断，非冷启）：**直接 `tftp` 成功**，27,754,032 B @ 13.8 MiB/s，
无 `alloc bp failed`。串口日志暴露真正机理 —— **U-Boot 第一次网络操作时自己会跑**：

```
pon_reset: read CRM_SW_CFG_RST_SW val = 0xffffffff
pon_reset: read CRM_SW_CFG_RST val = 0xffffffff
pon_reset: write CRM_SW_CFG_RST val = 0x0        ← 自己清成 0
boot dma_cfg:0x3f6564 / dma_init:TX_UP_DESC_DEPTH = 1024
zx_pon_init: PON_WORK_MODE = 0x17000018, val = 0x0
mac 0 phy status changed: 1000M full-duplex / Using eth0 device
```

⇒ 旧结论"必须冷启（PP virgin 态）"**作废**。正确姿势：**先直接试 tftp**；
只要 CRM 复位寄存器 = 0xffffffff（上次 bootm 前我们写进去的），U-Boot 就自己复位重建。

### I2C 现状（干净启动复测，与之前完全一致）

| 总线 | 现象 | 判据 |
|---|---|---|
| i2c-0 (0x14f03000) | **0 个设备**（全部 NAK，`dump 0x50` → I/O error） | 控制器**完全正常**，能真发传输、能判 NAK |
| i2c-1 (0x14f04000) | **51 个地址全 ACK**，读回全 0x00 | **SDA 被恒低**（假 ACK），不是真设备 |

**结论**：控制器与驱动都没问题（bus0 是铁证）。bus1 的 SDA 恒低是**硬件层面**：
光模块未供电 / 模块把 SDA 拉死 / 上拉缺失。干净启动后一模一样 ⇒ 与我们之前的读写操作无关。

**引脚已核对**（按 pinctrl 驱动的 shift/width 算法手算）：
- p0-6/p0-7 = i2c0，mux = 0 ✓
- p1-8/p1-9 = i2c1，mux = 0 ✓
- pinmux 值：bank0=0x001995ff bank1=0x0010dd75 bank2=0x000fefff bank3=0xfd bank4=0x20ec bank5=0x0aabaaaa
- CRM：i2c0 `+0x0c` = 0x21b，i2c1 `+0x10` = 0x21b（`zx-clock,reset-bit=<0x04>` = bit4，`mux-bits=<0x09 0x01>` = bit9）
- 厂商 DTS 的 `i2c0_clk`/`i2c1_clk` 节点确认了 regmap 偏移与位定义

### ⚠ 事故：不要往 PON 复位寄存器写 0

在 Linux 里执行 `echo 1a2c0004 0 > zx_poke` / `echo 17240004 0 > zx_poke` 后
**网络立即全断**（ping/ssh 全失败，串口只剩 `tx: hw done` 打印，无 console）。
**只能断电恢复**。⇒ 这两个寄存器**不能随意改写**，尤其在系统运行时。

### 交付：固件 v9.1-pon

| 项 | 内容 |
|---|---|
| 文件 | `tftp/f7610u-v9.1-pon.itb`（30,572,080 B，md5 `0ba1f578c346995a4ebd1d72be5fe036`）|
| 内核 | 6.18.38 + `zx279131-net`（v8.8c CLA）+ **`zx_i2c` 驱动**（`/dev/i2c-0/1`）|
| 页面来源 | **https://github.com/czghzh/openwrt-pon-userspace.git**（clone 到 `f7610u/pon-userspace/`，840K）|
| 页面文件 | 仓库版 `luci-app-pon`：status.js(498) config.js(309) hardware.js(117) diagnostics.js(112) |
| 数据源 | 自写 `usr/sbin/ponctl`（主状态，schema_version=1）+ `usr/bin/pondctl`（OMCI/OAM）|
| 配置 | `/etc/config/pon`：xpon/omci/oam |
| 菜单 | `PON 光线路 → 状态 / 配置`（中文；hardware/diagnostics 因依赖缺失自动隐藏）|
| 其他 | 防火墙 firewall4 + luci-app-firewall、中文 zh_cn、时区 CST-8、argon 主题 |

**实测**：`ponctl --device pon status --json` 输出正常
（`lifecycle: stopped` / `optical_signal: false` / `mpcp_state: wait` / `onu_state: O1`）；
4 个 JS 静态资源 HTTP 200（17KB/9.6KB/3.2KB/3.5KB）。

### 编译坑（再次踩到）

`immortalwrt/target/linux/zte/zx279131/config-6.18` 会覆盖内核树 `.config`：
- 必须在此文件里显式写 `CONFIG_I2C=y` / `CONFIG_I2C_CHARDEV=y` / `CONFIG_I2C_ZX279133=y`
- 否则 `make -j8` 出来的内核**没有 I2C 驱动**（第一次就是这么丢的）
- 同理 `CONFIG_NET_VENDOR_ZTE=y` + `CONFIG_ZTE_ZX279131_ETH=y`（已有注释警告）

### 下一步（I2C 硬件）

1. 找光模块供电/使能（PON 电源域、GPIO、或 zx_ponreg 的电源寄存器）
2. 用 GPIOLIB 把 p1-8/p1-9 临时切 GPIO 输入，读实际电平（确认是"被拉低"还是"没上拉"）
3. 或者从原厂 `optical.ko` 反汇编找它注册的 adapter（确认光模块到底是 i2c0 还是 i2c1）

---

## 附3：2026-10-18 00:00 光模块链路彻底解剖（反汇编 bspdriver/plat）

### ★ 纠正：i2c1 是死路，原厂根本禁用

从**原厂 F7610U DTB 反编译**（`work/k1_logic.fit` → fdt → `/tmp/f7610u-stock.dts`）：

```dts
i2c@14f03000 { compatible="zte,zx279131-i2c"; status = "okay";     };  // i2c0 启用
i2c@14f04000 { compatible="zte,zx279131-i2c"; status = "disabled"; };  // ★ i2c1 禁用
```

⇒ 之前"光模块在 i2c1"的判断**完全错误**。i2c1 全 ACK 是"控制器没时钟产生的假 ACK"，
不需要再管它。**光模块 I2C 走 i2c0**（但当前扫描全 NAK = 没供电）。

### ★ 光模块不在 zx_i2c 直连，而走 BOB 控制器

`bspdriver.ko`（**not stripped**，符号完整）反汇编：

```c
// 光模块读写：标准 i2c_transfer，但有"假模块"分支
bob_i2c_readb(u8 addr, u8 reg, u8 *val) {
    if (flag == 0 && addr == 0x50) { *val = table[reg]; return 0; }  // 无模块时返回静态表
    client = generic_addr_to_client(addr);   // 按地址找已注册的 i2c client
    i2c_transfer(client->adapter, msgs, 2);
}

// 光模块电源 + 激光器开关
hw_power_optx_set(int on) {
    do_cmd_set(25, on != 0, 0);              // ★ 表索引 25 → 一个 GPIO
    bob_i2c_writeb(0x51, 0x7f, 6);
    bob_i2c_readb (0x51, 0x6e, &v);
    if (on) v &= ~0x40; else v |= 0x40;      // ★ A2[0x6e] bit6 = TX_DISABLE
    bob_i2c_writeb(0x51, 0x6e, v);
}

// 光接收信号检测（RXSD / LOS）
optical_signal_rxsd_get(u32 *out) {
    *out = gpio_pin_value_scan(24);          // ★ GPIO 24
}

// BOB 命令 = 纯 GPIO
do_cmd_set(u8 idx, u8 val, u8 expect) {
    entry = current_board->gpio_tbl + idx*0x18;
    if (entry->type != 1) return -1;
    zte_gpio_set_value(entry->gpio, val);
}
```

`current_board` 是 **外部符号**，由板级驱动 `plat-zxylzb_9131.ko` 提供（`init_module` →
`board_ability_identify()` 选板型后填充）。

### 引脚真相（推翻我之前的推断）

| 引脚 | 真实用途 | 证据 |
|---|---|---|
| p0-6 / p0-7 | i2c0 SCL/SDA | mux=0，控制器正常工作（真 NAK）|
| p0-9 | 原厂 SR1010 的 i2c1 之一（F7610U 上 i2c1 disabled，无意义）| DTB |
| **p1-9** | **RXSD（RX Signal Detect）** | `optical_signal_rxsd_get` → GPIO 24 = p1-9 |
| p1-8 | 未知（mux=0，实测低电平）| — |

实测 p1-8 = 0、p1-9 = 0（切 GPIO 后 DATA=0xa026 三次一致）⇒ **RXSD = 0**
（无光信号，或光模块没电导致 RXSD 无输出——两者当前无法区分）。

### PON SerDes 完全未初始化（实测）

| 寄存器 | 值 | 含义 |
|---|---|---|
| `0x17000000` | `0x27913100` | PON 芯片 ID，**芯片有电** |
| `0x10e300d0` | `0x00000000` | PLL_STATUS bit0 = **PLL 未锁定** |
| `0x10e300e4` | `0x00000000` | RX_STATUS：LOS=0 但 CDR_LOCK=0（没跑起来）|
| `0x1700004c` / `0x170000fc` | `0` / `0` | PON 核心复位/时钟寄存器全 0 |

`plat-zxylzb_9131.ko` 的 `zx_pon_clk_reset_init` 反汇编（PON 复位/时钟序列）：

```c
zx_pon_clk_reset_init(mode) {
    ref_clk_set();
    base = *pon_base;
    base[0x4c] &= ~BIT(24);           // 复位 assert
    base[0x4c] &= ~BIT(8);
    udelay(10 * 0x418958);            // 长延时
    base[0x4c] |=  BIT(24);           // 复位 release
    udelay(10 * 0x418958);
    reg_def_set();
    apb_bit_write(0x30, 0, 2, 0);
    serdes_mode_set(mode, 0, 0);
    base[0x4c] |= BIT(8);             // 时钟使能
    base[0xfc] |= BIT(0);
    ...
}
```

### 完整 PON 需要什么（现状 vs 目标）

| 组件 | 状态 |
|---|---|
| `zx_i2c` 驱动 | ✅ 已写（i2c0 工作正常）|
| BOB 驱动（GPIO 表 + 光模块电源 + TX_DISABLE）| ❌ 需从 `bspdriver.ko` 移植 |
| PON SerDes PHY（PLL/复位/模式）| ⚠️ 有源码 `phy-zx279133-pon-serdes.c`，但地址是 SR1010 的（0x16000000，F7610U 是 USB）|
| PON MAC 驱动（GPON/EPON 帧、T-CONT/GEM/DBA）| ❌ 闭源（`gpondrv.ko` vermagic 4.1.25）|
| OMCI/OAM | ⚠️ 仓库有 Rust 实现（`airoha-pon-daemons`），但绑定 Airoha SDK |
| LuCI 页面 | ✅ **已交付并跑起来**（数据源为自写 ponctl，显示 stopped/无光）|

### 下一步的三个可选方向

1. **A. 点亮光模块（中等工作量，有明确产出）**
   从 `plat-zxylzb_9131.ko` 的 `current_board` 静态表里抠出 GPIO 表 → 找到索引 25 的 GPIO 号
   → 用 zx_poke 直接拉高 → 看 i2c0 是否出现 0x50 → 能读 DDM（温度/电压/光功率）
2. **B. 启动 PON SerDes（中高工作量）**
   复刻 `zx_pon_clk_reset_init` 到内核驱动，让 PLL 锁定、CDR 锁定 → RXSD 有效
3. **C. 真实 GPON 注册（人月级）**
   需要重写 Ploam/GEM/T-CONT/DBA 整个协议栈，不建议

### 事故记录（第二次）
在 Linux 里 `zx_poke 1a2c0004 0` / `17240004 0` → **网络全断、串口无 console，只能断电**。
这两个是 PON 复位寄存器，**运行时绝对不要写**。

## §63 i2c 攻坚收尾纪要（2026-10-18）

### 关键地址（全部实测）
- PON 节点（原厂 DTS）：`0x10e00000 / 0x10e10000(topcrm) / 0x10e20000 / 0x10e30000(serdes) / 0x10e40000 / 0x17000000(PON MAC, 16MB) / 0x14f0d000(efuse)`
- topcrm：`0x10e1004c = 0x01ffffff`、`0x10e10038 = 0x0828142a`、`0x10e100fc = 1`
  ⇒ **U-Boot 已把 PON 时钟与 PLL 配到 `zx_pon_clk_reset_init` 的目标态**，这步不用重做
- serdes：`0x10e3007c` bit31 = 0 ⇒ 不在复位；`0x10e30000`、`0x10e30058` 未按 vendor 序列配置（=0 / 1）
- GPIO：6 bank × 16 @ `0x10d10000 + bank*0x40`；DIR +0x00、DATA +0x14、SET +0x18、CLR +0x1c
  - **GPIO47 = bank2 bit15 = RXSD**（=1 表示有下行光）
  - **GPIO48 = bank3 bit0 = OP_TXPOWER_ON**（光模块电源，高 = 上电）
- i2c0 `0x14f03000`：SCL_DIV +0x14（0xf9 = 100 kHz）；CMD +0x04（0x41 写 / 0x51 读 / 0x01 idle）；
  DATA +0x24；STAT +0x28（bit1=NAK、bit6=busy，写 0x80 清）；TX_STAT +0x2c（bit0=full、bit1=空）；
  RX_STAT +0x30（bit1=空）

### i2c 驱动修正结论（`i2c-zx279133.c`）
原厂语义与我们最初实现**四处相反/写错**，全部已修：
1. STAT bit6 是 busy，不是 done（等到它清零才算完成）
2. RX_STAT bit1 = 1 表示 FIFO **空**（等待时轮询它，为 0 才能读 DATA）
3. TX_STAT bit0 = full（续推字节时等它清零）；bit1 = empty
4. SCL_DIV 按时钟计算（100 MHz → 249），回退值 249
5. 结束一律 `CMD=0x01` idle，STAT 写 0x80 清

### BOSA 实测读数
A0 @0x50、A2 @0x51（at24 驱动绑定成 `0-0050/0-0051`）。
`ZTE_XEPON_AS`，温度 ~40 °C，VCC 3.31 V，TX −4.89 dBm，RX power 恒 0（不可用），RXSD=1 有光。
A2 的 DDM 块**必须从 0x60 起始读**（顺序整页读 0x60+ 全 0）；A0 只前 128 字节有效。

### 工具
- `work/zxr.c` → `tftp/zxr`：/dev/mem 读写/dump + 原生 i2c（`rd/wr/dump/i2c <a7> <off> <len>`）
- `work/i2cprobe.c` → `httpdrop/i2cprobe`：scan/read/dump，**地址是十进制**（0x50 要写 80）
- 内核已开 `CONFIG_DEVMEM=y` ⇒ 设备上可直接 `/dev/mem` 调寄存器，不用每次重编

### 刷机 / 部署
- 已在 U-Boot：`cd f7610u && bash work/uboot_go.sh <itb>`
- 系统在跑：`bash work/relaunch.sh <itb>`，或 SSH `reboot` 后立刻跑 uboot_go.sh
- bootargs：`console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000 root=/dev/mtdblock7 ro rootwait`
- 复位序列：`mw 0x1a2c0004 0`→`mw 0x17240004 0`→两地址写 0xffffffff→`mw 0x1a000008`/`0x1a00000c = 0xffffffff`
- `ponctl` 改动要进 initramfs：`install` 到 `immortalwrt/build_dir/target-aarch64_cortex-a53_musl/root-zte/usr/sbin/`，
  再 `rm -f usr/initramfs_data.cpio usr/.initramfs_data.cpio.cmd` 重编

### PON MAC 栈现实评估（未做）
vendor 全栈在 `kmdule-dump/plat-zxylzb_9131.ko` + `bspdriver.ko`；`zx_pon_probe` 用固定虚地址
`0xffd0_0000_0000 + 物理地址`。关键函数 `zx_pon_clk_reset_init / pon_reset_blk / pon_reset /
serdes_mode_set(0x9ec 字节) / ref_clk_set / reg_def_set / apb_read|write|bit_write`。
要出真链路还差 MPCP/PLOAM/OMCI 整套协议栈（人月级），建议 PON 页以监控为主、MAC 栈另立项。

## §64 PON 页面验收与 config 页修复（2026-10-18 01:15）

### 验收手段（无浏览器，三路实测）
1. **静态**：`curl http://192.168.1.1/luci-static/resources/view/pon/{status,config,hardware,diagnostics}.js` → 全 200。
2. **菜单**：LuCI 前端 `ui.menu.load()` → `L.url('admin/menu')`，即 `GET /cgi-bin/luci/admin/menu`。
   带 `sysauth_http` cookie 拉 JSON，看每个节点的 `satisfied`。**这是菜单显不显示的唯一真源**
   （HTML 里 `#topmenu` 是空的，`style="display:none"`，JS 异步填）。
3. **数据面**：`POST /cgi-bin/luci/admin/ubus`，body
   `{"jsonrpc":"2.0","id":1,"method":"call","params":["<SID>","file","exec",{"command":"...","params":[...]}]}`。
   等价于页面里 `fs.exec()`。

### 实测结果
| 节点 | satisfied | 路由 | 数据源 |
|---|---|---|---|
| admin/network/pon | true | 200 | — |
| .../pon/status | true | 200 | `ponctl --device pon status --json` → ubus code 0 ✅ |
| .../pon/config | true | 200 | `/usr/libexec/airoha-pon-data` **缺** → ubus `result:[4]` |
| .../pon/hardware | **false** | 404 | 缺 `/tmp/pon-board-identity.available` |
| .../pon/diagnostics | **false** | 404 | 缺 `/usr/libexec/airoha-pon-debug` |

### 修复：config.js load 崩溃
原实现 `fs.exec('/usr/libexec/airoha-pon-data',['list'])` 不兜底 ⇒ 命令不存在时 view load reject
⇒ **菜单可见、点进去白屏/报错**。改为：

```js
return L.resolveDefault(fs.exec('/usr/libexec/airoha-pon-data', [ 'list' ]), null)
	.then(function(result) {
		if (!result || result.code != 0) return [];
		return parseStorageList(result.stdout);
	});
```
`parseStorageList` 另加 try/catch，防 helper 输出非 JSON。storage 为空时 render 里
`if (!storages.length) return map;` 自动跳过 board-data 上传块，页面退化为纯 UCI 表单。

### 产物
`tftp/f7610u-v9.7-cfgfix.itb` 30,642,060 B，md5 `eb8b9cc863a61585f924b28f5cc8556a`。

### 打包注意
`mkimage` 在 `immortalwrt/staging_dir/host/bin/`（非交互 ssh 的 PATH 里没有 mkimage/dumpimage）。
增量重编流程：改 base-files → `cp` 到 `build_dir/target-aarch64_cortex-a53_musl/root-zte/...`
→ 内核树 `rm -f usr/initramfs_data.cpio usr/.initramfs_data.cpio.cmd`
→ `make ARCH=arm64 CROSS_COMPILE=aarch64-openwrt-linux-musl- -j8 Image`（16 秒）→ `mkimage -f f7610u-bringup.its`。

### 遗留
- `pondctl` 仍是早期草稿（走 `i2cget`，busybox 无此工具）⇒ 恒返回 `optical_signal:false` +
  `frontend.error:"not present"`，与 ponctl 矛盾。status.js 只用它取协议侧状态，暂不影响主面板。
- hardware / diagnostics 两页待补 ZTE 版 helper（前者还需 flash 里的 SN/MAC，当前内核无 flash 驱动）。

## §65 PON 页四视图全通 + 中文 + 去 Argon（2026-10-18 01:30）

### 新增 helper（ZTE 版，替代 Airoha 专有件）
1. **`/usr/libexec/pon-board-identity`**（硬件信息页）
   - 接口：`list` → `{"targets":{...}}`；`read <target> <field>` → 值；`write` → 退出 1（只读）
   - target `module`（BOSA A0 + ponctl）：vendor / part_number / serial / temperature / voltage / tx_power
   - target `board`：soc（`/proc/device-tree/compatible` 首串 = `zte,zx279131-f7610u`）、model
     （`ZTE F7610U`）、pon_mac（eth0）、lan_base_mac（br-lan）
   - ★ **A0 必须整页读 128 字节再切片**：`hexdump -s 20 -n 16` 这种短读在本 i2c 控制器上会**整体错位一字节**
     （实测同一地址短读得 `...415359000000`，整页读得 `...415300`）。整页读才与 ponctl 一致。
   - 实测：vendor `ZTE_XEPON_AS`（13 字节区实际是 `ZTE_XEPON_ASY`? 否 —— 整页读为 12 字符，短读多出的 `Y` 是错位产物），
     part_number / serial 该模块**没写**（A0 0x20 之后全 0），温度 42.26 / 电压 3.3125 / TX −4.89。
2. **`/usr/libexec/airoha-pon-debug`**（诊断页）
   - `collect` → 打包 `/tmp/pon-debug.tar.gz`（system / dmesg / dmesg-pon / pon.txt / bosa / gpio / net / logread，实测 24–27 KB）
   - `restart` → `{"ok":false,"error":"no PON protocol stack..."}`，**老实报失败**，不假装成功
   - 配套 `/etc/config/airoha-pon-debug`（`config capture 'capture'`），否则 `form.Map` 无节可渲染
3. **`/usr/bin/pondctl` 重写**（原来走 `i2cget`，busybox 没这工具 ⇒ 恒报 `optical_signal:false`）
   - 输出**扁平** values（页面直接当 `values` 用，不是嵌套 `{line:...}`）
   - 协议侧如实报 `channel_available:false / backend_state:inactive / authentication_meaning:not-reported`
   - `optical_signal` 从 `ponctl --device <uci 名> status --json` 取，与主面板不再矛盾

### menu.d / hardware.js 改动
- `admin/network/pon/hardware` 的 depends 从 `/tmp/pon-board-identity.available`（临时标记文件，从不存在）
  改为 `/usr/libexec/pon-board-identity` 可执行 —— 标记文件方案本身是错的。
- `hardware.js`：字段标签改中文；`m.readonly = true`（数据全来自硬件，没有可写项，
  不该给一个必然失败的「保存」按钮）。

### 验收（2026-10-18 01:30，v9.10）
| 项 | 结果 |
|---|---|
| 菜单 5 节点 satisfied | 全 **true** |
| 路由 /pon{,/status,/config,/hardware,/diagnostics} | 全 **200** |
| ubus `file.exec` 四项（ponctl / pondctl / identity list / debug collect） | 全 `code:0` |
| `mediaurlbase` | `/luci-static/bootstrap` |
| `pon.zh-cn.lmo` | 已装，`TR` 字典里能查到「PON 线路」等 |

### 去 Argon（文哥交代）
- `.config`：`CONFIG_PACKAGE_luci-theme-argon=y` → `# ... is not set`（已备份 `.config.bak-*`）
- `98-sr1010-defaults`：`luci.main.mediaurlbase` 由 `/luci-static/argon` → `/luci-static/bootstrap`
- root-zte 里清掉全部残留（**只删目录不够**）：
  `www/luci-static/argon`、`usr/share/ucode/luci/template/themes/argon`、
  `usr/libexec/rpcd/luci.argon_wallpaper`、`usr/share/rpcd/acl.d/luci-theme-argon.json`、
  `www/luci-static/resources/menu-argon.js`、`lib/apk/packages/luci-theme-argon.*`、
  `lib/upgrade/keep.d/luci-theme-argon`、`etc/uci-defaults/30_luci-theme-argon`、`lib/apk/db/installed` 里那条
- ★ **`etc/uci-defaults/30_luci-theme-argon` 必须删**：它会把 mediaurlbase 强行设回 argon，
  argon 目录又没了 ⇒ 页面 CSS 404 白板。只删目录不删这个脚本 = 必翻车。

### 中文界面的真正缺口
LuCI 页面文字走 `_()` + `/usr/lib/lua/luci/i18n/<pkg>.<lang>.lmo`。之前只有 base/firewall 两个 lmo，
所以菜单是中文（menu.d 里写死中文）、**页面内容是英文**（`_()` 没词典）。
补法：仓库 `pon-userspace/luci-app-pon/po/zh_Hans/pon.po`（300 条）→
`immortalwrt/staging_dir/hostpkg/bin/po2lmo` 编译 → `root-zte/usr/lib/lua/luci/i18n/pon.zh-cn.lmo`。

### 产物
`tftp/f7610u-v9.10-zhcn.itb` 30,117,772 B，md5 `9a9db21249bb204b8e3e9bf90998d2d9`（设备在跑）。

## §66 PON 功能本体评估（2026-10-18 01:40）

### 一、vendor 协议栈能不能直接搬？——**不能**
实测 `kmdule-dump/*.ko`：

| 模块 | vermagic | depends |
|---|---|---|
| `gpondrv.ko`（GPON MAC 驱动，75 KB） | **4.1.25** SMP mod_unload aarch64 | `zte_xgpon,tm,bspdriver` |
| `plat-zxylzb_9131.ko`（板级） | **4.1.25** | — |
| `bspdriver.ko` | **4.1.25** | — |

我们的内核是 **6.18.38**，差两个大版本；gpondrv 还引用一大批 4.1 专有内核符号
（`CspGetPortInfo` / `MontorSendMsg` / `OssBbxKernWrite` / `hw_power_optx_set` / `optical_signal_rxsd_get`
/ `enhanced_security_control_omci` / `generate_key_omci` …），这些由 `zte_xgpon.ko` + `tm.ko` 提供。
⇒ **Ko 移植路线死了**（不是 vermagic 改几个字节的事，是 API 与符号体系全不对）。

gpondrv 里的函数清单（有符号，未 strip）说明这套栈的规模：
`gpondrvCfgRegisterValueAdapter / gpondrvCfgMulGemportAdapter / gpondrvCfgTcontMaxAdapter /
gpondrvGetEqdAdapter / gpondrvGetPowerAdapter / gpondrvPonFlowRateRecord / func_laser_transmitter_init /
gpondrv_scan_thread / gpondrv_task_thread / adapter_onuGpon{LinkState,OpState,Disable}Notify …`
—— 再加 OMCI/OAM 两张表，就是人月级工作量。

### 二、当前 PON MAC 现场（v9.12 实测）
| 寄存器 | 值 | 含义 |
|---|---|---|
| `0x17000000` | `0x27913100` | PON MAC 芯片 ID，供电正常 |
| `0x17000018` | `0x4` | PON_WORK_MODE = GPON |
| `0x10e3007c` | `0x5` | SerDes bit31=0 ⇒ **不在复位** |
| `0x10e300d0` | `0` | **PLL 未锁** |
| `0x10e300e4` | `0` | **CDR 未锁** |
| `0x1700004c` | `0` | PON MAC 未使能 |
| GPIO47（bank2 bit15） | `1` | RXSD，**下行光有** |
| `0x10d100d4`（bank3 DATA） | `0x0b` | bit0=GPIO48 高（原厂 `0x08`，即 GPIO48 低） |

读法（`/dev/mem` 对这些区返回 EFAULT，**必须走 PP 驱动的 peek 节点**）：
```sh
D=/sys/bus/platform/devices/18000000.pp
echo 0x10e300d0 1 > $D/zx_peek    # 值打到 kernel log
dmesg | tail -1
```

### 三、物理层能不能自己点亮？（未做，评估）
`plat-zxylzb_9131.ko` 里有完整序列，符号在册（可反汇编）：
`zx_pon_clk_reset_init`(712B) / `serdes_mode_set`(**2540B**) / `reg_def_set`(348B) /
`ref_clk_set` / `pon_serdes_reset` / `pon_reset` / `pon_reset_blk` / `serdes_set_tx_eq` / `serdes_set_rx_eq`。
`serdes_mode_set` 是 2540 字节代码，内部引用一张大表 —— 复刻它能让 PLL/CDR 锁定，
**但即使锁上，没有 MAC/协议栈也注册不了、上不了网**。

### 四、结论
1. PON **光层**已完成：DDM 可读、RXSD 可见、模块身份可读，页面上都有真数据。
2. PON **MAC/协议层**：vendor 路线堵死，自研 = 人月级（SerDes 序列 + GPON TC 层 + PLOAM + OMCI）。
3. 建议定位：这台设备上 PON 页做成「光层监控台」是当前最合理的交付；
   真要跑 PON，得单独立项按芯片手册重写 MAC 驱动。

### 五、本轮代码改动
- `pon-board-identity` 新增 target `pon`：`rx_signal` / `work_mode` / `serdes_reset` / `serdes_pll` / `serdes_cdr`
  （走 `zx_peek` + `dmesg` 取数；`rxsd_state` 自己 export gpio，不依赖 ponctl 先跑过）
- `ponctl`：`tx_gate_enabled` 判断从 **A2[0x6E] bit7（TX_FAULT）** 改成 **bit6（TX_DISABLE）**。
  实测该模块 0x6E=`0x82`（bit7=1 是 fault 位、bit6=0），旧代码把「有 fault」当成「关断」，报 `false`；现在正确报 `true`。
- 产物 `tftp/f7610u-v9.12-ponstat.itb`（30,117,772 B，md5 `df12d4665d51b42042bf8a4aa43b6b7f`，设备在跑）。

## §67 SerDes 逆向与实机实验（2026-10-18 02:00–02:40）

### 一、拿到的完整厂商序列（反汇编 `plat-zxylzb_9131.ko`）

基址符号（literal pool 重定位确认）：

| 符号 | 物理地址 | 用途 |
|---|---|---|
| `top_crm_base` | `0x10e10000` | PON 时钟/复位/PLL（+0x38 / +0x40 / +0x4c / +0xfc） |
| `sys_ctrl_base` | `0x10e20000` | **pinmux**（+0x10 / +0x18 / +0x20）—— 就是断网那个区 |
| `pon_serdes_base` | `0x10e30000` | SerDes 寄存器（+0x00..+0x5c） |
| `efuse_base` | `0x14f0d000` | 芯片版本（+0x20c 的 [15:12]） |
| `product_vid` | ko 内 u8 | 0x9c / 0xaa 走额外分支 |

**`zx_pon_clk_reset_init(mode)` 全流程**：
```
ref_clk_set()                       # top_crm+0x40：设 bit[9:3]=0x28，脉冲 bit2/bit10/bit11，
                                    #   然后最多 1001×4.8us 等 bit12=1（参考时钟稳定）
top_crm+0x4c &= ~0x1000000          # 清 PON 复位
top_crm+0x4c &= ~0x100              # 关时钟
delay(10×35160)                     # __const_udelay
top_crm+0x4c |= 0x1000000           # 释放复位
delay
reg_def_set()                       # ★ SerDes 全量配置，见下
apb_bit_write(0x30, value=0, width=2, bit=0)
serdes_mode_set(mode, 0, 0)         # ★ 52 条位域写，见下
top_crm+0x4c |= 0x100               # 时钟使能
top_crm+0xfc |= 1
serdes+0x58 = 0xb3e23333
top_crm+0x38 = (v & 0xff03ffff) | 0x280000
top_crm+0x38 = (v & 0xfffc003f) | 0x1400
top_crm+0x38 |= 0x08000000          # bit27
top_crm+0x38 |= 0x80000000 ; delay ; &= ~0x80000000   # PLL 复位脉冲
efuse: *efuse |= 1 → 读 efuse+0x20c & 0xf000 → 还原 *efuse
sys_ctrl+0x10 = (v & 0xffffff0f) | (efuse[0x20c] >> 8 & 0xf)   # ← pinmux，有断网风险
sys_ctrl+0x10 &= ~0x800
sys_ctrl+0x20 = (v & 0xffffff0f) | 0x90
sys_ctrl+0x18 = (v & 0xffffffe0) | 0x1a
if product_vid in (0x9c,0xaa): serdes+0x04/0x00/0x24 = 0x400019ff / 0xe009fef3 / 0x09570087
```

**`apb_bit_write(reg, value, width, bit)`** = `base+reg` 读改写，`mask=((1<<width)-1)<<bit`。
（注意参数顺序：反汇编里 w1 被左移 w3，所以是 (reg, value, width, bit)，不是常见的 (reg,bit,width,value)。）

**`reg_def_set()`**（整字写 `serdes+0x00..0x5c`，共 24 个）：
```
00:e009fef5 04:400019ff 08:00001df0 0c:00003034 10:00000000 14:118e2000
18:00850231 1c:01a1e3e4 20:c8000002 24:08570087 28:00080000 2c:00000000
30:01212000 34:00793f46 38:00000000 3c:00000000 40:003c0000 44:ea00a013
48:101038ca 4c:0005a088 50:33333333 54:33333333 58:03e23333 5c:00000000
```

**`serdes_mode_set(mode)`**：跳转表 8 项，mode 4/5 共用入口 `0xd098`。
`w6=w7=0` 时序列在 `0xd4e0` 提前返回 ⇒ 实走 `d098..d4dc`，提取出 **52 条**位域写
（bit 分布：0×17、8×16、16×6、24×11、4×1、22×1）。原始产物在 `work/pon-dis/`。

### 二、实机实验（v9.12，网络全程未断）

| 实验 | 结果 |
|---|---|
| 按厂商顺序写 `top_crm+0x4c/0xfc/0x38` + `serdes+0x58` | 写入无异常，`0x10e1004c` 回到 `0x01ffffff`，**ping 0% 丢包** |
| 写 `serdes+0x58 = 0xb3e23333` | 读回 **`0x00000033`**（只有低字节） |
| 写 `top_crm+0x38 = 0x0a28142a`（bit27） | 读回 **`0x0828142a`**（bit27 保持 0） |
| `reg_def_set` 24 个字全写 | 每个寄存器读回**都只有低字节**（0xf5/0xff/0xf0/0x34/0xe4/0x87/0x46/0x13/0xca/0x88/0x33…） |
| 位宽定位：`0x10e30014` 依次写 `0xaa` / `0xaa00` / `0xaa0000` / `0xaa000000` | 只有 `0xaa` 保持，**其余三个写回 0** ⇒ 该寄存器只有 bit[7:0] 可写 |
| 对照：`0x17000024` 写 `0x00112233` | 读回 `0x112233`（**24 位可写**）⇒ 不是全局限制 |
| 对照：`0x17000018` 写 `0x12345678` | 读回 `0x18`（**5 位可写**）⇒ 每个寄存器位宽不同是正常设计 |
| 写前先把 `0x17000018` 备份后写入再恢复 4 | 恢复成功，PON_WORK_MODE 未损坏 |

**结论性判断**：
1. PON 时钟/PLL 控制寄存器（`0x10e10000` 区）**可以安全写**，实测不中断以太网。
2. `0x10e30000` 区至少部分寄存器只有 bit[7:0] 可写，而 `reg_def_set`/`serdes_mode_set`
   大量使用 bit 8/16/24 —— 这两件事**互相矛盾**，说明要么该区确实是 8 位寄存器空间
   （厂商高位写在硅片上被丢弃，PLL 参数走别的通路），要么 `pon_serdes_base` 的映射路径与
   `ioremap` 不同（厂商用 `0xffd0_0000_0000 + pa` 的固定映射）。
3. `0x10e300d0` / `0x10e300e4` 是否真的是 PLL/CDR lock 位，**尚无独立证据**，
   之前"未锁"的判断可能读错了寄存器。
4. SerDes PLL 未确认锁定；设备重启即恢复原状（tmpfs，无持久影响）。

### 三、下一步要解决的技术分叉（二选一）
- **A. 确认寄存器访问语义**：反汇编厂商 `zx_pon_probe` 的 `ioremap` 参数与返回值，
  确认 `pon_serdes_base` 是否就是 `0x10e30000` 的普通映射，还是带特殊属性的固定映射。
  若是前者，则 8 位宽是硬件事实，序列要按"每 offset 只用低字节"重写。
- **B. 换验证通道**：查厂商是否留了 serdes 调试接口（`serdes_rx_ready` / `serdes_get_err_cnt` /
  `serdes_set_check_en_open` 都在 ko 里，说明有状态检查函数），先确认"PLL 到底锁没锁"，
  再谈配不配。

**不建议**在没有 A/B 结论前继续盲写寄存器——目前所有写入都在 SerDes/PON 时钟域内，
没造成损害，但再往下就要碰 `sys_ctrl`（pinmux，实测三次断网）。

## §68 SerDes 打通（2026-10-18 02:40–03:00）

**结论：PON SerDes 已在我们自己的 6.18 内核上跑通，`serdes_rx_ready` = 1，配置字与厂商
在用设备逐字节相同。** 已固化进内核驱动，开机自动执行。

### 一、§67 的三个误判，全部推翻
| §67 的判断 | 实际事实 |
|---|---|
| `pon_serdes_base = 0x10e30000` | **`0x10e40000`**。原厂 DTS `pon@10e00000` 的 7 段 reg 与
`zx_pon_probe` 的 7 次 `of_iomap` 一一对应：0→sys_ctrl(0x10e00000)、1→top_crm(0x10e10000)、
2→pin_mux(0x10e20000)、3→ncs_pll(0x10e30000)、**4→serdes(0x10e40000)**、5→pon_base(0x17000000)、
6→efuse(0x14f0d000) |
| `0x10e30000` 区只有 8 位可写 | 那是 **ncs_pll 的字节宽寄存器文件**（0x0c=0x99、0x10=0xce、0x14=0x07…），
本来就只有低字节；SerDes 区 `0x10e40010` 可整字读写 |
| PLL 未锁 | `0x10e10038 = 0x0828142a` 本来就是厂商序列的**终态**（bits[23:16]=0x28、
bits[15:6]=0x50、bit27=1），之前"写不进"是把 bit31 的**复位脉冲**误读成"写失败" |

### 二、决定性证据：原厂固件活着，并且能拿到 root
设备因写坏 ncs_pll 触发看门狗、重启进了原厂固件。原厂 telnet **`root/Zte521`** 可登录
（`admin/admin`、`root/root` 均拒绝），Linux 4.1.25，`gpondrv`/`zte_xgpon`/`plat_zxylzb_9131`
全部在跑。`/dev/mem` 可读 ⇒ 直接拿到一台**工作中**设备的寄存器现场。

黄金值（`0x10e40000`，厂商在用、RX ready）：
```
00 e009fef3   04 400019ff   08 00001dd0   0c 00003034
10 00000000   14 118f2000   18 00950231   1c 01a1e014
20 48000002   24 09570087   28 00080000   2c 00000000
30 01212000   34 00793d46   38 00000000   3c 00000000
40 003c0000   44 ea00a013   48 101038ca   4c 0005a088
50 33333333   54 33333333   58 03e23333   5c 00000000
```
状态：`0x68 = 0x598`（bit4 rx_ready、bit8 sig、bit10 pll）、`0x60 = 0x8000`、
`0x70 = 0x916596`；`0x17000018 = 3`（GPON）；`0x10e10040 = 0x1944`（ref_clk 0x140）；
`0x10e1004c = 0x01ffffff`；`0x10e100fc = 0x59`；`0x10e00018 = 0x7a`；`0x10e00020 = 0x9f`。

**关键推论**：厂商跑的是 `serdes_mode_set(mode 7)`（不是 3）：probe 里 port-info
`& 0xe40` 命中 → `work_mode = 3` → `ref_clk_set(7)` → `serdes_mode_set(7)` → 命中
`product_vid ∈ {0x9c,0xaa}` 分支再补写 `0x00=0xe009fef3`、`0x24=0x09570087`。
所以 `0x10e40000` 的最终值和 `reg_def_set` 的表**有 8 处不同**，必须以黄金值为准。

### 三、两个致命坑（以后不要再踩）
1. **`/dev/mem` 的 mmap 写在本内核是丢的**：`zxreg w` 读回"成功"其实是缓存行回读，
   寄存器没动。所有写必须走驱动的 `zx_poke`（`ioremap + writel`）。读可以用 mmap。
2. **顺序不能反**：必须先写 SerDes 24 字，再写 `0x58` PLL 字，最后打 `0x38` 的 bit31
   复位脉冲。先打脉冲再写配置 = 白打。

### 四、落地：`zx279131-pon.c`
新增 `drivers/net/ethernet/zte/zx279131-pon.c`（并入 `zx279131-y`），在 PP 驱动 probe 末尾调用：
1. `0x17000018 = 3`
2. `0x10e1004c`：清 bit24+bit8 → 20ms → 抬 bit24
3. `0x10e10040 = 0x1944`
4. 写 24 字 SerDes 表
5. `0x10e1004c = 0x01ffffff`、`0x10e100fc = 0x59`
6. `0x10e40058 = 0xb3e23333` → `0x10e10038` 置位/bit31 脉冲
7. `0x10e00010/18/20 = 0x2000 / 0x7a / 0x9f`

启动日志：`zx279131-pon: serdes status 0x00000598 (rx_ready=1 pll_lock=1)`。
sysfs：`/sys/bus/platform/devices/18000000.pp/pon_status`（status/work_mode/0x060/0x070），
`pon_reinit` 可运行时重跑。

### 五、用户态
- `pon-board-identity`：`serdes_reset`→RX ready、`serdes_pll`→locked、`serdes_cdr`→原始
  `0x10e40068`；`work_mode` 认 3=GPON（4 是 ROM 默认值，不是 PON 模式）。
- `ponctl` / `pondctl`：`optical_signal` 优先取 SerDes `rx_ready`，GPIO47 只作兜底 ——
  初始化后 RXSD 焊盘交给 SerDes，GPIO 恒读 0，单看 GPIO 会误报"无光"。
- 硬件页标签：`RX 就绪 / SerDes PLL / SerDes 状态字`。

### 六、还没做的（下一步）
- GPON MAC（`0x17000000`）+ TC 层（MPCP/PLOAM/OMCI）：SerDes 只解决物理层收，
  要注册上网还差 MAC 初始化与协议栈。黄金现场可直接用于比对 MAC 寄存器。
- **DDM 复核结论（03:10）**：A2 页从 0x60 整窗读是准的，之前 RX 恒 0 是短读错位造成的
  假象。当前实读 `TX +0.91 dBm / RX -20.81 dBm / 41.85℃ / 3.3125V`，-20.8 dBm 是正常
  GPON 下行电平 ⇒ **下行光功率链路是通的**。注意 `cat eeprom` 顺序读只回前 32 字节，
  必须 `hexdump -s 96`。
- GPON MAC + TC 层（MPCP/PLOAM/OMCI）仍未做。

## §69 GPON MAC 全景取证：配置已 100% 对齐，边界在协议栈（2026-10-18 03:20–04:30）

**结论：MAC 的硬件配置与厂商在跑设备没有任何实质差异，但未注册的 ONU 不会产生 PON
中断。继续往下的唯一路径是实现 PLOAM/MPCP 发现流程，不再是"抄寄存器"能解决的。**

### 一、把厂商的 MAC 全量搬回来做基线

原厂固件 telnet `root/Zte521` 起来后，用静态 `zxreg`（`/dev/mem` 只读）扫了 6 个窗口，
每份都存了档（`work/vendor_mac_win014.txt`、`vendor_mac_win36.txt`、`vendor_pinmux.txt`）。

MAC 寄存器空间不是一整块，而是 `0x17000000` 之上按 **0xc0000** 间隔切的 7 个窗口
（对应厂商 `xmac_vir_addr_map()`）：

| 窗口 | 基址 | 内容 |
|---|---|---|
| 0 | `0x17000000` | MAC 核心（ID、work mode、定时器）；`+0x400` 是第二个视图 |
| 3 | `0x170C0000` | GEM / TCONT / 队列 |
| 4 | `0x17180000` | DBA / TC 层 |
| 5 | `0x171C0000` | MPCP / TC 层 |
| 6 | `0x17200000` | 中断状态、GEM 队列、`+0x200A04` 状态字 |
| 7 | `0x17240000` | 复位块（`+0x04` = PON 复位） |

对照结果（共 1536 个寄存器）：

| 区 | 相同 | 不同 |
|---|---|---|
| `0x17000000`(256)+`+0x400`(256) | 508 | 5（`0x20`/`0x44`/`0x68` 及 `+0x400` 的同名者） |
| `0x170C0000` | 120 | 8 |
| `0x17180000` | 127 | 1（`0x10`） |
| `0x171C0000` | 255 | 1（`0x22c` = 随机数发生器，每次读都不同） |
| `0x17200000` | 256 | 0 |
| `0x17240000` | 256 | 0 |
| pin_mux `0x10e20000` | 254 | 2（`0x04` bit22/bit18、`0x0c` bit0） |

### 二、逐项排除

| 假设 | 实验 | 结果 |
|---|---|---|
| MAC 没配置 | 1536 个寄存器全量对比 | **几乎全同**，配置不是问题 |
| pin_mux 没接 PON | 写 `0x10e20004=0x0154dd75`、`0x10e2000c=0xfc` | 写入成功、**网络 0% 丢包**、SerDes 状态不变 |
| GEM/TCONT 没配 | 写 `0x170C0028/98/b8/d8/dc/e4/e8`、`0x17180010` | `0x98` bit28 只读；其余写入成功，**中断仍 0** |
| RX 使能位没开 | 试过 `0x17200880`（后证实映射推算错，已还原） | 无影响 |
| 中断没注册 | DTS 加 `pon@10e00000` + `interrupts = <0 0x25 4>`，驱动 `request_irq` | **注册成功**，`/proc/interrupts` 出现 `zx279131-pon`，但 `count=0` |

### 三、决定性事实

厂商设备 `/proc/interrupts`：

```
16:   120 ... GICv3  69 Level pon     → 15 秒后 162 → 再 20 秒后 197
```

约 **1.75 次/秒**，稳定增长。我们同一根线（GIC hwirq 69）注册了 handler，**从未触发**。

`xedpm_isr_handler()` 读的两个状态字（`0x17200A04`/`0x17200A08`）在双方读到的都是 0 —
它们是瞬时/读清状态，静态采样看不到内容，不能当判据。

### 四、为什么抄配置不够

GEM/TCONT 区那 8 个差异不是"使能开关"，是**服务实例**：厂商已注册（有 ONU-ID、TCONT、
GEM port、DBA 配置），我们没有任何一个是。MAC 侧对未注册 ONU 不上报事件，所以：

1. SerDes 只解决"光→电、时钟恢复"（`rx_ready=1` 已达成）
2. MAC 要上报帧，需要**至少一个已建立的 TC 服务**
3. 建立 TC 服务 = MPCP Discovery（Gate/Register/ACK）+ PLOAM + OMCI

这就是之前判断的人月级边界，现在有了实证：不是缺一个 bit，是缺整套状态机。

### 五、这一轮的真实产出

1. **PON 中断通路打通**（可复用）：`zx279131-f7610u.dts` 加 `pon@10e00000` 节点，
   `zx279131-pon.c` 里 `of_find_compatible_node` + `irq_of_parse_and_map` + `request_irq`。
   首个中断到来时会打印状态字并自动 `disable_irq_nosync`（避免 level 中断风暴）。
2. **两个观测节点**：
   - `.../18000000.pp/pon_irq` → `irq=17 count=N stat0=... stat1=...`
   - `.../18000000.pp/pon_irq_sample` → 40 次 ×50ms 被动采样状态字，不写任何寄存器
3. **全量基线快照**（`work/vendor_mac_*.txt`），以后做 MAC 任何改动都能 diff。
4. 纠正一条旧经验：`0x17000400` 起是 MAC 的第二个视图（同一组寄存器），不是独立设备。

### 六、下一步（真要做，按顺序）

1. **MPCP Discovery**：ONU 上电 → 收下游 Gate → 随机延迟 → Register_REQ → 收 Register → 
   分配 ONU-ID → 进入 O5。需要的寄存器在 `0x171C0000`（MPCP）与 `0x170C0000`（TCONT）。
2. **PLOAM**：下游 PLOAM 解析（`incoming_downstream_ploam_msg_controller`，厂商 gpondrv 有符号名）。
3. **OMCI**：慢协议，可以先 stub 掉，只做 PLOAM 层就能让 PON 灯进入"注册中"。
4. 建议：先只做 **MPCP Discovery + PLOAM 最小集**，用 `pon_irq` 计数验证，再谈 OMCI。

产物：`tftp/f7610u-v9.19-ponirq.itb`（30,117,996 B，md5 `1a50ff1d50cb8ea5ece32d191d86d290`）。

### ⚠ 事故记录（04:35）读未映射 MMIO 导致整机挂死

为验证 `reg(idx)` 的基址假设（`0x800220*4 → 0x19000880`），用 `zxreg d 0x19000880` 直接读了一个
**未映射**地址。结果是总线挂死：CPU 卡在访问上，网络、串口全无响应，看门狗未动作，
DTR/RTS 切换也无效 ⇒ 只能物理断电。

**教训（硬性）**：
1. **绝不猜地址去读**。`zxreg` 走 `/dev/mem` mmap，读到未映射区域不是返回错误，是挂总线。
   只读已知有效范围内的地址（`0x17000000`+0x240000、`0x10e00000`-`0x10e40000`、`0x10d10000` 等）。
2. 想验证地址是否存在，先用内核 `ioremap` + `readl`（有异常保护），不要用用户态 mmap。
3. `reg(idx)` 的 `0x8xxxxx` 系列基址仍未知，**不要再用算术推**。

### reg(idx) 映射的部分结论（可信）

`base = 0x17000000`，`idx` 是**字偏移**，这条已被三个独立事实确认：

| reg | 地址 | init 写入 | 实测 | 结论 |
|---|---|---|---|---|
| `6` | `0x17000018` | 6 | 3（后改） | ✓ work_mode |
| `9` | `0x17000024` | 0xc8 | 0xc8 | ✓ 完全吻合 |
| `0x7001d` | `0x171C0074` | 0x1b00300c | 0x1b3008 | 接近（低位被硬件改） |
| `0x70017` | `0x171C005C` | 0xc1900000 | 0x61900000 | bit31 被硬件清 |

所以 `0x7xxxx` 系列 = MPCP 窗口 `0x171C0000`，可直接算。`0x8xxxxx` 系列需要别的基址，
在查明之前**不要碰**。

## §70 深扫 16KB×3：MPCP 区与我们 100% 一致，配置层面已无路可走（2026-10-18 04:00–05:00）

### 一、又发现一层结构：每个窗口都是 16 个 0x400 的实例

扫 `0x17000000` 前 16 KB（4096 个寄存器）后，差异呈完美规律：**每 0x400 一组，共 16 组**，
每组差同样 3 个寄存器：

| 偏移 | vendor | ours |
|---|---|---|
| `+0x20` | `0x320` | `0x300` |
| `+0x44` | `0xffffff0f` | `0xffffffff` |
| `+0x68` | `0x61a80150` | `0x61a8015c` |

48 个差异 = 16 × 3。这说明 `0x17000000` 不是"一个 MAC"，而是 **16 个端口实例**，
每实例 1 KB。`0x170C0000`（GEM）同样是 16 个实例，每实例 7 处差异（`+0x28/98/b8/d8/dc/e4/e8`）。

**把 16 个实例全部对齐后，中断依然 0**（`0x170C0098` 的 bit28 是只读状态位，写不进去）。

### 二、决定性：MPCP 区 16 KB 只差 1 个寄存器

| 区 | 扫描量 | 差异 |
|---|---|---|
| `0x17000000` | 4096 | 48（16 实例 × 3） |
| `0x170C0000` GEM | 4096 | 112（16 实例 × 7） |
| `0x171C0000` MPCP | 4096 | **1** —— `0x171C022C`，随机数发生器（每读必变） |

**MPCP / TC 层的配置与我们逐寄存器相同。** 加上之前中断区、复位区都是 0 差异，
结论只能是：**硬件配置已经和厂商一模一样，MAC 依然不上报事件。**

### 三、`reg(idx)` 的 `0x8xxxxx` 系列仍然是谜

- `base=0x17000000` + `idx*4` 对 `0x7xxxx` 成立（`0x7001d→0x171C0074`、`0x70017→0x171C005C`
  实测值与 init 写入值形态一致）
- 但 `0x800220*4 → 0x19000880` **不可访问**，读了直接挂总线
- 试过"字节偏移"假设（`0x17070044`、`0x17080220`），全 0，不成立
- ⇒ `0x8xxxxx` 用另一套基址，指向 `0x19000000` 附近，厂商 ioremap 了而硬件/映射我们够不到

这意味着 `set_ponrcv` / `set_ponsend` / `xedpm_isr_handler` 这几个关键函数的真实地址
**无法从现有信息推出**。

### 四、这一轮的落地

**新增内核节点 `pon_reg`（重要）** —— 内核态寄存器读写，走 `ioremap` + `readl`，
带地址白名单（只允许 `0x17000000-0x1724FFFF`、`0x10e00000-0x10e100FF`、`0x10d10000-0x10d100FF`）：

```
echo 0x17000020        > .../18000000.pp/pon_reg   # 读
echo "0x17000020 0x320" > .../18000000.pp/pon_reg  # 写并回读
cat .../18000000.pp/pon_reg                        # → 0x17000020 = 0x00000320
```

白名单外的地址（如 `0x19000000`）直接 `-EINVAL`，**再也不会把总线读挂**。
实测通过。

**硬件页新增「PON 中断计数」**：`pon-board-identity read pon irq_count`，ubus 通路验证
`code:0, stdout "0\n"`。这是"MAC 是否真的在交货"的唯一 UI 指标。

产物 `tftp/f7610u-v9.20-ponreg.itb`（30,117,996 B，md5 `2991fb2e…`）。

### 五、到此为止的完整排除表

| 层面 | 状态 |
|---|---|
| 光模块 i2c / DDM | ✅ 通（TX +0.91 / RX −20.7 dBm） |
| SerDes 物理层 | ✅ 通（rx_ready / pll_lock / sig_detect 全 1） |
| pin_mux | ✅ 已对齐（写厂商值网络不丢包） |
| MAC 核心 16 实例 | ✅ 已对齐（无效） |
| GEM 16 实例 | ✅ 已对齐（无效） |
| MPCP / TC 16 KB | ✅ **本来就完全一致** |
| 中断区 / 复位区 | ✅ 完全一致 |
| GIC 中断注册 | ✅ 成功（`zx279131-pon`, hwirq 69），但 **count=0** |
| **厂商对照** | 同一根线 **1.75 次/秒** |

### 六、结论与下一步的正确姿势

**配置层面已经走到底了。** 剩下的未知只有三样，都需要新工具/新信息才能推进：

1. **`0x8xxxxx` 那套寄存器**（`set_ponrcv` = RX 使能、`set_ponsend` = TX 使能、中断状态字）——
   需要知道 `reg` 的第二个基址。可能的获取途径：厂商 `gpondrv.ko` 的 `.bss` 初始化代码、
   或原厂 `/proc/gpon_sdk/debug` 的完整 dump。
2. **MAC 的 DMA 描述符表** —— 收到 GTC 帧后要写内存，没有描述符可能直接丢帧且不报中断。
   `xemac_init` 里有 `pp_reg_write(0x301b,1)` / `(0x8000,9)`，说明与 PP 有耦合。
3. **GTC 帧同步（Psync 0xB6AB31E0）是否启用** —— 未同步则不会上报任何帧。

**建议**：这三项都是"逆向 + 实验"的量级，不是配置对齐能解决的。真要往下走，先做 1
（找 `0x8xxxxx` 基址），它是 RX 使能的总开关；找不到就不要在 2、3 上花时间。

### §70 补充：xemac2（GPON 驱动）反汇编，基址链终于闭环一半

之前一直看 `zte_xemac1`（EP 层），这是错的。**GPON 的 MAC 驱动是 `zte_xemac2.ko`**，
`gpondrv` 调的是它的 `zte_api_gp_*` 封装。

`zte_xemac2` 的寄存器访问是**五种独立基址**，存在一个全局结构的固定偏移里：

| 函数族 | 结构偏移 | 用途 |
|---|---|---|
| `fpga_pon_*` | **+8** | PON 全局寄存器 |
| `fpga_gmac_*` | +16 | GPON MAC |
| `fpga_emac_*` | +32 | EPON MAC |
| `fpga_xedmac_*` | +40 | XE（10G）MAC |

**`+8` 就是 `0x17000000`，已用三个独立值闭环确认**（`xemac_init` 里
`pongreg_write_reg(idx, val)` ⇒ `*(+8) + idx*4`）：

| 调用 | 目标 | 实测 |
|---|---|---|
| `pongreg_write_reg(8, 0x300)` | `0x17000020` | `0x300` ✅ 完全吻合 |
| `pongreg_write_reg(9, 0xc8)` | `0x17000024` | `0xc8` ✅ 完全吻合 |
| `pongreg_write_reg(0x1a, 0x61a88158)` | `0x17000068` | `0x61a8015c` ✅ 形态一致 |

`xemac_init`（GPON）的完整序列开头：

```
pongreg_write_reg(6, 4)              0x17000018 = 4   (ROM 默认 work mode)
zx_pon_clk_reset_init(2)             ← 注意参数是 2
pongreg_write_reg(9, 0xc8)           0x17000024 = 0xc8
pongreg_write_reg(0x1a, 0x61a88158)  0x17000068
pongreg_write_reg(0x1b, 0xbd8)       0x1700006c
pongreg_write_reg(8, 0x300)          0x17000020 = 0x300
eponmac_write_reg(0x13, 1) …         一串 EPON 侧配置
```

**注意 `zx_pon_clk_reset_init(2)`**：厂商 GPON 路径传 **2**，而我们从 plat 模块的
`zx_pon_probe` 里推出来的是 **3**。两者可能对应不同 port type 分支（2 = GPON？3 = ？），
需要单独核对 —— 这是目前唯一发现的、我们的初始化参数与厂商 GPON 路径不一致的地方。

**但 `zp_api_gp_set_ponrcv`（GPON 的 RX 使能）仍然找不到实现**：`gpondrv` 里是 UND 符号，
11 个 ko 里没有任何一个导出它，`tm.ko` 的 `__ksymtab_strings` 里也没有。
它很可能编在内核里（`work/k1_vmlinux.bin`），而裸 vmlinux 没有符号表，反汇编无从下手。

⇒ **`0x8xxxxx` / GPON RX 使能这条路，靠现有 ko 集合已经走到尽头。**

---

## §71 突破：从原厂活体拿到全量符号表，GPON RX 打通（2026-10-18 05:15）

上一节说"靠现有 ko 集合走到尽头"—— 结论作废。换了个思路：**不拆 ko，直接问运行中的原厂内核**。

### 71.1 打开符号表的三步

1. **mtd7/mtd8（kernel1/2）是加密的**：读出来既没有 ARM64 magic(`ARM\x64`)，也没有
   `Linux version` / `kallsyms` 字符串。离线提取符号这条路死了。
2. **原厂 `kptr_restrict=0`，telnet root 能直接 `cat /proc/kallsyms`**。
   4.6 MB，解析出 **119 525 条符号**：
   - `vmlinux` 51 434 条，基址 `ffffffc000080000`（无 KASLR）
   - 模块符号带 `[modname]` 后缀，**每个模块都有完整内部符号**
3. 关键发现：GPON MAC 驱动叫 **`zte_xgpon`**（227 KB，1948 个符号），不是我们一直以为的
   `zte_xemac2`。它在 `/kmodule/zte_xgpon.ko`，用 tftp -p 拉回来（302 928 B，
   md5 `6d9c66f28de66acfbe13f06e1eea8c18`）。

有了符号表，`zte_api_gp_set_ponrcv` 立刻定位：`ffffffbffc30b5f8`，在 `zte_xgpon` 里。

### 71.2 寄存器寻址的真实语义（重要）

```
extern u32 *fpga_pon_base;          // zx_ponreg.ko 的 .bss
fpga_read_reg(idx) = fpga_pon_base[idx]        // 注意是 **字下标**，不是字节偏移
```

所以 `Hal_*XmacReg(idx, v)` 的物理地址 = `0x17000000 + idx*4`，**跨窗口也是连续的**：

| idx | 物理地址 | idx | 物理地址 |
|---|---|---|---|
| 0x6 | 0x17000018 | 0x16003 | 0x1705800c |
| 0x1a | 0x17000068 | 0x16004 | 0x17058010 |
| 0x3000a | 0x170c0028 | 0x16100 | 0x17058400 |
| 0x500c0 | 0x17140300 | 0x16600 | 0x17059800 |

`Hal_ReadGmacReg` 是**另一个基址**（EPON/独立口），GPON 走不到，忽略。

### 71.3 RX 使能 = 0x17058400 bit0

```
zte_api_gp_set_ponrcv(v) -> fi_Configure_Ponmac_Set_Downstream_Enable(v)
    v=1 -> 0x16100 |= 0x01        v=0 -> 0x16100 &= ~0x01
```

`fi_Configure_Ponmac_Set_Gpon_Enable(1)` = `Upstream_Enable(1)` + `Downstream_Enable.part.0`，
即同时置 `0x17059800` 的 bit1/2 与 `0x17058400` bit0。

### 71.4 寄存器整体对比：只差 28 个

把 166 个 `fi_*` / `zte_api_*` 函数里 HAL 访问过的寄存器去重，得到 **86 个**
（`work/stock_dump/regmap.json`），在原厂活体逐个读出来（`work/vendor_regs.json`），
再和我们镜像逐个读、对比：

- 76 个完全一致（U-Boot/ROM 已经把 MAC 配得七七八八）
- **28 个不一致**，其中关键几条：

| 地址 | 原厂 | 我们 | 含义 |
|---|---|---|---|
| `0x17058400` | `0x63b` | `0x61a` | bit0=RX 使能、bit5 |
| `0x1705800c` | `0x9be` | `0x0` | 中断事件使能 |
| `0x17059800` | `0x16` | `0x05` | 上行 |
| `0x17059428` | `0xdd` | `0x0` | GPON profile 字段 |
| `0x17140100` | `0x1` | `0x0` | 中断模块使能 |
| `0x17140300` | `0x1` | `0x0` | 中断模块使能 |

把这几条写进去之后，**GIC 69 号中断（irq 17）第一次跳了起来** —— 之前一直是 0。

### 71.5 中断语义（xedpm 那对是错的）

`fi_Interrupt_PonMAC_Handle`：

```
en = read(0x16003)      -> 0x1705800c
st = read(0x16004)      -> 0x17058010
ev = en & st
```

bit 含义（逐个分支对照）：
- **bit2 = PLOAM 消息到达** ← 最重要的一个
- bit3/4/5/6/7 = 各类 TC/MPCP 事件
- bit8 = 上游相关

厂商 ISR 读的那对 `0x800281/0x800282`（我们早先当成窗口 4 的 `0x17200a04/0a08`）
在 GPON 路径上根本用不到，原厂活体读数恒为 0。**我们之前的 ISR 读错寄存器了。**

### 71.6 bit2 的清除方式（踩坑）

`fi_Configure_Interrupt_Read_Ploam_Msg`：

```
轮询 0x16147 (0x1705851c) bit0，最多 16 次
从 0x16148 (0x17058520) 连读 16 个字，每字 rev（字节翻转）  -> 64 字节 PLOAM 消息
写 0x16147 = 0                     <-- 这一步才真正让中断线落下
```

**教训：只写 `0x16004 = 4` 是清不掉的。** v9.21 只做了这件事，结果电平永远拉低，
实测 **67 万次/秒** 的中断风暴，整机 livelock —— 网口、串口控制台全部饿死，
`disable_irq_nosync` 写在 sysfs 里根本没机会执行。**只能断电。**

⇒ v9.22 的修正：
1. bit2 就走上面那套 drain（读 16 字 + 写 0 到 `0x1705851c`）
2. **风暴保护放进 ISR 本体**：同一个 jiffy 内超过 200 次直接 `disable_irq_nosync` 并打印
3. 加 `pon_ploam` sysfs 节点，显示最近一条 64 字节 PLOAM

### 71.7 由此确认的事实

中断能跳出 bit2（PLOAM），说明 **SerDes → GPON MAC 的下行接收链路是真的通了**，
OLT 正在持续广播 PLOAM。之前"配置层已经走到尽头"的判断是错的：
差的就是这 28 个寄存器 + 正确的 ISR。

### 71.8 环境/脚本（这次新增，都在 work/）

| 文件 | 作用 |
|---|---|
| `ub_boot.py` | **单次 open 串口**抢 U-Boot。bash 里 `printf > /dev/ttyUSB0` 循环会反复 open/close，USB 串口跟着复位，日志全空 —— 这个坑踩过 |
| `uboot_load.py` | 已在提示符后：setenv → tftp → pon_reset 脉冲 → bootm，同样单次 open |
| `grab_uboot.sh` | 旧版（多进程，已废弃，留作反面教材） |
| `tn_batch.py` | telnet 批量跑命令，输出落文件 |
| `hal_extract.py` | **objdump 反汇编 → 寄存器读改写序列**。`hal_extract.py x.dis <函数名...>` |
| `stock_dump/` | 原厂 kallsyms/iomem/modules/dmesg + regmap.json |
| `vendor_regs.json` | 原厂 86 个寄存器的活体值 |

⚠ **原厂 secure U-Boot 的提示符**：它会打印 `Hit any key to stop autoboot: 3`，
但 `bootdelay=3` 的窗口很短，必须从复位那一刻就持续喂回车（30 ms 间隔）。
从原厂 telnet 发 `reboot -f`，然后立刻起 `ub_boot.py keep`。

### 71.9 下一步（设备通电后）

1. 断电重启 → 抓 U-Boot → `uboot_load.py f7610u-v9.22-ploam.itb`
2. 看 `cat pon_irq` / `pon_status`：确认 interrupt 频率正常（不应是风暴）
3. `cat pon_ploam` 把 OLT 的 PLOAM 抓出来 —— 里面就有 ONU-ID / 授权 / 测距请求
4. 有 PLOAM 之后才能写 MPCP Discovery（Registration Request）→ 拿 ONU-ID

---

## §72 上行发送通路（v9.24，2026-10-18 05:40）

### 72.1 发送寄存器

`fi_Configure_Ponmac_Send_Ploam_Msg`：

```
轮询 0x16642 (0x17059908) 直到 0        <- FIFO 空
0x66c2..0x66cd (0x17059b08..) 写 12 个字，每字 rev
0x16642 = 1                             <- 触发，这一下真发
再轮询回 0
```

`0x17059908` 实测开机即 0（空），通路可用。

### 72.2 报文格式（不是 13 字节 PLOAM）

`Drv_PloamuMic_Generate_Activation` 揭示：**48 字节 = 40 字节正文 + 8 字节 AES-CMAC**，
CMAC 对 `0x02 + 正文前 40 字节` 计算，结果写回 `+40`。

`send_registration_msg` 构造的正文：

```
[0..1]  16 位字段（高字节在前）
[2]     MsgID = 2   <- 注册
[3]     参数
[4..35] 32 字节来自配置块（SN / 密码）
[36..39] 4 字节
```

对照收到的下行报文 `03 ff 01 xx ...`，可推断 `[1]` = ONU-ID（`0xff` 广播）、`[2]` = MsgID。

### 72.3 ONU 的 SN（从 mtd12 配置块里挖出来）

`mtd12-plugin_data.bin` 偏移 `0x2639cf6` 附近是一个 TLV 配置块：

| 字段 | 值 |
|---|---|
| Vendor | ZTE |
| MAC | D8:E8:44:5C:0A:5D |
| ProductClass | F7610U-C |
| MainChip | ZXIC_ZX27913x |
| **GPONSN** | **ZTEGD09D561** |

串是 11 字符，缺前导 0 ⇒ 线上 8 字节 SN 应为 **`ZTEG` + `0x0D09D561`**，
即显示成 `ZTEG0D09D561`。

### 72.4 v9.24 做了什么

新增三个 sysfs 节点，**排队与触发分离**：

| 节点 | 作用 |
|---|---|
| `pon_send` (WO) | 写 96 个 hex 字符 = 把 48 字节压进硬件 FIFO，**不发送** |
| `pon_fire` (WO) | 写 1 = 触发发送 |
| `pon_send_state` (RO) | queued/sent/FIFO 状态 + 当前报文 |

这么拆是因为**上行是时分共享介质**，乱发会撞同 PON 口其他 ONU 的时隙。

### 72.5 还没做（blocking）

1. **MIC**：注册报文带 AES-CMAC，密钥来自激活流程；不带正确 MIC 的报文 OLT 会丢
2. **上行突发时序**：激光开启/关闭、burst profile、EqD 测距
3. **BWmap 解析**：不知道 OLT 给的授权窗口，就没有合法的发送时机

---

## §73 原厂 O5 活体取金（2026-10-18 05:47，用户回原厂填真注册码上线）

思路：与其猜 MPCP，不如让**合法原厂设备上到 O5**，把稳态寄存器与报文全抄下来当基准。
用户配合填了真注册码，原厂 telnet `root/Zte521` 进去取数。整个过程 **只读，不写任何寄存器**。

### 73.1 全量寄存器快照（6144 个）

`work/o5_regs.json`，覆盖：

| 区间 | 大小 | 内容 |
|---|---|---|
| `0x17000000`-`0x17004000` | 4 KB | 全局 / EP 层 |
| `0x17058000`-`0x1705a000` | 8 KB | **XMAC 主控 + PLOAM FIFO + TX FIFO + 加密** |
| `0x170c0000`-`0x170c4000` | 4 KB | GEM / TCONT |
| `0x17140000` `0x17180000` `0x171c0000` `0x17200000` | 各 2 KB | 中断聚合 / DBA / MPCP / 中断屏蔽 |

对比自研 v9.20 的 86 个点（77 个共同项）：**44 个不同**。

### 73.2 ★★ 抓到 OLT 下行报文全文（48 字节）

`0x17058520` 起是**只读数据窗口**（读不消耗、不清标志），可以安全反复读：

```
03 ff 01 92 | 03 01 04 a5 | 66 79 e0 00 | 00 00 00 08
1f aa aa aa | aa aa aa aa | aa 48 57 54 | 43 20 50 4f
4e 00 00 00 | 00 00 00 00 | 96 b5 fd a0 | fa 2b 33 1b
00 00 00 00 | 00 00 00 00 | 00 00 02 85 | 00 00 00 00
```

2 秒后再读，**只有两处变化**：
- `byte3`: `0x92` → `0x96`（2 秒 +4 ⇒ **0.5 秒一条**，即每 4 个 GPON 帧一条）
- `byte40..47`: `96b5fda0 fa2b331b` → `1be36f8f 7273407d`

⇒ **硬确认：报文 = 40 字节正文 + 8 字节 AES-CMAC（MIC）**，与
`Drv_PloamuMic_Generate_Activation` 反汇编结论一致。byte3 是序号，在正文内，MIC 随之重算。

配套寄存器：
- `0x17058514` = 接收计数（2 秒 +4，与报文序号同步）
- `0x1705851c` bit0 = "有新报文" 有效位，原厂读完即清 0
- 正文中固定段 `48 57 54 43 20 50 4f 4e` = ASCII **"HWTC PON"** ⇒ OLT 是华为

### 73.3 ★★ AES 密钥的真实存放位置（并印证 OMCC 0x86）

`0x17058c80` 起两个**完全对称**的 16+4 字节块：

```
0x17058c80  ce072d2f  0x17058c94  ce072d2f   <-- 同一把 AES-128
0x17058c84  6a5c1988  0x17058c98  6a5c1988
0x17058c88  70ba23f8  0x17058c9c  70ba23f8
0x17058c8c  9dd85628  0x17058ca0  9dd85628
0x17058c90  80000000  0x17058ca4  80000000   <-- bit31 = 通道使能
```

紧跟其后：

```
0x17058cb8 = 0x00860086     <-- 两个 0x86 字段
0x17058cbc = 0x00003578
```

**`0x86` 正是华为私有 OMCC 模式**（文哥告知：LuCI 兼容性里有 0x86 / 0xb0 两档，
0x86 才能走华为私有 OMCI）。硬件在这里得到印证 —— 这很可能就是 OMCC 类型字段，
非华为 OLT 应配 `0x00b000b0`。**这是静态配置，可以直接照抄。**

另有一组疑似第二把密钥：`0x170598f8 = 0x66e17d0e` / `0x170598fc = 0x35d7c55f`，
以及 `0x170598d8 = 0x534b5957`（ASCII "SKYW"）。

⚠ 密钥是**会话级**的，抄不走也没用（我们自己注册时会重新协商）；有价值的是
**地址、结构、使能位**。

### 73.4 上行 / burst profile 关键差异（O5 vs 自研 v9.20）

| 地址 | O5 原厂 | 自研 v9.20 | 含义 |
|---|---|---|---|
| `0x17059910` | `0x00001b01` | `0x00000f02` | **burst profile**，差异最大 |
| `0x17059800` | `0x00000016` | `0x00000005` | 上行使能 |
| `0x17059400` | `0x00090005` | `0x03ff0000` | Alloc-ID / GEM 映射 |
| `0x1705947c` | `0x03ff0000` | `0x43ff0000` | |
| `0x17059480` | `0x03fe03fd` | `0` | alloc-ID 递减表 |
| `0x1705943c` | `0x00004009` | `0` | alloc 9 / TCONT 类型 |
| `0x17058414` | `0x00000002` | `0x00000010` | |
| `0x1705842c` | `0x00080000` | `0` | |
| `0x17058430` | `0x000007ff` | `0` | |
| `0x17059008` | `0x00000008` | `0x00000040` | |
| `0x1705800c` | `0x000009be` | `0` | 中断使能（已验证）|

### 73.5 MPCP 区（0x171c0000）候选值

| 地址 | 值 | 猜测 |
|---|---|---|
| `0x171c0054` | `0x0000d864` = 55416 | **EqD**，按 ns 计 = 55.4 us ⇒ RTT 110 us ⇒ 约 11 km（合理） |
| `0x171c0044` | `0x3d` = 61 | ONU-ID 候选 |
| `0x171c0048` | `0x3b` = 59 | 备用 |
| `0x171c0004` | `0x6` | |
| `0x171c0158` | `0x07735939` ≈ 125000000 | 125 MHz 时基 |
| `0x171c0190` | `0x03b9aca0` = 62 500 000 | 62.5 MHz |
| `0x171c0194` | `0x2540be40` = 625 000 000 | |
| `0x171c0100/114` | `0x0180c200` | MPCP 组播 MAC 01:80:c2:00 |

### 73.6 TX FIFO 残影（价值低）

`0x17059b08 = 0x000909e9`、`0x17059b30 = 0x4b22ba17`、`0x17059b34 = 0x9ecf226f`，
中间全 0，多次读**完全不变** ⇒ 是最后一次发送的静态残影，不是活动报文。
要拿原厂上行报文原文，只能靠"注册瞬间高频轮询"，成本高、收益低，暂缓。

### 73.7 结论

A 路线（BWmap + burst 时序）需要的输入，这一轮**基本齐了**：
- burst profile 具体值：`0x17059910 = 0x1b01` 等 44 项差异
- 下行报文格式：40+8，已确证
- 加密通道结构与 OMCC 0x86 配置：已定位
- 上行时隙：OLT 授权由硬件自动写入 DBA 区 `0x17180000`，可实时读

⇒ 下一步回自研固件，把这批值固化进 `zx279131-pon.c`，再实现 BWmap 读取与上行使能。

---

## §74 v9.25 + 精确 diff 结论（2026-10-18 06:10）

### 74.1 推翻 §73.5 的猜测

在自研 v9.24 上抓同一批寄存器，**值与原厂 O5 完全相同且完全静止**（间隔 2 秒连读三次无变化）：

| 地址 | 原厂 O5 | 自研未注册 | 判定 |
|---|---|---|---|
| `0x171c0054` | `0x0000d864` | `0x0000d864` | **不是 EqD**，是静态配置 |
| `0x171c0044` | `0x0000003d` | `0x0000003d` | **不是 ONU-ID** |
| `0x17180000` 区 | 133 个非零 | **一模一样** | DBA 区是静态配置，不是实时授权 |

⇒ **DBA / MPCP 两区两边已完全一致**，§73.5 的猜测作废。
整片 6144 个寄存器里，`0x1718` 只差 2 个、`0x171c` 只差 1 个。

### 74.2 两张 256 项表：写入机制已破解

原厂 O5：`0x17058040` 区首项 `0x04444444`、其余全 `0x09999999`；我们全是 `0x4bef`。
在自研固件上单点实验：

```
写 0x17058040 = 0x00004bef  0x17058044 = 0x00004bef  ...  0x170583fc = 0x00004bef
写 0x17058044 = 0x09999999
再读 0x17058044 / 0x17058048 / 0x170583fc  ->  全部 = 0x09999999
写 0x17058040 = 0x04444444
再读 0x17058040 = 0x04444444   0x17058044 = 0x09999999（未被覆盖）
```

⇒ **写一个地址会铺开 255 个寄存器**，但首项是独立控制寄存器。
这解释了厂商 `fi_Configure_Hw_Init` 只写 `0x16010=0x4444`、`0x16011=0x9999`
两个地址却读出整片的现象。第一张表已在 v9.25 上补齐，与原厂完全一致。

第二张表 `0x17059200` 区（原厂全 9、我们全 1）**故意不填**：
`9` 很可能是 OLT 分配给已注册 ONU 的 alloc-ID，未注册时保持默认才正确。

填表后中断频率无变化（0.66 次/秒，填表前 0.72 次/秒），符合预期 ——
这两张表影响 GEM 业务转发，而要先注册才有业务。

### 74.3 ★★ 找到 O5 判据：下行状态广播帧

48 字节下行帧的内部结构（8 条样本归纳）：

```
off 0-2   03 ff 01                 恒定
off 3     序号，0.5 秒 +1
off 4     序号 mod 4  (00/01/02/03)
off 5     序号 mod 2  (00/01)
off 8-11  4c c3 0f 00 / 66 79 e0 00   随 off4/off5 交替
off 15    ★ 注册状态
off 16    ★ 授权规模
off 18-28 aa x 11
off 29-36 "HWTC PON"
off 40-47 AES-CMAC
```

**与原厂 O5 同位置对比：**

| off | 原厂 O5 | 自研未注册 |
|---|---|---|
| 15 | `0x08` | `0x00` |
| 16 | `0x1f` | `0x04` / `0x08` |

⇒ **`off 15` 极可能就是「已注册 / O5」标志**，`off 16`（0x1f=31）是授权规模：
未注册只有 0x04/0x08，已注册拿到 31 个时隙。

这个字段比任何 MPCP 寄存器都可靠 —— **它是 OLT 直接告诉我们的状态**。
下一步把它做进 `pon_status`，作为注册是否成功的实时判据。

### 74.4 修正与新增

- `0x17059800`：mask 由 `0x16` 改成 `0x1f`，**强制清 bit0**（原厂 O5 实测 `0x16`，我们此前是 `0x17`）
- 新增 `pon_allocmap`（WO，写 1 批量填两张表），**默认不自动执行**
- 产物：`tftp/f7610u-v9.25-o5ref.itb`（30 117 996 B，md5 `9e7b55dfdd1be3c3b7f8c5afe67274d7`）
- v9.25 实测：irq 41/50 s ≈ 0.8 次/秒，`storm=0`，`0x17059800 = 0x16` ✓

### 74.5 下一步

配置层面对齐度已经很高（DBA/MPCP 完全一致，XMAC 只差"注册后才有"的状态）。
**剩下的唯一通路是真正发注册帧**，需要：

1. 报文构造（40 正文 + 8 MIC）—— MIC 密钥来源未知，初始可能全 0
2. discovery window 时机 —— 上面那个 48 字节帧里可能就藏着 grant 信息（off 8-11、off 16）
3. 上行突发使能时序

⚠ 发上行是**共享介质**操作，会占用同 PON 口其他 ONU 的时隙，需用户明确同意后再做。

---

## §75 v9.26-v9.28：字节序 bug + **推翻「O5 判据」**（2026-10-18 06:15）

### 75.1 字节取值方向：不是统一 >>24

`pon_ploam_drain()` 里 `w[i] = swab32(readl(...))`，w 是 `u32 *`。在小端 aarch64 上：

```
m[4n+0] = w[n] 的 **最低** 字节   (帧内偏移 4n)
m[4n+3] = w[n] 的 **最高** 字节   (帧内偏移 4n+3)
```

所以"取帧内第 k 字节"的方向取决于 `k % 4`，**不能一律 `>> 24`**：

| 帧内偏移 | 正确写法 |
|---|---|
| off 3  | `(w[0] >> 24) & 0xff` |
| off 15 | `(w[3] >> 24) & 0xff` |
| **off 16** | **`w[4] & 0xff`** ← 曾误写成 `>> 24` |

误写导致 off 16 读出的是 off 19（恒为 `0xaa`），一度看起来像"grant=170"。

### 75.2 ★★ 推翻 §74.3 的「O5 判据」结论

实测 8 条连续帧（seq 0x1a..0x21）：

```
off4(=seq mod 4)   off15   off16
   0,1             04     09
   2,3             08     1f        <- 每 4 帧一组，周期性
```

**我们未注册状态下，同样会出现 `off15=0x08 / off16=0x1f`**，
与原厂 O5 那条样本（`03ff0192 030104a5 ... 00000008 1faaaa`，off4=3）取值**完全一致**。

⇒ **off15 / off16 只是随帧序号周期性变化的字段，与注册状态无关。**
§74.3 "找到 O5 判据"的结论**作废**，向用户报告过的那条判断是错的。

LuaCI 上原本按"已注册(O5)/未注册(O1)"解读的标签已改成中性描述
`OLT 状态字节 15` / `OLT 状态字节 16`，避免误导。

### 75.3 环形缓冲显示顺序

`pon_ploam_all_show()` 里 `k = (pon_ploam_ring_idx + i) % RING`，
而 `pon_ploam_ring_idx` 在 drain 后已自增，指向**下一个待写槽位**（即最旧）。
所以 **`#0` 是最旧、`#7` 是最新**，字段解读不受影响，但阅读时注意。

### 75.4 其他

- `grant` 输出改十六进制（`%#04x`），与 `olt_reg` 及 shell 侧 `0x` 前缀一致
- `olt_byte()` 用 `echo "0x${v#0x}"`；原先 `printf 0x%sn` 经 python 写入时
  `\n` 被转成真实换行，把 printf 拆成两行 → 设备上 `08: not found`
- 版本：v9.26(v9.25+OLT字段) / v9.27(字节序修正) / **v9.28(hex 输出+脚本修复)**，均在跑
- v9.28 实测：`irq=17 count=13`、`ploam=12`、`olt_reg=0x08`、`grant=0x1f`、`storm=0`

### 75.5 结论

配置层面对齐度已经很高，但**"怎么判断注册成功"这个判据目前并不存在** ——
§74.3 那条是误判。剩下的唯一通路仍然是**真正发注册帧**，
并且成功与否只能靠 OLT 侧的反应（PLOAM 回包、DBA 授权变化、GEM 收包）来间接判断。

---

## §76 注册帧逆向定论：SN 恢复 + 硬件发送路径（2026-10-18 18:40–19:20）

> 今天用户把光纤插上（原插别处供网），设备曾断电回原厂；串口重灌 v9.28 后光层全通
> （serdes 0x598、ploam=112+、storm=0、olt_reg=0x08/grant=0x1f 未注册特征）。
> 原厂侧体检：telnet root/Zte521，pon RX 5.3MB/24433 包、omci 299 包 ⇒ 光纤+OLT 链路正常。

### 76.1 全量反汇编落盘

`zte_xgpon.ko` 全量反汇编 → `work/zte_xgpon.dis`（19772 行）。
工具：`immortalwrt/staging_dir/toolchain-aarch64_cortex-a53_gcc-14.3.0_musl/bin/aarch64-openwrt-linux-objdump -d tftp/zte_xgpon.ko`。
符号表 `work/stock_dump/kallsyms.json`（zte_xgpon 1948 条）。

### 76.2 ★ SN 恢复（首发帧的前提）

- `fi_Configure_Ponmac_Set_Sn(x0=sn8B)`：`Hal_WriteXmacReg(0x16636, rev(sn_lo))` +
  `Hal_WriteXmacReg(0x16637, rev(sn_hi))`，并缓存 8B 到 `XgponDataBase+0x261d`。
  `0x16636/0x16637` → 物理 **`0x170598d8 / 0x170598dc`**。
- O5 活体 `work/o5_regs.json`：`0x170598d8 = 0x534b5957`、`0x170598dc = 0xa8d40ba7`
  ⇒ 反推 SN（存储序）= **`53 4b 59 57 a8 d4 0b a7`**，前 4 字节 ASCII **"SKYW"**。
- **EEPROM 里没有 SN**：A0(0-0050) 只有 `ZTE_XEPON_AS`（SFP 厂商名区），序列号区(0x44)全零；
  A2(0-0051) 是 DDM 校准数据。SN 只在原厂 flash/配置里，但活体寄存器里有黄金值。
- 我们固件这两个寄存器原为 0，已用 zx_poke 写入并回读验证（**易失，重启必须重写**）。

### 76.3 ★ 注册分两步，首发帧走硬件路径

**第一步 Serial Number（首发帧）＝硬件路径**，不是 pon_send/pon_fire：

1. `Set_Sn`：SN 写入 `0x170598d8/dc`（见 76.2）。
2. `power_p2_lsi`（0xf788，O2 入口）→ `fi_Configure_Ponmac_Send_Sr_Ploam_Msg(2)`：
   RMW `0x1705999c`：poll bit3=0 → bits[2:1]=onu_id(=2) → bit3=1 触发 → poll bit3 清 0。
   报文由硬件用 SN 寄存器自动构造，**不需要软件拼帧**。
   onu_id=2 与 O5 活体 `0x1705999c=0x4`（bits[2:1]=2）互相印证。

**第二步注册应答＝软件路径**（OLT 发 Password 之后）：
`send_registration_msg`（0xdc68）拼 48B：byte0-1=u16@`XgponDataBase+0x264b`、
**byte2=MsgID 0x02**、byte3=onu_id、byte4-39=RegID 36B@`XgponDataBase+0x2625`、
byte40-47=MIC（`Drv_PloamuMic_Generate_Activation`，key@`+0x2691`），
再走 `fi_Configure_Ponmac_Send_Ploam_Msg`（= 我们驱动的 pon_send/pon_fire：
poll `0x17059908` → 12 字 rev 写 `0x17059b08` → `0x17059908=1`）。
⇒ **pon_send/pon_fire 不是首发帧，别拿它发 Serial Number。**

### 76.4 其他注册相关符号/结构

- `fi_Configure_SN_AllocId_Set(0..3)`（0x6960）：RMW `0x1705947c/80` 的 bits[13:0] 和 bit14；
  `fi_reset_Register` 依次调 (0)(1)。我们 `0x17059480=0x03fe03fd` 与 O5 一致，
  `0x1705947c=0x43ff0000`（O5=0x3ff0000，差 bit22）——首发前未处理，见 76.6。
- `fi_Configure_Ponmac_Set_RegID`：36B RegID 存 `XgponDataBase+0x2625` 并刷 9 个 xmac 寄存器。
- 接收侧 `fi_Configure_Interrupt_Read_Ploam_Msg`（0xad90）：poll `0x1705851c` bit0 →
  读 12 字 `0x17058520`（每字 rev）→ 写 0 清标志，与我们驱动一致。
- `XgponDataBase` 在 .bss（size 0x279c），SN 缓存 +0x261d、RegID +0x2625、
  u16 +0x264b、MIC key +0x2691、flag +0x272b。

### 76.5 当前武装状态（2026-10-18 19:15，设备 192.168.1.1 跑 v9.28）

| 寄存器 | 值 | 说明 |
|---|---|---|
| `0x170598d8` | `0x534b5957` | SN lo，已写 |
| `0x170598dc` | `0xa8d40ba7` | SN hi，已写 |
| `0x1705999c` | `0x0` | 未触发（**开火即写 `0xc`**：bits[2:1]=2 + bit3=1） |
| `0x17059908` | `0x0` | TX FIFO 空闲 |
| `0x17059480` | `0x03fe03fd` | 与 O5 一致 |
| `0x1705947c` | `0x43ff0000` | O5=0x3ff0000，差 bit22，未处理 |

开火命令：`echo "0x1705999c 0xc" > /sys/bus/platform/devices/18000000.pp/zx_poke`。
开火后观测：`0x17058514` 跳增 / `pon_ploam_all` 新内容 / `0x1705999c` bit3 自清 /
`dmesg | grep "irq 17"` / alloc 类寄存器变化。

### 76.6 未决与下一步

1. **开火许可**：用户 19:15 叫停做上下文记录，尚未许可发射（共享介质纪律）。
2. 若首发无反应：补 `SN_AllocId_Set` 序列（RMW `0x1705947c/80`）再试；排查 `0x1705947c` bit22。
3. 若 OLT 回 Password：走第二步软件路径（`pon_send` 已就绪，MsgID=0x02，需 RegID——来源待定，
   原厂由 gpondrv 经 `zte_api_gp_set_sn`/Set_RegID 下发）。
4. 注册成功判据仍不存在，只能看 OLT 侧反应。

---

## §77 真开火：SN 发送打通（硬件侧），OLT 仍无回应（2026-10-18 19:30–21:05）

> 本节推翻 §76 的两处错误：**SN 值** 与 **开火寄存器地址**。以本节为准。

### 77.0 重大勘误（§76 错在哪）

1. **SN 值错了**：§76 用 O5 寄存器 `0x170598d8/dc` 反推成 `"SKYW"+a8d40ba7`，
   实际真 SN 早在 §72.3 就从 mtd12 配置 TLV 挖出：**`ZTEG` + `0x0D09D561`**。
   O5 那两个寄存器当时装的不是 SN（可能是随机/临时值）。
   - 正确寄存器值：`0x170598d8 = 0x4745445A`、`0x170598dc = 0x0D09D561`（rev 存储）。
2. **开火地址错了 8 字节**：`Send_Sr` 用的是索引 `0x1666f` → 物理 **`0x170599bc`**，
   §76 误写成 `0x1705999c`（该地址写入不生效、读恒 0）。
   - `0x170599bc`：bit0 = SN 发送使能（`fi_power_init` 置/清），bits[2:1] = onu_id(=2)，
     **bit3 = 触发**（发送完成后硬件自清）。

### 77.1 ★ 硬件拒发的真因：MAC 必须处于 O2 态

`0x17059400` bits[4:0] = ONU 激活状态（`fi_Configure_Ponmac_Set_OnuState` 写）。
我们驱动 `pon_mac_tbl` 写的是 **3(O3)**，而厂商在 **O2** 态（`power_p2_lsi`）才发 SN。

- O3 态下触发 `0x170599bc` bit3：**永不清**（硬件拒发，等不到条件）。
- 把 `0x17059400` 改成 `0x03ff0002`（O2）后触发：**bit3 立即清 0，发送完成**（实测多次复现）。
- 发送像"进场一枪"：O3→O2 切换后触发一次成功率高；同一 O2 态里重复触发不一定每次都发。
  厂商本就靠 O2 状态机周期性重进 + 重发。

### 77.2 ★ SN 报文格式与 MIC（`setSnMIC`/`Drv_PloamuMic_fake` 解出）

48 字节 = 40 正文 + 8 MIC：

```
[0]=0x03 [1]=0xFF [2]=0x01(MsgID=Serial_Number) [3]=0x00
[4..11] = SN 8 字节（ZTEG0D09D561）
[12..15]= 随机延迟 4 字节（原厂取 base+0x264d）
[16..39]= 0
[40..47]= MIC
```

- MIC = **AES-CMAC(key = 0x55 × 16, data = 0x02 ‖ 40 字节正文)**，
  再做**零位奇偶翻转**：统计正文 40B 的 0 位数 zc，
  `idx = 7 - ((zc>>3)&7)`、`bit = zc & 7`，把 MIC[idx] 的这一位取反。
- 密钥来源：`memset(XgponDataBase+0x2691, 0x55, 16)`（注册前固定 0x55）。
- 写寄存器：`0x170598f8 = rev(MIC[0:4])`、`0x170598fc = rev(MIC[4:8])`。
  以 ZTEG SN + rand=0 算得：`0x170598f8=0xCBF6F509`、`0x170598fc=0x5C9DF5E5`（`work/sn_mic.py`）。
- 原厂另有 `0x170598f4`（写正文第 36 字节）与 `0x17058444` bit0 置位，未复刻。

### 77.3 实测结果：能发，但 OLT 不回

- 触发位清 0、连发 10+ 次均成功（硬件侧确认发出）。
- 下行始终只有周期 profile 报文（`03ff01xx 000004ad 4cc30f00 000000000409/081f aa… HWTC PON …`），
  `grant=09`/`1f` 交替；**没有任何 Password / Ranging / Assign_ONU_ID 这类回应**。
- 因此尚未进入注册第二步（MsgID=0x02 注册应答，RegID 36B@`XgponDataBase+0x2625`）。

**未决 / 下一步（按可能性排序）**
1. **突发时序 / 激光**：硬件"发完"≠ OLT 收到。需对比原厂 O2 态的突发参数
   （`0x17059910` 已=0x1b01、profile 四寄存器 0x17059880/94/a8/bc 已照抄 O5=静态表值）。
2. **RX ONU-ID 过滤**：OLT 若按 ONU-ID 单播回包，可能被 MAC 过滤掉，
   需查 `0x17058400` 等 RX 过滤配置。
3. **OLT 侧 provisioning**：该 SN 在 OLT 上可能视为"已在线"而不重新发现。
4. 抓原厂真实 O2 态快照（原厂固件启动后 telnet 抢在注册前 dump）做逐寄存器对比 —— 最权威。

### 77.4 事故记录：大量 sysfs 读会把以太网 RX 打瘫

用 `pon_reg` 逐地址 `echo+cat` 读 2048 个寄存器（每地址两次进程）后，
设备 eth0 **RX packets 归零、TX 正常**，网络层完全不通（内核活着、br-lan 有 IP），
`ip link set eth0 down/up` 无效，只能重启（回原厂固件 → 串口重灌）。
**结论：批量读寄存器必须用 `work/zxreg`（静态 aarch64，mmap，单进程），
禁止用 shell 循环打 pon_reg。**

### 77.5 ★ 注册码（用户确认，2026-10-18 23:5x）

- **注册码 = `f663702212`**（10 字节 ASCII，GPON password）。
- 原厂 O5 活体中的存放位置（big-endian 读即 ASCII）：
  `0x17059854=0x66363633`("f663")、`0x17059858=0x37303232`("7022")、`0x1705985c=0x31320000`("12")
- 紧挨着的 `0x1705984c=0x4342F052`、`0x17059850=0xAC5DCA31` 不是 ASCII，
  疑似该注册码的加密/校验形态（与 RegID 36B 块有关，待解）。
- 用途：GPON 注册第二步（OLT 下发 Password 后，ONU 回注册应答 MsgID=0x02，
  36B RegID @ `XgponDataBase+0x2625`）。
- **原厂 O5 活体 SN 寄存器仍为 `0x534B5957/0xA8D40BA7`**（BE ASCII "SKYW"），
  与 TLV 的 `ZTEGD09D561` 不同 —— 说明 OLT 实际接受的是 SKYW 这组（很可能 OEM 出厂 SN）。

---

## §78 驱动内注册流程：分步可行、整串会挂死（2026-10-18 23:30–01:00）

> 本节是 §77 之后在 `zx279131-pon.c` 里实做注册流程的实测记录。
> 交付固件 **v9.33**（`tftp/f7610u-v9.33-gponsettle.itb`）。

### 78.1 驱动新增接口（v9.29 起）

`/sys/bus/platform/devices/18000000.pp/` 下新增：

| 节点 | 作用 |
|---|---|
| `pon_gpon` (RW) | `sn <8B ASCII 或 16 hex>` / `password <10B>` / `mic auto\|manual` / `period <ms>` / `clear` |
| | **`step sn\|mic\|o2\|fire\|o3`** ← 单个动作，一次 sysfs 写只做一件事 |
| | `run <n>` 同步跑 n 轮完整流程 / `start [n]` 交工作队列 / `stop` |
| `pon_gpon_state` (RO) | tries/tx_ok/tx_timeout、last_state/last_ctrl、**msgid_hist**（下行报文 MsgID 直方图） |

`msgid_hist` 是本次最有用的一件工具：OLT 一旦认了 SN，直方图里就会出现
`02`(Serial_Number_ONU request) 或 `03`(Assignment_ONU_ID)，比任何寄存器都直白。

内核侧 MIC：probe 时 `crypto_alloc_shash("cmac(aes)")` 一次并缓存 tfm，
**不能在 work 里分配**（会走 request_module，工作队列卡死）。
`config-6.18` 需加 `CONFIG_CRYPTO_CMAC=y` / `CONFIG_CRYPTO_HASH=y` / `CONFIG_CRYPTO_AES=y`。
实测核内算出 `aba8a90059697115`，与 `work/sn_mic.py` 离线结果逐字节一致。

### 78.2 ★ 整串执行会挂死整机，分步不会

| 操作 | 结果 |
|---|---|
| `step sn` / `step mic` / `step o2` / `step fire` 分别单独写 | **全部正常**，设备不重启 |
| `run 1`（一次系统调用里做完 sn+mic+o2+fire+o3） | **MAC 卡死总线 → 整机 hang**，无串口输出，只能等看门狗（约 1–2 分钟）复位回原厂 |
| `start 1`（工作队列里做同样的事） | 同上 |

**根因（已定位）**：`gpon_send_sn()` 只在 fire 后轮询 75 ms 就放弃，
超时后**把 MAC 切回 O3**，而此时 `0x170599bc` bit3 仍置位（硬件还在等发送窗口）。
**在触发未完成时切 O3 会把 MAC 状态机搞死。**
⇒ 修复方向：bit3 未清时**绝不切 O3**，且等待时间要按秒级（见 78.3）。
（v9.33 的 `run/start` 仍是危险路径，只用 `step`。）

### 78.3 ★★ O2 后必须等 ~45 秒才命中 discovery 窗口

在 O2 态保持不动、每 5 秒触发一次（`0x170599bc` 写 `0xd`）实测：

```
t0..t8  (0–40s)   ctl=0xd   bit3 不清 → 硬件还没拿到发送时隙
t9..t11 (45–55s)  ctl=0x5   bit3 清   → 真的发出去了
```

⇒ **OLT 的 discovery 窗口周期约 45–60 秒**。此前所有「写 0xd 没反应」的尝试，
绝大多数是**根本没到窗口**，而不是 SN 或 MIC 错。

### 78.4 MIC 不参与发送决策

把 `0x170598f8/fc` 清 0（MIC 全零）后照样能在窗口内发出（bit3 清）。
⇒ 硬件发送不看 MIC；MIC 只影响 OLT 是否接受。

### 78.5 实测结果：SN 能发，OLT 仍不回

在窗口内成功发送多次（bit3 每次都清），下行 `msgid_hist` 从 236 条起
**始终只有 `01`**（Upstream_Overhead），**从未出现 02/03/04**。

下行报文实测稳定为 4 帧循环（每 0.5 s 一条，seq 递增）：

```
03ff01xx 020004ad 4cc30f00 00000000 08 1f aa… "HWTC PON" … 
03ff01xx 030104a5 6679e000 00000000 08 1f aa… "HWTC PON" …
03ff01xx 000004ad 4cc30f00 00000000 04 09 aa… "HWTC PON" …
03ff01xx 010104a5 6679e000 00000000 04 09 aa… "HWTC PON" …
```
off15（08/04）与 off16（1f/09）成对交替，像是两组 T-CONT/alloc 配置轮转。

**判定**：`0x170599bc` bit3 清只能证明「MAC 把突发送进了发送通道」，
**不能证明上了光纤**。OLT 无任何回应，说明要么上行没真出去（激光/突发时序），
要么 OLT 侧丢弃（报文格式/MIC/该 SN 未开通）。

### 78.6 当前可用工具

- `work/gpon-register.sh`：分步跑完整一轮（`gpon-register.sh [间隔秒]`，默认 2 s）。
  实测 5 步 × 3 s 跑完**设备不挂**。
- ⚠ 但 2–3 秒间隔**不到 discovery 窗口**，脚本跑完 SN 只是停在发送通道里。
  要真发出去，需在 O2 态保持 ≥45 秒再触发 —— 见 78.3 的循环写法。
- `work/sn_mic.py`：离线算 MIC（openssl CMAC + 零位奇偶翻转）。

### 78.7 未决 / 下一步

1. **改驱动**：fire 等待改成 ≥60 s，bit3 未清绝不切 O3 —— 这样 `run/start` 才安全。
2. **验证上行是否真上光纤**：最硬的办法是在原厂固件 O2 态抓一次发送瞬间的
   `0x170599bc`/发送通道寄存器，与自研逐格对比（原厂在 O2 只停留极短，需高频采样）。
3. **SN 候选**：目前用 O5 活体 `534B5957/A8D40BA7`（"SKYW"）；mtd12 TLV 里另有
   `ZTEGD09D561`（11 字符，非 8 字节标准 SN）。两组都该试。
4. OLT 侧该 SN 是否开通，无法本地确认 —— 若原厂插上就能上网，说明开通的是原厂那组。

---

## §79 原厂 O2/O1 活体对比：原厂根本不用 bit3（2026-10-19 01:00–02:15）

> 这一节推翻了 §77/§78 的核心假设。以本节为准。

### 79.1 ★★ 原厂重注册全过程实测（抖动 LOS 逼迫重走状态机）

方法：原厂固件（4.1.25）telnet 登录，后台 16 ms/点采样
`0x17059400`(state) / `0x170599bc`(snctl) / `0x17059910`(burst) / `0x1705947c`(alloc)，
前台高频清 `0x17058400` bit0 制造 LOS。实测时间线：

```
uptime  state      snctl  burst   alloc
179.31  0x90005    0x1    0x1B01  0x3FF0000     O5 在线
183.35  0x90006    0x1    0x1B01  0x43FF0000    掉线过渡
183.52  0x3FF0001  0x1    0x1B01  0x43FF0000    O1
193.85  0x3FF0003  0x1    0x1B01  0x43FF0000    O3   ← 10 秒后
195.77  0x90004    0x1    0x1B01  0x3FF0000     O4
196.00  0x90005    0x1    0x1B01  0x3FF0000     O5
```

**三条硬结论**：

1. **原厂全程没有出现 O2**（0x3FF0002）。O1 → O3 直接过渡。
2. **`0x170599bc` 全程恒为 `0x1`，bit3 一次都没有置位**
   （单独跑 963 点、16 ms 间隔的单寄存器采样，`awk '$2!="0x1"'` 计数为 0）。
   ⇒ **§77/§78 里「写 bit3 触发 SN」的路径，原厂根本不用。**
3. **`0x1705947c` bit22 = 未注册标志**：O1/O3 时 `0x43FF0000`，
   O4/O5（拿到 alloc）时 `0x03FF0000`。这一位可以当"是否已分配到 ONU-ID"的判据。

### 79.2 O1 态保持 60 秒：什么都不发生

我们固件上把 state 钉在 `0x03FF0001`（O1）并保持 60 秒：

- state 纹丝不动，**不会自己跳到 O3**（硬件不自动发 SN）
- `msgid_hist` 只有 `01`（Upstream_Overhead），**从未收到 `02`（OLT 的 SN 请求）**

⇒ OLT 在这个 PON 口**不发 Serial_Number_ONU request**；原厂能进 O3，
是它自己的软件状态机（gpondrv）靠**超时**驱动的，不是等 OLT 的 02。

### 79.3 三种发送路径全部试过，OLT 均无回应

| 路径 | 条件 | 结果 |
|---|---|---|
| 硬件 bit3（`0x170599bc` 写 `0xd`） | O2 态 + 等 45 s 窗口 | bit3 清（硬件接受），OLT 无回应 |
| 软件 FIFO（`pon_send` 48B + `pon_fire`） | O3 态，24 次/120 s | 12 次真发出（sent=12），OLT 无回应 |
| 软件 FIFO | O2 态，10 次/50 s | 全部发出，OLT 无回应 |

SN 帧（MsgID=0x01，实测格式）：
```
03 ff 01 00 | 53 4b 59 57 a8 d4 0b a7 | 00 00 00 00 | 0 × 24 | ab a8 a9 00 59 69 71 15
```

**判定**：问题不在「用什么方式发」，而在**发送那一刻 MAC 的完整配置**。
原厂在 O1→O3 的 10 秒里，软件按 GPON 协议栈写了一大片寄存器
（alloc 表、burst profile、PLOAM 通道、ONU-ID 预留等），我们手工只覆盖了 4–6 个。

### 79.4 因此：手工 poke 这条路走到头了

要继续，只有两条：

1. **在驱动里实现 GPON 激活状态机**（O1 定时 → 配 burst/alloc → 发 SN → O3 → 收 PLOAM →
   O4 → O5），寄存器语义从 `zte_xgpon.dis` 逐函数挖。工作量大（数天级）。
2. **移植原厂 gpondrv**：它是 4.1.25 内核模块，vermagic 不匹配，需要重编或大量适配。

短期能做的验证：在原厂 O1→O3 的 10 秒窗口里，用 `devmem2` 分批扫
`0x17058000-0x17059fff` 的关键子块（每次 30–40 个寄存器，约 0.5 s），
把 O1 与 O3 两组快照做 diff，找出原厂在这 10 秒里到底改了哪些寄存器。
（`hexdump -s /dev/mem` 不可用：`Bad address`，原厂没有 `dd`。）

### 79.5 附带发现

- 原厂用的 SN 就是 **`534B5957 / A8D40BA7`（"SKYW"）**，
  重注册成功后 `0x170598d8/dc` 仍是这两个值 ⇒ OLT 认的确实是这组。
- mtd12 的 `ZTEGD09D561` 是盒子序列号，与 GPON SN 无关。
- U-Boot 的 tftp 连续失败后会耗尽包缓冲池，串口刷 `alloc bp failed`；
  只能 `reset` 才能清（`work/ub_reset_grab.py`）。

---

## §80 原厂 O1 快照 + 全量配置对齐（2026-10-19 01:35–02:20）

### 80.1 拿到原厂 O1 快照

方法：清 `0x17058400` bit0 制造 LOS 逼重注册（抖动约 5 秒），O1 窗口内用 `devmem2`
顺扫 768 个寄存器（7.8 秒扫完，10 秒窗口够用）：

- `0x17059400` .. `0x170594ff`（256 个）
- `0x17059800` .. `0x170599ff`（512 个）

产物：`work/stock_o1.txt`（"addr value" 文本）。
取回必须**分批**（整文件 19 KB 会被 telnet 丢包，`work/stock_fetch.py` 按 96 行一段取）。

### 80.2 ★ 原厂 O1（注册前）就已经写好的东西

**这是本轮最重要的发现**：下面这些在**未注册状态**就存在，说明是本地配置，不是 OLT 下发：

| 寄存器 | O1 值 | 含义 |
|---|---|---|
| `0x17059854` | `0x66363633` | ASCII **"f663"** |
| `0x17059858` | `0x37303232` | ASCII **"7022"** |
| `0x1705985c` | `0x31320000` | ASCII **"12"** |
| | | ⇒ **注册码 `f663702212` 在 O1 就写好了** |
| `0x1705984c` | `0x4342f052` | 注册码的加密/校验形态（待解） |
| `0x17059850` | `0xac5dca31` | 同上 |
| `0x17059878` / `7c` | `0x1ee5fff2` / `0xc1106b3a` | 未知，疑似同一组密钥 |
| `0x170599d8..f4` | 8 个 32-bit | **一串非零密钥材料**（`fab0a992 64aa2dea 869ce0e2 b4af015e e371dedc 3d86af5f d021ccc8 73ccf4da`），我们原来全是 0 |

### 80.3 我们 vs 原厂 O1：76 处差异，已对齐 57 处

关键差异（`work/diff_ours_o1.py`）：

| 寄存器 | 我们 | 原厂 O1 | 说明 |
|---|---|---|---|
| `0x17059880/94` | `0x001f0308` | `0x00190808` | burst profile 0/1 |
| `0x170598a8/bc` | `0x001f0308` | `0x00190808` | burst profile 2/3 |
| `0x17059844` | 0 | `0x00000003` | — |
| `0x17059440` | 0 | `0x00004409` | alloc 9 |
| `0x170599d8..f4` | 全 0 | 8 个密钥值 | 见 80.2 |
| `0x17059a10` | 0 | `0x00070707` | — |
| `0x17059a58/5c` | 0 | `0x000077d8` | — |
| `0x17059b30/34` | 0 | `0x7d5d5fcd` / `0xedb77b01` | — |

（`0x17059988..a4` 一组看着像配置，但**写不进去且数值自己增长** ⇒ 是硬件计数器，跳过。）

用 `work/align_o1.sh`（57 条 pon_reg 写）把上述差异全部写成原厂 O1 值，回读确认生效。

### 80.4 对齐后重试：OLT 仍然不回

- O1 态 + 软件 FIFO 发 SN 帧 ×8（每次间隔 4 秒）→ `msgid_hist` 仍只有 `01`
- 之前 O2 + bit3、O3 + FIFO、O2 + FIFO 也都试过 → 同样无回应

**结论**：配置差异已经不是原因。我们与原厂 O1 的寄存器状态基本一致（除动态计数器），
SN 也发得出去（bit3 清 / FIFO sent 递增），但 **OLT 一个字都不回**。

### 80.5 光模块状态（排除 TX_DISABLE）

A2 页 DDM（`hexdump -s 96` 读 `0-0051/eeprom`）：

```
0x60: 2c b4 81 65 00 00 31 6a 00 57 ff ff 00 00 80 04
      temp  Vcc   bias   TXpwr  RXpwr       0x6E
```

- `0x64-65` TX bias = 0（突发模式下空闲为 0，正常）
- `0x66-67` **TX power = `0x316a` ≈ 1265 µW ≈ +1.0 dBm** ⇒ **激光有输出，没被 TX_DISABLE 锁死**
- `0x6E` = `0x80`（bit7）。按 SFF-8472 是 TX_FAULT，但 TX power 明明有值 ⇒
  该模块此位定义与标准不同，**不能当故障判据**

### 80.6 剩下的可能（按可能性）

1. **上行突发的模拟部分不对**：preamble/delimiter 的比特图案、pre-equalization delay、
   激光开断时序——这些在 MAC 内部，改寄存器改不出来。
2. **OLT 侧策略**：该 SN 被标记为"已知/已在线"而不重新 discovery；或者 PON 口关了自动发现。
3. **SN 与 password 的绑定组合**：OLT 可能要"SN + password"一起才对（password 在 O4/O5 才发，
   但如果 OLT 先校验 SN 再要 password，我们连第一步都没过）。

⇒ 要继续只能往「上行突发时序」和「OLT 侧行为」两个方向查，
而这两条都需要示波器 / OLT 权限，本地寄存器层面已经挖到底了。

---

## §81 ★★★ 设备是 XG-PON，不是 GPON（2026-10-19 02:00，用户确认）

> 这一节推翻 §1–§80 的**协议前提**。之前所有"GPON 注册"的尝试都建立在一个错误假设上。

### 81.1 证据

1. **用户明确说：「我这个光猫就是 XG 的」。**
2. 光模块 A0 EEPROM 名称 = **`ZTE_XEPON_AS`**（XEPON = XG-PON）。
3. **PLOAM 帧长 48 字节**。GPON 的 PLOAM 是 13 字节；**XG-PON 才是 48 字节（40 字节 payload + 8 字节 MIC）**。
   §2 当初把这个 48 字节当成"这个硬件的报文粒度"，其实它就是 XG-PON 的标准帧长。
4. 对比参考实现 `pbs05/openwrt-pon-drivers`（`airoha-xpon`）里的常量：

```c
AIROHA_XGPON_PLOAMD_PROFILE        0x01   ← 我们收到的下行 0x01 正是它
AIROHA_XGPON_PLOAMD_ASSIGN_ONU_ID  0x03
AIROHA_XGPON_PLOAMD_RANGING_TIME   0x04
AIROHA_XGPON_PLOAMD_REQUEST_REGIS  0x09
AIROHA_XGPON_PLOAMD_ASSIGN_ALLOC_ID 0x0a
AIROHA_XGPON_PLOAMD_KEY_CONTROL    0x0d
AIROHA_XGPON_PLOAMU_REGISTRATION   0x02   ← 上行注册帧 MsgID = 0x02
AIROHA_XGPON_PLOAMU_KEY_REPORT     0x05
AIROHA_XGPON_PLOAMU_ACKNOWLEDGE    0x09
```

我们收到的下行 `03ff01xx 020004ad 4cc30f00 …` 里 **byte2 = 0x01 = Profile**，
不是 GPON 的 Upstream_Overhead。OLT 一直在周期性广播 XG-PON Profile。

### 81.2 因此之前错在哪

| 事项 | 之前（GPON 假设） | 实际（XG-PON） |
|---|---|---|
| 下行 0x01 | Upstream_Overhead | **Profile**（含 discovery/突发参数） |
| 上行注册 MsgID | 0x01（Serial_Number） | **0x02（Registration）** |
| PLOAM 长度 | 以为"这个硬件特殊" | 标准 48 字节 |
| 状态机 | O1–O5（GPON，ITU-T G.984） | **O1–O5（XG-PON，ITU-T G.987）**，事件不同 |
| SN/MIC | GPON fake MIC（key 0x55×16） | XG-PON 用 **PLOAM integrity key**（default IK） |

⇒ 我们用 GPON 的 SN 帧去回 XG-PON 的 OLT，**协议层根本对不上**，
所以"发出去了但 OLT 一字不回"完全说得通。

### 81.3 已试（仍然无回应）

- `03ff02 00 | SN | pad | MIC(CMAC 0x55×16)` → 10 次，`msgid_hist` 仍只有 `01`
- `03ff02 00 | SN | pad | MIC=0` → 11 次，同样

⇒ 光改 MsgID 不够。XG-PON 注册还要求：
1. **解析 Profile 消息**（0x01）拿到 discovery grant / burst profile 参数；
2. **正确的 default PLOAM IK**（我们用的 0x55×16 是从 GPON 的 `Drv_PloamuMic_fake` 推的，XG-PON 未必是它）；
3. 在 OLT 指定的 **discovery window** 内发送。

### 81.4 现成的协议参考

`pbs05/openwrt-pon-drivers` 的 `airoha-xpon/src/`（已 clone 到 `/home/wen/WorkBuddy/zx/pon-drivers-ref/`）：

| 文件 | 价值 |
|---|---|
| `airoha-xpon-ploam.c`（1104 行） | **完整 PLOAM 状态机**：`try_arm_upstream`（上行使能的前置条件清单）、`send_registration`、`accept_assign_onu_id`、`accept_ranging_time`、`accept_alloc_id`、`handle_ploamd` 分发表 |
| `airoha-xpon-xgpon.c`（791 行） | XG-PON 模式/PHY/TC 配置 |
| `airoha-xpon-identity.c` | SN / password / registration-id 管理 |
| `airoha-xpon-crypto.c`（414 行） | **default PLOAM IK + 激活密钥**处理 |
| `airoha-xpon-main.c` | 状态机主循环、`onu_state` 迁移 |

**关键洞见（`try_arm_upstream`）**：上行发送前必须同时满足——
有光 / MAC 初始化完 / **default PLOAM IK 已编程** / **激活密钥已编程** /
SN 已配置 / **burst profile 已编程** / ONU 状态 ∈ {O2/O3, O4, O5} /
PHY ready + RX sync in / **TX sync ready**，最后才 `frontend_set_tx_enable(true)`（开光模块 burst gate）。

我们缺的正是这套"armed"链条。

### 81.5 下一步（按顺序）

1. **解析 Profile 消息**：把收到的 `03ff01xx` 48 字节逐字段拆开（G.987.3 §11.3.1），
   拿到 superframe / discovery grant / delimiter / preamble 参数。
2. **搞清 default PLOAM IK**：从 `airoha-xpon-crypto.c` 看它从哪来（是否固定常量 / 是否 OLT 下发），
   再到 `zte_xgpon.ko` 里找对应的默认密钥写点。
3. **状态机按 G.987 重写**：O1 →（收 Profile）→ O2 →（discovery window 发 Registration 0x02）→
   O3 →（收 Assign_ONU-ID 0x03）→ O4 →（Ranging_Time 0x04）→ O5（Assign_Alloc-ID 0x0a）。
4. 光模块型号确认支持 XG-PON（`ZTE_XEPON_AS` 已提示），SerDes 速率配置需按 9.953G/2.488G 核对。

---

## §82 全量 6144 对比 + SerDes 差异（2026-10-19 02:10–03:00）

### 82.1 ★ 整个 0x17000000 窗口 6144 个寄存器，只有 32 个差异

用 `work/zxreg d 0x17000000 6144` dump 我们的，与 `work/o5_regs.json`（原厂 O5）逐格对比：

| 类别 | 数量 |
|---|---|
| 我们 = 0、原厂 ≠ 0 | **0** |
| 我们 ≠ 0、原厂 = 0 | **0** |
| 两边都非 0 但不同 | **32**（＝4 个 bank × 8 个寄存器） |

那 32 个全是**注册之后才出现的状态**，四个 bank 布局相同（0x17000000 / 0x17000400*4 / 0x17000800 / 0x17000c00）：

```
0x..24  c8        vs 000100c8      bit16
0x..68  61a8015c  vs 61a80150
0x..94  0000000f  vs 00000187
0x..98  0000072e  vs 000035b9
0x..a0  0000000f  vs 00000059
0x..ac  000f000f  vs 01870187
0x..b0  000f000f  vs 01870187
0x..c4  0000072e  vs 00001989
```

⇒ **MAC 侧配置已经和原厂注册后的状态一致**。发送失败不是"少配了某个 MAC 寄存器"。

### 82.2 ★★ 只有 SerDes / CRM 两处真正的配置差异

（这是 6144 个之外的区域，之前从未对比过）

| 寄存器 | 含义 | 原厂 O5 | 我们 |
|---|---|---|---|
| `0x10e40058` | SerDes word 0x58 | `0x03E23333` | `0xB3E23333` |
| `0x10e10050` | CRM word 0x50 | `0x00DFF3FF` | `0x001F8070` |

其余全部一致，包括：
- `0x10e10038` = `0x0828142A`（PLL word，与驱动 `PLL_WORD_GPON` 相同）
- `0x10e10040` = `0x00001944`（ref clk，与 `REF_CLK_GPON` 相同）
- `0x10e40068` = `0x598`（SerDes status，rx_ready/pll_lock 全 1）
- `0x10e40060` = `0x8000`、`0x10e40070` = `0x916596`

`0x10e40058` 的差异在最高半字节（`0xB` vs `0x0`，即 bit31/29/28），
`0x10e10050` 差异更大 —— **两者都可能是"模式/速率"选择位**。
⇒ 下一步最值得试的就是把这两个写成原厂值（SerDes 写入有风险，需做好重启准备）。

### 82.3 XG-PON Profile 消息字段布局（从参考实现取得）

`airoha_xpon_program_profile()` 的解析方式（message 为 48 字节帧）：

```
index         = message[4] & 0x03
line_rate     = (message[4] >> 2) & 0x01
version       = message[4] >> 4
FEC enable    = message[5] & 0x01
delimiter_len = message[6] & 0x0f
delimiter pat = message[7..14]   (8 B)
preamble_len  = message[15] & 0x0f
repeat        = message[16] & 0x1f
preamble pat  = message[17..24]  (8 B)
PON Tag       = message[25..32]  (8 B)
length        = preamble_len * repeat + delimiter_len
```

拿我们收到的帧代进去：`03ff0101 020004ad 4cc30f00 00000000 08 1f aa…` →
`index=2`、`repeat=8`、`delimiter_len=4`、PON Tag 从 `message[25]` 起（`aa 48 57 54 43 20 50 4f`，含 "HWTC"）。
**PON Tag 用于派生激活密钥**（`program_activation_keys`）。

### 82.4 默认 PLOAM IK 已确认

`airoha_xpon_program_activation_keys()`：

```c
static const u32 default_msk[4] = {
    0xb5d432eb, 0x538bb1b4, 0xe95e6ee3, 0x2437be54 };
...
for (i = 0; i < 4; i++)
    airoha_xgpon_write(xpon, AIROHA_XGPON_PIK1_0 + i*4, 0x55555555);
```

⇒ **默认 PIK = `0x55555555` × 4 = 16 字节 `0x55`**，和我们算 MIC 用的 key 一致。
**MIC 算法没问题**，所以"OLT 不回"不是密钥导致的。

### 82.5 U-Boot 灌机的两个新坑（已写工具）

1. **tftp 反复失败后 `alloc bp failed` 不可逆**：连 `ping` 都发不出去。
   warm reset 也清不掉。→ `work/ub_reset_grab.py`（reset + 单次 open 喂键）
   有时能清，但更可靠的是**冷启动路径**（从原厂 telnet `reboot -f` 再抢 U-Boot）。
2. ★ **中断过 autoboot 的 U-Boot 无法手动启动原厂**：`bootcmd` 是
   `setenv bootargs …;bootm 0x88000240`，而那个地址的镜像是 autoboot 流程负责加载的，
   中断后 `bootm 0x88000240` 会报 `B't get kernel image`。
   ⇒ 让设备回原厂的办法：串口发 `reset` 然后**完全不喂键**，等 95 秒
   （`work/ser_reset_nokey.py`）。U-Boot 自己不被打断就会加载并启动原厂。

---

## §83 SerDes/CRM 对齐后仍然发不出去（2026-10-19 03:30–04:00）

### 83.1 v9.34：把 SerDes 区加进 pon_reg 白名单

`pon_addr_ok()` 原来只放行 `0x10e00000–0x10e11000`（sys/CRM），
SerDes `0x10e40000` 读写都被拒（`ash: write error: Invalid argument`）。
v9.34 增加：

```c
if (addr >= PON_SERDES_BASE && addr < PON_SERDES_BASE + PON_BLOCK_SIZE)
    return true;
```

之后 `0x10e40058` / `0x10e40068` 都能正常读写。

### 83.2 写原厂值：链路没断，但发送依然无效

| 操作 | 结果 |
|---|---|
| `0x10e10050 ← 0x00DFF3FF`（CRM） | 写入成功，`status` 仍 `0x598`（rx_ready/pll/sig 全 1） |
| `0x10e40058 ← 0x03E23333`（SerDes） | 写入成功，`status` 仍 `0x598`，下行报文照收 |

⇒ **这两个寄存器不是"GPON/XG-PON 模式开关"**，改了不影响链路，也没让 OLT 回应。

### 83.3 最后的发送尝试：XG-PON O2 态持续 56 秒

state 固定在 `0x03FF0002`（O2），每 2 秒压一帧 `Registration(MsgID=0x02)` 并 fire，
共 28 次（`sent` 累计到 34，说明大部分真的进了发送通道）：

```
state=0x03ff0002   queued=0 sent=34
msgid_hist: 00:0 01:120 02:0 03:0 …（全部为 0）
```

**OLT 仍然只发 `01`(Profile)，一个字都没回。**

### 83.4 结论：寄存器层面已经挖到底

已经排除的：

| 假设 | 排除依据 |
|---|---|
| MAC 配置不全 | 6144 个寄存器与原厂 O5 只差 32 个状态位（§82.1） |
| MIC/密钥错 | 默认 PIK 就是 `0x55555555`×4，与我们的算法一致（§82.4） |
| 缺注册码/密钥材料 | 原厂 O1 的注册码区、8 个密钥寄存器已全部对齐（§80.2） |
| SerDes 模式错 | `0x10e40058`/`0x10e10050` 改成原厂值，链路与发送都不变（§83.2） |
| 发送时机没命中窗口 | O2 态连续 56 秒、28 帧全覆盖，仍无回应（§83.3） |
| 光模块被关 | DDM TX power ≈ +1.0 dBm（§80.5） |

**剩下的只可能是上行突发的模拟时序**：XG-PON 上行 2.488 Gbps 的突发，
preamble/delimiter 的比特图案、激光开断沿、pre-equalization delay ——
这些在 MAC/PMA 的模拟侧，寄存器改不出来。

**要继续往下走，需要**：示波器抓 ONU 端上行光波形（看有没有突发、图案对不对），
或者拿到 OLT 侧这个 ONU 的注册状态/日志。

### 83.5 交付物

- `tftp/f7610u-v9.34-serdesreg.itb`（PON 驱动 + SerDes 白名单）
- `work/ub_cold_boot.py`：冷启动一体脚本（抢 U-Boot → tftp → pon_reset → bootm，单次 open 串口）
- `work/ub_grab_long.py`、`work/ser_reset_nokey.py`：抢提示符 / 静默回原厂

---

## §84 ★★★ 反汇编找到根因：上行使能位 bit1 从未打开（2026-10-19 04:45–05:15）

### 84.1 突破口：`p_o2_3_profile_msg` 的完整调用链

从 `work/zte_xgpon.dis` 反汇编 O2/O3 状态下处理 OLT Profile 消息的函数：

```asm
<p_o2_3_profile_msg>:
    bl  fi_Configure_Ponmac_Set_Profile        ; ① 写 burst profile
    bl  setSnMIC                               ; ② 重算 SN MIC
    bl  fi_Configure_Ponmac_Upstream_Speed_Para ; ③ 上行速率参数
    bl  generate_key                           ; ④ 从 PON Tag 派生密钥
    strncmp(msg+?, "HWTC"/..., 4)              ; ⑤ 按厂商分支
    ...
    bl  fi_Configure_Ponmac_Set_Upstream_Enable(1) ; ⑥ ★ 打开上行使能
```

**⑥ 这一步我们从来没做过。**

### 84.2 `Set_Upstream_Enable` 写的是 `0x17059800` 的 bit1

```asm
<fi_Configure_Ponmac_Set_Upstream_Enable>:
    ; enable == 1 分支（0x4418）
    ReadXmacReg(0x16600, &val)
    val |= 0x2                 ; ← bit1
    WriteXmacReg(0x16600, val)
    ; enable == 0 分支（先读，and 0xfffffffd 清 bit1，再写）
```

`0x16600` 是 XMAC **字号索引**，物理地址 = `0x17000000 + 0x16600*4` = **`0x17059800`**。

| 状态 | `0x17059800` | bit1（上行使能） |
|---|---|---|
| **原厂 O5** | `0x16` | **1** ✔ |
| 我们（一直） | `0x15` | **0** ✘ |

**上行使能从开机到现在就没打开过。**
这完美解释了全部现象：`0x170599bc` bit3 会清、`pon_send` 的 `sent` 会加——
那只是"帧进了发送通道"，但总闸 bit1=0，光口上什么都没出去，OLT 自然一个字都不回。

### 84.3 待验证

灌 v9.34 后：
1. 读 `0x17059800` 确认当前值
2. 写 `0x17059800 |= 0x2`（`0x15` → `0x17`；或直接写原厂的 `0x16`）
3. 发 `03ff02 00 | SN | pad | MIC`（XG-PON Registration）
4. 看 `msgid_hist` 是否出现 `03`(Assign_ONU_ID)

如果 bit1 就是总闸，这一步应该立刻有反应。

### 84.4 同一函数里另外两个待挖的点

- `fi_Configure_Ponmac_Upstream_Speed_Para`：**上行速率参数**（XG-PON 与 GPON 的关键差异点）
- `fi_Configure_Ponmac_Set_Profile`：burst profile 的真正写入序列（我们目前只对齐了 `0x17059880/94/a8/bc` 四个值）
- `fi_Configure_Ponmac_Set_Optx_Close_Ctl` / `_Soft_Vcct_Ctl` / `_Vcct_Polar`：**光模块 TX 控制**

---

## §85 Set_Profile 的寄存器格式（2026-10-19 05:20–06:00）

### 85.1 一个必须先纠正的乌龙

`0x17059800` 的 `0x15` **是我自己写出来的**，不是硬件默认。
§80 对齐"原厂 O1"时把 `0x17059800` 也照抄成 `0x15`（O1 确实是 `0x15`），
结果把**上行使能 bit1 关掉了**。硬件开机默认是 `0x16`（bit1=1）。

⇒ `align_o1.sh` 里对 `0x17059800` 那一行是错的，**发 SN 前必须保证它是 `0x16`**。
（§84 的"根因"因此不成立，bit1 本来就开着。）

### 85.2 `0x17059428`：delimiter 参数寄存器（XMAC 0x1650a）

```asm
ReadXmacReg(0x1650a, &v)
bfxil w0, delimiter_len, #0, #4   ; bits[3:0]  = delimiter_len
orr   w0, w0, #0x1                ; bit0       = 1（使能）
bfi   w0, fec, #1, #1             ; bit1       = FEC enable
WriteXmacReg(0x1650a, v)
```

我们的 `0x17059428` = **`0xdd`**（低 4 位 = 13），是从驱动 `pon_mac_tbl[]` 里带出来的，
而 OLT 的 Profile 消息里 `delimiter_len = 4`。改成 `0x5`（=4 且 bit0=1）后链路正常，
但发送仍无回应。

### 85.3 4 个 profile 寄存器的字段格式

`XMAC 0x16620 / 0x16625 / 0x1662a / 0x1662f` → 物理
**`0x17059880 / 0x17059894 / 0x170598a8 / 0x170598bc`**（步长 5 字 = 0x14 字节）：

```asm
bfi w24, delimiter_len, #8, #4    ; bits[11:8]  = delimiter_len
bfi w24, preamble_len,  #16, #8   ; bits[23:16] = preamble_len
andi w24, w24, #0xffffff
WriteXmacReg(0x16620 + idx*5, w24)
```

对照三组值：

| 寄存器 | 值 | bits[11:8] | bits[23:16] |
|---|---|---|---|
| 原厂 O1（默认） | `0x00190808` | 8 | 0x19 |
| **原厂 O5** | `0x00090404` | **4** | 0x09 |

⇒ O5 的 delimiter_len 是 4，和 Profile 消息一致；O1 的 8 只是默认值。
**§80 对齐 O1 时把这四个写成 O1 值是错的**，应该用 O5 的 `0x00090404 / 0x001f0408`。
（已试，链路正常但发送仍无回应。）

### 85.4 ★ 还没做的关键一步：preamble / delimiter 的**比特图案**

同一个函数里还有 4 次写：

```asm
memcpy(buf, msg + 0x11, preamble_len)   ; 从消息里拷 preamble 图案
memcpy(buf2, msg + 7,  delimiter_len)   ; 拷 delimiter 图案
rev   w1, w1                            ; 字节翻转
Hal_WriteXmacReg(addr_a, w1)            ; 4 个寄存器
Hal_WriteXmacReg(addr_b, w1)
Hal_WriteXmacReg(addr_c, w1)
Hal_WriteXmacReg(addr_d, w1)
```

**这四个"图案寄存器"我们从来没写过**——它们不是 `0x17059880` 那 4 个 profile，
而是由栈上的地址表（`0x16620 + idx*5` 附近的另一组）决定，需要继续追 `x20/x22` 的基址。

⇒ **上行突发的 preamble/delimiter 比特图案目前是硬件默认值**，
这极可能就是"帧发得出去但 OLT 解不出来"的最后一环。

### 85.5 下一步

1. 追 `fi_Configure_Ponmac_Set_Profile` 里那 4 个图案寄存器的确切地址
   （`ldur w0, [x22, #-80]` 的 x22 基址、以及 `[x20,#-64]` 那组），
2. 把 OLT Profile 消息里的 `msg[7..14]`（delimiter）和 `msg[17..24]`（preamble）
   按 `rev` 后的字节序写进去，
3. 再发 Registration。

---

## §86 ★★ 驱动里两处 SerDes/CRM 值写错（2026-10-19 07:20–07:45）

### 86.1 SerDes 段只有 22 个非零寄存器

`/tmp/zxreg d 0x10e40000 1024`（1024 个）→ **非零仅 22 个**，全部落在
`0x10e40000 – 0x10e40070`，其余 995 个恒 0。

所以对比 SerDes 只需要比前 64 个（§82 已做），结论仍然成立：
**唯一的配置差异是 `0x10e40058`**。

### 86.2 ★ 但驱动每次启动都会把它写回错的值

`zx279131_pon_serdes_init()` 里：

```c
writel(0x00000059, pon_crm + CRM_AUX);      /* 原厂活体是 0x00DFF3FF */
...
writel(0xb3e23333, pon_ser + SERDES_PLL_WORD);  /* 原厂活体是 0x03E23333 */
```

⇒ 之前手工把这两个改成原厂值，**一重启就被驱动覆盖回去**，所以那两次实验等于白做。

### 86.3 v9.35 修正

```c
writel(0x00dff3ff, pon_crm + CRM_AUX);
writel(0x03e23333, pon_ser + SERDES_PLL_WORD);
```

`0x10e40058` 的差异在 bit31/29/28（`0xB` → `0x0`），
这两个位在 PMA/PLL 里通常是**分频/倍频选择**，直接影响速率。
XG-PON 上行 2.488 Gbps、下行 9.953 Gbps，和 GPON 完全不同档 ——
**这是"下行收得到、上行发不出去"最合理的解释**。

### 86.4 状态

- `tftp/f7610u-v9.35-serdesfix.itb` 已编译打包。
- 灌机失败（U-Boot `tftp` 又是 `Retry` / `alloc bp failed`），**待冷启动后再灌**。
- 冷启动灌机路径已验证可用：断电上电 + 手动停在 `=>` → `uboot_load.py` 一次成功。

### 86.5 下一步

灌 v9.35 后：
1. 确认 `0x10e40058 == 0x03E23333`、`0x10e10050 == 0x00DFF3FF`（驱动写的，不用手工）
2. 确认 `status` 仍 `0x598`（链路没断）
3. 发 `03ff02 00 | SN | pad | MIC`，看 `msgid_hist` 是否出现 `03`

---

## §87 v9.35 起不来：CRM_AUX 不能照抄原厂值（2026-10-19 08:00）

灌 v9.35（同时改 `CRM_AUX=0x00DFF3FF` 和 `0x10e40058=0x03E23333`）后：
`bootm` 报 Starting kernel，但**网络起不来**（`No route to host`），串口只有
`done / Unsupported hardware. Net`，随后整机不通。

⇒ **`CRM_AUX` 在 PON 初始化这个时间点不能写 `0x00DFF3FF`**。
§83 里手工写它没事，是因为那时时钟树早就跑起来了；在 probe 早期写会破坏时钟，
后面 UART/网络初始化全崩。

**回退**：v9.36 只保留 `0x10e40058 = 0x03E23333`（SerDes PLL word），
`CRM_AUX` 恢复 `0x00000059`。

- `tftp/f7610u-v9.36-serdesonly.itb`
- 待灌（设备当前不通，需冷启动）

**教训**：`CRM_AUX` 那一处如果要对齐，必须放到 PON 初始化**之后**再做，
不能把它塞进 §68 的 SerDes 序列里。

---

## §88 注册帧的真实路径（反汇编定论，2026-10-19 08:40–09:05）

### 88.1 ★ 纠正 §77：「`Send_Sr_Ploam_Msg` 不是 SN 发送函数」

`fi_Configure_Ponmac_Send_Sr_Ploam_Msg` 的 **13 个调用者全部是省电模式函数**：

```
power_p2_lsi / power_p2_ldi / power_p2_lpi
power_p3_fwi / power_p3_sa_off / power_p3_lwi
power_p4_lwi
power_p5_fwi / power_p5_sa_off / power_p5_lwi
power_p6_fwi / power_p6_sa_off / power_p6_lwi
```

⇒ 它发的是 **Sleep Request**（P2–P6 省电档），跟注册无关。
§77「首发帧走硬件 `Send_Sr_Ploam_Msg`」这个说法**不成立**。

### 88.2 真正的注册路径

```
p_o5_request_registration_msg
    └── send_registration_msg
            ├── Drv_PloamuMic_Generate_Activation   ← MIC（激活密钥）
            └── fi_Configure_Ponmac_Send_Ploam_Msg  ← 软件 FIFO 发送
```

`send_registration_msg` 的帧构造（反汇编读出来的）：

```asm
ldurh  w0, [x3, #75]        ; ONU-ID（broadcast 0x3FF）
strb   w0, [x29, #32]       ; byte0 = 高字节
lsr    w0, w0, #8
strb   w0, [x29, #33]       ; byte1
strb   w6, [x29, #34]       ; byte2 = 0x02 (MsgID)  ★
strb   w?, [x29, #35]       ; byte3 = seq
lp     x4,x5,[x2]           ; 从 [x19+0x2625] 拷 40 字节（SN 等）
bl     Drv_PloamuMic_Generate_Activation
bl     fi_Configure_Ponmac_Send_Ploam_Msg
```

**⇒ MsgID = 0x02、走软件 FIFO、我们用的帧格式是对的。**

### 88.3 MIC 走哪把钥匙取决于一个标志

```asm
ldrb w2, [x7, #1835]     ; 某一标志
cbnz w2, dd58            ; 非 0 → 另一分支（fake MIC）
...                      ; 为 0 → Drv_PloamuMic_Generate_Activation（激活密钥）
```

首次注册（密钥还没协商）应该走 fake（`0x55×16`），我们用的一致。
`setRegistrationMIC` 的调用者是 `p_o2_3_assigned_onu_id_msg` —— 那是**收到 ONU-ID 之后**的密码上报，不是 discovery。

### 88.4 结论

到这一步，软件侧能确认的全确认了：

| 项 | 状态 |
|---|---|
| 帧格式（ONU-ID/0x03FF + MsgID 0x02 + SN + MIC） | ✅ 与原厂一致 |
| 发送通道（软件 FIFO `Send_Ploam_Msg`） | ✅ `sent` 计数递增 |
| MAC 配置（6144 个 + SerDes 22 个） | ✅ 与原厂一致 |
| 上行使能 `0x17059800` bit1 | ✅ =1 |
| MIC key | ✅ fake 0x55×16 |

**软件侧已经没有可改的东西了。**
剩下的只能是**上行突发在模拟侧真的没出去**（激光/时序/图案），或者 **OLT 侧不接受这个 SN**。

### 88.5 v9.36 的净收益

`0x10e40058` 从 `0xB3E23333` 改成原厂的 `0x03E23333` **保留了下来**
（`CRM_AUX` 已回退，见 §87）。这一处是确定的修正，虽然不影响发送结果。

---

## §89 ★★★ 真相：帧根本没发出去——FIFO 一直卡着，硬件没有 grant（2026-10-19 10:30）

### 89.1 决定性观察

`0x17059908`（XMAC 0x16642，TX FIFO 状态）**一直读回 `0x1`**。

`pon_up_load()` 的流程是「等 `0x17059908 == 0` → 写 48 字节到 `0x17059b08` → 写 `1` 触发」。
但等空这一步**每次都超时**（1000 × 10 µs = 10 ms 内没等到），所以：

- `pon_send` 返回 `-ETIMEDOUT`
- 帧**根本没进 FIFO**
- `sent` 计数只在 `pon_up_fire()` 里 `++`，那只是软件计数，**不代表硬件发过**

⇒ **§78/§83 里那些「sent 递增说明进了发送通道」的结论全部作废。一帧都没出去。**

### 89.2 为什么 FIFO 不空：没有上行 grant

XG-PON 的 ONU **只能在 OLT 在 BWmap 里分配的 grant 时隙发送**。
`0x17059908` 的 bit0 表示"FIFO 非空/发送未完成"——硬件一直不消费，
说明 **MAC 从来没拿到过 grant**。

这一条把所有反常现象都解释通了：

| 现象 | 解释 |
|---|---|
| `0x170599bc` bit3 会清 | 硬件接受了"发 SN"请求，把帧装进 FIFO |
| `sent` 计数递增 | **纯软件计数**，与硬件无关 |
| OLT 一个字不回 | 光口上一个字节都没出去 |
| 改 bit1 / SerDes / 密钥 / MIC 都无效 | 因为根本到不了发射那一步 |

### 89.3 另一个异常位

`0x17000068`（4 个 bank 各一个）：我们 `0x61a8015C`，原厂 O5 `0x61a80150`
—— 差 bit2 / bit3，**疑似 TX 错误标志**（可能是 "no grant / TX timeout"）。
该寄存器可写（写入后读回就是写入值），清 bit2/3 后 FIFO **仍然**是 1
⇒ 不是错误标志把 FIFO 卡住的，是真的没有 grant。

### 89.4 下一步（真正的方向）

**不是**"怎么把帧写进去"，而是**"怎么让 MAC 拿到 grant"**：

1. **帧同步**：XG-PON 下行 125 µs 帧，MAC 必须先与 OLT 的帧头同步（PSync/超级帧计数），
   才能解析 BWmap。查 `zte_xgpon.dis` 里的 `sync` / `psync` / `bwmap` 相关函数。
2. **ONU-ID**：XG-PON 里 ONU 在未分配 ONU-ID 时用 broadcast(0x3FF) 接收 BWmap 里的
   discovery grant；确认 MAC 是否在 O1/O2 状态下仍解析 BWmap。
3. **`0x17000068` 那两位**：对比原厂 O1 时的值（本次没抓这一段），确认它们是不是
   "BWmap 已同步"标志。

### 89.5 本次临时写入（重启会丢，未固化）

`align_o1.sh` 的 27 个值（注册码区 `0x1705984c-5c`、8 个密钥 `0x170599d8-f4`、
`0x17059440`、`0x17059844`、`0x17059a08/0c/10/58/5c/64/6c/7c`）已写回内存，
**但这些必须固化进驱动**，否则每次重启都是 0。

---

## §90 继续深挖：SerDes 全序列反汇编 + 全量 6144 对比（2026-10-19 06:00–10:00）

### 90.1 `serdes_mode_set` 反汇编（已归档 `work/serdes_mode_set.txt`）

`plat-zxylzb_9131.ko` 里 `serdes_mode_set(mode,0,0)` 是 **8 路跳转表**（表在 `.rodata+0x13c8`）：

| mode | 入口 | 说明 |
|---|---|---|
| 1 | 0xd3b0 | |
| 2 | 0xd17c | 直接返回（空操作） |
| 3 | 0xd518 | `zx_pon_probe` 里 EPON 用 |
| 4/5 | 0xd188 | |
| 6 | 0xd298 | |
| **7** | **0xd048** | **GPON**（`gponsdk_initModule` 判 `g_pon_work_mode` bit9） |
| 8 | 0xd640 | bit10（另一制式） |
| 15 | 0xd17c | 空操作 |

`apb_bit_write(reg,value,width,bit)` 语义已确认：`addr = *pon_serdes_base + reg`，
读改写 `mask=((1<<width)-1)<<bit`。mode 7 分支（vid≠0x9c/0xaa）最终写的是：
`0x08/0x14/0x18/0x1c/0x20/0x24/0x34/0x4c` 八处，**全部落在我们已经对齐的 22 个非零字里**。

`reg_def_set()` 就是 §68 那 24 字表（0x00..0x5c），已实现；
`ref_clk_set()` 管 CRM+0x40 的 PLL 分频与 bit12 锁定轮询，**U-Boot 已配好，不用重做**。

⇒ **SerDes 这条路彻底走完了：没有遗漏的 TX 使能位。**

### 90.2 ★ 全量 6144 寄存器对比（原厂 O5 vs 现场）

新增能力：`pon_reg` 节点覆盖 0x17000000–0x17250000，可逐点读。
`work/diff_all_o5.py` 把 o5_regs.json 的 6144 个地址全比了一遍：

**不同 619 个**，按块分布：

| 块 | 数量 | 性质 |
|---|---|---|
| 0x17000000 | 28 | 4 个 bank 的测距/计数残值（0x94/98/a0/ac/b0/c4） |
| 0x17058000 | 318 | 240 项功率表 + 8 个中断路由字 + 计数/测距 |
| 0x17059000 | 202 | 127 项 allocmap2 + 静态 GEM 配置 |
| 0x170c0000 | 52 | 省电/定时器块（状态） |
| 0x17140000 | 16 | GEM port 表（要 OMCI 之后才有） |
| 0x17180000/0x171c0000 | 3 | 计数器 |

### 90.3 两张表已按原厂顺序铺好（现场验证）

原厂 `fi_power_init` 的顺序是 **先 0x16011=0x19999999，再 0x16010=0x04444444**：

- 写 `0x17058044 = 0x19999999` → 一次铺满 `0x17058044..0x170583fc`（239 项 0x09999999）
- 写 `0x17058040 = 0x04444444` → 只改首项

现场执行后读回 **与原厂 O5 完全一致**（0x04444444 + 0x09999999×239）。
第二张表 `0x17059200..0x170593f8`（127 项 0x9）也铺好。
⇒ **这两处差异消除，TX 仍然卡住。**

### 90.4 仍然解不开的核心现象

| 观察 | 值 |
|---|---|
| `0x17059908`（TX FIFO 状态） | 手工写 0 后 **1 秒内自己回到 1**，且永不清 |
| `0x170599bc`（SN 触发） | 是**纯软件锁存**，写 0 能清、清了不反弹；原厂 O5 = `0x1`（只有 bit0） |
| `0x17059400` | 我们 `0x03ff0003`（alloc 0x3ff / state 3），原厂 O5 `0x090005` |
| `tx_bias_ma`（DDM） | **0.00 mA** —— 激光器一次都没被点亮 |

**结论不变**：MAC 里躺着一帧待发的 PLOAM，硬件永远不消费它。
读得到的配置已经和原厂一模一样，差异只可能在**只写寄存器**或**物理层**。

### 90.5 BWmap 记录 RAM 的坑（重要，推翻 §82 的一条依据）

`0x17059404` 的 **bit17 是"重新武装记录"**：置 1 再清 0，记录会重来一遍。
不碰 bit17 时，512 条记录是**冻结**的，`/proc/ponbw` 读多少次都一样。

- 首次（bit16 由 Hw_Init 置位时）抓到的是 `70064000 / xx8303ff`（含 alloc 0x3ff）
- 置 bit17 重启后抓到的是完全另一组 `9fffc0c0 / 784dc40a`，**两次重启内容逐字节相同**

⇒ 这个 RAM 的语义还没搞清（可能是内置的测试图样发生器，不是真实 BWmap 抓包）。
**之前"OLT 一直在发 0x3ff discovery grant"的结论不可靠，需要用别的办法重新确认。**

### 90.6 仍存在的真实配置差异（未解）

- `0x17058010..0x1705803c`：中断路由表。原厂 `0x36db6cbe / 0x12 / 0x12 / 0x00 / 0xff×7`，
  我们 `0x324b24ba / 0x32 / 0x32 / 0x88 / 0x88×7`。
  原厂 `fi_Configure_Interrupt_Init` 用 **0x17058014=索引 / 0x17058018=数据** 的间接对，
  41 次写把表建起来；**我们的驱动完全没复刻这张表**（PLOAM 中断能用，但其余源可能没配）。
- `0x1705900c/18/28/2c/40/100/160`：静态 GEM/ETH 配置，要 OMCI 之后才有意义。
- `0x17140008/0c/10 + 0x104`（4 bank）：GEM port 位图（0xfffe）+ GEM port id，同上。

### 90.7 下一步的两个方向（都还没做）

1. **复刻中断路由表**：从 `fi_Configure_Interrupt_Init` 的 41 次 (索引,数据) 写里
   还原成一张表，塞进 Hw_Init replay。工作量中等，**不保证能解 TX**，但这是目前
   唯一还没对齐的可读配置。
2. **光功率计**：直接看触发 `0x170599bc` 时激光器有没有出光。
   没有光 ⇒ 问题在 BOSA/驱动电路；有光 ⇒ 问题在 MAC 调度。
   **这条能把"物理层 vs MAC"一刀切开，性价比最高。**

---

## §91 原厂对照：推翻 bit3 判据 + 找到两处运行态差异（2026-10-19 22:30–00:20）

### 91.1 ★★ `0x170599bc` bit3 在原厂也不会自清

在原厂固件（O5 在线）上直接 `devmem2 0x170599bc w 0x00000009`：
**写进去就是 `0x9`，等 1 秒、2 秒、45 秒都不变。**

⇒ 之前「bit3 不回清 ⇒ SN 突发引擎从未发出去过」这条推理**作废**。
   这个位在原厂同样长期保持 1，不能当发送成功/失败的判据。

### 91.2 ★★ 手工 FIFO 发 SN 在原厂也不管用

把原厂强制退到 O1（`0x17059400=0x03ff0001`），再用我们驱动的
「O1 → 清 59908 → 灌 12 字 0x17059b08 → 切 O3」流程手工发一帧
（帧用原厂 O5 快照里的原样 48 字节 + 原厂 MIC `66e17d0e/35d7c55f`），
**状态 24 秒稳定停在 `0x3ff0003`，OLT 没有任何回信。**

⇒ 「O1 装帧 → 切 O3」只是让 `0x17059908` 不再反弹，**并不等于真的把帧发了出去**。
   这条路单独走不通。

另外：原厂被强制退到 O1 后，**它的驱动不会自己重新注册**（45 秒都没动），
必须重启才会回 O5。所以「原厂 O1 采样」只能拿到静态值。

### 91.3 ★★ 原厂 O1 / O5 实测值（telnet devmem2）

| 寄存器 | 原厂 O5 | 原厂 O1（强制） |
|---|---|---|
| 0x17059400 | 0x00090005 | 0x03ff0001 |
| 0x1705947c | 0x03ff0000 | 0x03ff0000 |
| 0x17059800 | 0x00000016 | **0x00000016**（不是 0x15） |
| 0x17059908 | 0 | 0 |
| 0x170599bc | 0x1 | 0x9（被我们写过） |

⇒ **注册阶段上游使能就是 0x16**，`Set_Upstream_Enable(0)→0x15` 那条推断是错的。

### 91.4 ★★ 两处运行态差异（已对齐进驱动 v9.58）

| 寄存器 | U-Boot 默认 / 我们旧值 | 原厂运行值 |
|---|---|---|
| GPIO bank3 DATA (0x10d100d4) | 0x0b | **0x08** |
| SYSCTRL 0x10e0004c | 0x94 | **0xB5**（多 bit0/bit5） |

- GPIO bit0/bit1 **不能用 DATA 直写**（写完回读仍是 0x0b），必须写
  **CLR `0x10d100dc` = 0x3** 才清得掉。原厂初始化就是清掉的。
- 两处都已写进 `zx279131_pon_board_align()`，开机日志确认
  `gpio bank3 dir=0x1b data=0x08`、`sysctrl 4c 0x94 -> 0xb5`。

### 91.5 `0x1705947c` bit30 是自置位，不是我们写错

驱动里写 `0x03ff0000`，运行中回读是 `0x43ff0000`；清 0 再写一次立刻回读
仍是 `0x03ff0000`，一秒后也还是。⇒ bit30 由 MAC 在「搜索 SN 窗口」时自己置起。
**它不是配置错误，别再改。**

### 91.6 v9.58 现场

- `gpio bank3 data=0x08`、`sysctrl 4c=0xb5`、`0x17059800=0x16`、state=O3。
- 每 2 秒一轮 O1→O3 发 SN，跑满 300 轮：`msgid_hist` 仍只有 `01`（约 1 条/秒），
  `02/03/05` 全 0。
- `0x17059908` 常读 0，`0x17059990`/`0x17059998` 恢复成非零计数。

### 91.7 下一步（真正还没做的）

1. **光功率计**：唯一能把「激光有没有出光」和「MAC 不调度」切开的手段。
2. **让原厂带串口日志启动**：U-Boot 里 `printenv bootcmd` 拿到原厂启动命令，
   改写 bootargs 加 `console=ttyAMA0,115200` 再 `bootm` 原厂内核，
   就能在串口上看到 `zte_xgpon` 注册全过程的 printk（现在被厂商静音了）。
   这是不拆机、不加硬件就能拿到「原厂 O1→O5 每一步」的唯一办法。

---

## §92 U-Boot 环境全部读出来了 + 一个坑（2026-10-20 00:20–00:50）

### 92.1 原厂 U-Boot 环境（`printenv` 原文，已验证）

```
bootcmd=setenv bootargs console=ttyAMA0,115200n8 earlyprintk=pl011,0x10d0d000
        swiotlb=128 rdinit=/sbin/init  mem=512M;bootm 0x88000240;
boota=setenv bootargs console=ttyAMA0,115200n8 earlycon=zteuart,mmio32,0x10d0d000
      root=/dev/mtdblock7 ro rootwait; nand read 0x88000000 0x3680000 0x1A80000
gatewayip=192.168.1.1 serverip=192.168.1.100 ethaddr=00:41:71:00:00:50
ver=U-Boot 2016.01-rc3 (Aug 29 2022)
```

命令表里**没有 `boot`**，有 `bootk <name>`、`bootzpon [debug]`、`run`、`nand`。

分区表（内核打印出来，13 个）：

| 分区 | 起止 |
|---|---|
| bootrom | 0x0–0x20000 |
| u-boot | 0x20000–0x100000 |
| others | 0x100000–0x180000 |
| parameter-tags | 0x180000–0x280000 |
| wlan | 0x280000–0x380000 |
| usercfg | 0x380000–0x580000 |
| middleware | 0x580000–0x680000 |
| **kernel1** | **0x680000–0x3680000** |
| **kernel2** | **0x3680000–0x6680000** |
| framework1/2 | 0x6680000–0x8680000 |
| preplugin_data | 0x8680000–0x9e80000 |
| plugin_data | 0x9e80000–0x10000000 |

### 92.2 ★★ 坑：`run boota` 启动的是 **kernel2 里一份我们的旧镜像**

`run boota` 从 **0x3680000（kernel2）** 读 0x1A80000 到 0x88000000，
读出来的 FIT 头里写着 `F7610U kernel+initramfs (UART fixed)` —— **是我们自己的旧镜像**
（Data Size 27748360，比现在的 30111752 小），不是原厂内核。

用 `bootm 0x88000000` 启动它，配上 vendor 的 bootargs（`root=/dev/mtdblock7`），
**卡死在 `zx279133-sfc 10d0f000.spi`，串口静默、ping 不通、DTR/RTS/串口 BREAK 都救不回**，
只能物理断电。

⇒ 以后**不要再 `run boota`**。原厂正常的启动路径是 `bootzpon`（或它内部调用的 bootcmd），
   kernel1 在 0x680000，`bootm 0x88000240` 那个 +0x240 偏移是另一套加载器留下的。
⇒ 另外：kernel2 里有我们自己的旧镜像这件事本身要记住，别把它当成"原厂内核"。

### 92.3 恢复后第一件事

断电重启 → 停在 U-Boot → `bootzpon`（原厂默认路径）确认原厂能起；
之后要加串口日志，正确做法是 **`bootzpon` 前 `setenv bootargs` 注入 console**，
而不是自己 `nand read` + `bootm`。

---

## §93 ★★ 打开原厂内核日志（本轮最大突破）+ pin_mux 差异（2026-10-20 00:20–01:10）

### 93.1 原厂启动路径确认：`bootzpon`

U-Boot 里 **没有 `boot` 命令**。正确路径是 `bootzpon`：
它从 middleware 分区读索引，定位到 **kernel1 = 0x680000 + 0x240**，
`RSA Verify OK` 后 bootm 0x88000240，内核是 gzip 的
"Unify(TODO) Linux kernel for project-131G"（load 0x80080000）。
`run boota` 读的是 **kernel2（0x3680000）**，那里是我们自己的旧镜像 —— **别再碰**。

### 93.2 ★★ `/proc/kmsg` 被 klog 守护进程抽干，但 `syslog(2)` 能拿到全量

- `/proc/kmsg` 读出来是 0 字节（vendor 的 Klogstdio 一直在读）。
- busybox 没有 `dmesg` applet。
- 但 `syslog(SYSLOG_ACTION_READ_ALL)` 不受影响 —— 写了 60 行的
  `work/klogdump.c`，用 OpenWrt 工具链静态编译（66 KB），丢到 `tftp/klogdump`，
  设备上 `tftp -g -r klogdump -l klogdump 192.168.1.100` 就能拉下来跑：

```
cd /tmp && ./klogdump > /tmp/k.txt      # 734 行，整个内核启动日志
grep -i pon /tmp/k.txt
```

**这是以后看原厂内部行为的通用手段**（`/proc/log/{file,serial,telnet}` 只是统计计数，
不是日志内容）。

### 93.3 原厂 PON 初始化日志（原文）

```
pon init
the pon mode is 200
pon_reset_blk: val = 0x1ff81ff
pon init:SQMG and QMG
enter ref_clk_set.. mode = 7 .
enter ref_clk_set PON_SERDES_MODE_NGPON2_NSYM!
ref_clk_set finished!
pon mode changed!
...
pon up
enter ref_clk_set.. mode = 7 .
enter ref_clk_set PON_SERDES_MODE_NGPON2_NSYM!
ref_clk_set finished!
DEbug: GPON PIN_MUX_REG3:(0xfc)
gpon_Init ok
omci up
...
o1-o2-3
o2_3-o4
==DG_EN at O5
o4-o5
```

- `the pon mode is 200` = `g_pon_work_mode` bit9 → `ref_clk_set(mode=7)`，和 §90 的结论一致。
- 注册全过程只有 4 行日志，**没有任何寄存器级 printk**；而且是事件驱动的
  —— 把 59400 强改成 O1 后，驱动**一声不吭**，也不会自己重注册。
- `pon_reset_blk: val = 0x1ff81ff`：某个基址 +0x4c 的旧值，尚未定位到具体寄存器
  （0x1700004c / 0x10e0004c / 0x10e2004c / 0x10e3004c / 0x10e4004c 都不是这个值）。

### 93.4 ★★ pin_mux 差异（`fi_Configure_Init` 里的 GPON 专用设置）

反汇编 `fi_Configure_Init`（0x2140）在 0x21b4–0x21d0：

```
w1 = *(pin_mux_base + 0x0c);
w2 = (w1 & 0xfffffff6) | 0x8;     // 清 bit0/bit3，置 bit3
*(pin_mux_base + 0x0c) = w2;
printk("DEbug: GPON PIN_MUX_REG3:(0x%x)", w2);
```

现场对照（0x10e20000 起 5 个字）：

| 寄存器 | 原厂 | 我们（U-Boot 默认） | 处理 |
|---|---|---|---|
| 0x10e20000 | 0x001995ff | 0x001995ff | 一致 |
| **0x10e20004** | **0x0154dd75** | **0x0010dd75** | 差 bit22/bit24，**尚未写进驱动** |
| 0x10e20008 | 0x000fefff | 0x000fefff | 一致 |
| **0x10e2000c** | **0x000000fc** | **0x000000fd** | 已在 v9.59 的 `board_align()` 里对齐 |
| 0x10e20010 | 0x000020ec | 0x000020ec | 一致 |

实测写这两个 pin_mux 都**不会断网**（0x10e20004 也现场试过 0x0154dd75，SSH 正常）。

### 93.5 v9.59 现场

- 开机三行对齐日志全部出现：gpio bank3 data=0x08、sysctrl 4c→0xb5、pin_mux3 0xfd→0xfc。
- 每 2 秒一轮发 SN，`msgid_hist` 仍只有 `01`。
- ⇒ 光层/引脚层的可读、可写配置已经全部对齐到原厂，**TX 依旧不出**。
  下一步只剩：光功率计，或把 0x10e20004 也写进去再试，或直接测 BOSA 的 TX_DISABLE 脚电平。

---

## 94. 反汇编 zte_xgpon.ko：SN 发送黄金路径 + v9.60（2026-10-20）

### 94.1 setSnMIC —— O2_3 硬件自动发送的完整装备序列（根因）

`setSnMIC`（ko 0xe058）由 `p_o2_3_profile_msg`（0xe258，每个 Profile 消息一次）调用。
四步，缺一不可：

| 步 | 动作 | 地址 |
|---|---|---|
| 1 | SN（大端 rev） | 0x170598d8 / 0x170598dc |
| 2 | MIC = CMAC(0x55×16, 0x02‖body[40])，body=[03 ff 01 00][SN 8B][delay 4B][zeros 24B] | 0x170598f8 / 0x170598fc |
| 3 | header 顶字节打 0x03（mode 1；O1 期 0x00） | 0x170598f4 |
| 4 | **bit0 = SN MIC 就绪** | 0x17059844 |

- 原厂 O5 活体 0x17059844=**3**（bit0+bit1）；我们的周期重放路径长期只有 **2**。
  ⇒ 这就是「所有可读配置都对齐了 TX 还是不出」的根因：**bit0 不置位，硬件 SN 突发引擎不武装**。
- 随机延迟：`fi_Configure_Ponmac_Set_Random_Delay`（0x6880）算值存 g+0x264D，并把
  `&0x1ffff` 镜像到 0x1705940c；硬件按 0x1705940c 拼 body，所以我们读该寄存器算 MIC 正确。
- 密钥：`generate_key`（0xbfb8）就是 **0x55×16**（不是动态生成的）。

### 94.2 fi_Configure_Ponmac_Send_Ploam_Msg —— O5 软件重注册路径（0x4c90）

1. 轮询 0x17059908 bit0 等清（最多 16 次）
2. 12 个大端字（rev）写 0x17059b08..0x17059b38
3. 写 1 到 0x17059908（启动）
4. 轮询等 bit0 自清；清了返回 0，超时返回 0x1022

- **不碰状态寄存器**。厂商 O2_3 稳态也从不写 0x17059400；我们旧代码每 2 秒强制
  O1→O3 是把硬件状态机反复打回 O1，SN 引擎永远 arm 不了（v9.60 已删）。
- 0x170599bc 是老路（b0 使能 / b2:1 onu_id / b3 触发），bit3 原厂也不自清，别再用。

### 94.3 O1→O2_3 转换链（p_o1_clear_los_or_lof_event 0xcfa0）

- 门：0x17058414 bit4 必须清（我们活体 0x02 ⇒ 过）；软件缓存状态须为 O1。
- 动作：`fi_set_ONU_activation_state(3)` + `fi_Configure_SN_AllocId_Set(1)`。
- `fi_set_ONU_activation_state`（0xcad0）确认 0x17059400 布局：
  bits[4:0]=状态（1=O1 / 3=O2_3 / 4=O4 / 5=O5），bits[25:16]=ONU-ID（0x3ff=未注册）。
- AllocId_Set 按模式写 0x1705947c 的 14 位字段（0x3ff/0x3fe/0x3fd）；我们字段已是
  0x3ff，与 O5 一致，非瓶颈。

### 94.4 O2_3 稳态（p_o2_3_profile_msg 0xe258）

Set_Profile → setSnMIC → `Upstream_Speed_Para`（mode=0 时 0x17059408=**0x2a86**，已一致）
→ generate_key → 条件性 `Set_Upstream_Enable(1)`（0x17059800 bit1；我们 0x16 已是使能态）。
`p_o2_3_sn_request_event` 只打日志 ⇒ **SN 发送纯硬件行为，软件只负责装备**。

### 94.5 MIC 验证更正（工具 bug，非内核问题）

- 自写 CMAC 第一版补丁写成 `blk[-1]=0x80`，正确应为 `blk[len(last)]=0x80`；该 bug 让
  Python/Node 与内核不一致，差点误判 cmac-aes-neon。修正后三方一致，且
  CMAC(0x55×16, 0x02‖body, delay=0xade5) = 66e17d0e35d7c55f = O5 快照值，
  **驱动"O5 MIC == plain CMAC"的原结论成立**。工具存 `work/gpon_mic.py`（带自测向量）。

### 94.6 v9.60 补丁与实测

补丁（zx279131-pon.c）：
- `gpon_sn_auto` = setSnMIC 四步全套（SN/MIC/header/bit0），删 FIFO+状态翻转；
- `gpon_init_work` 开机做一次 O1→O2_3 跳变；
- 产物 `tftp/f7610u-v9.60-snmic.itb`。

实测（~4 分钟）：
- `enter O2_3, state=0x03ff0003`、`sn mic refresh ready=0x03 hdr=0x03000000` 全部生效；
- **OLT 仍只回 01**；0x1705947c=0x43ff0000 ⇒ **bit30「SN 窗口搜索」挂着不清**
  （原厂 O5=0x3ff0000 已清）；0x17059988 随 seq 上涨（计数器，语义未定位）。
- TX 区 0x17059800-0x17059c00 对 O5 仅 62 处差异（计数器/注册后状态为主）。

### 94.7 下一步

1. bit30 挂着 ⇒ 重点查 OLT 的 BWmap 有没有给 discovery（Alloc-ID 0x3ff）窗口：
   抓 0x17059404/0x17059428 记录 RAM 重新取证（旧"0x3ff grant"结论已作废）。
2. 硬件是否真把突发打出去：DDM 不可信，需光功率计/示波器。
3. bank1/2/3 profile（0x1f0408）与 O5 形态差异是否有害。
4. 0x17059988/seq 计数器语义（可作 TX 尝试判据）。

---

## 95. ★★★ 突破：OLT 分配 ONU-ID=9，注册走到 O4（2026-10-20 凌晨）

### 95.1 两个真正的根因（此前全部精力都花在 MAC 配置上，方向错了）

**根因 A：激光器从来没开过。**
- 光模块 A2 页在 I2C 7bit 地址 **0x51**，寄存器 **0x6e bit6 = TX_DISABLE，上电默认 1**。
- 厂商 `hw_power_optx_set(1)`（bspdriver.ko 0x5d40）：`bob_i2c_writeb(0x51,0x7f,6)`（模块密码）
  → 读 0x6e → `and ~0x40` → 写回。我们的驱动完全不碰 A2。
- 现场：`/tmp/i2cprobe 0 wr 0x51 0x7f 6; /tmp/i2cprobe 0 wr 0x51 0x6e 0x00`，
  DDM 立即真实：tx_bias 9.4mA / tx_power +0.7~0.9dBm（此前恒 0，就是激光没开）。
- 工具要点：0-0051 被 at24 驱动绑定，I2C_SLAVE 报 EBUSY；at24 的 eeprom sysfs 读出全 0
  （不可信）。必须用 `work/i2cprobe.c` 的 I2C_RDWR 原始消息路径。

**根因 B：SN 突发缺少最后一下"扣扳机"。**
厂商 `fi_Configure_Ponmac_Send_Ploam_Msg`（0x4c90）：
1. 轮询 0x17059908 bit0 等 0；2. 12 个大端字写 FIFO 0x17059b08..0x17059b38；
3. **写 1 到 0x17059908**；4. 轮询等 bit0 自清。
我们旧代码只写过 0、从没写过 1 ⇒ 硬件从未收到发送请求。FIFO 内容就是
`gpon_build_sn_frame()` 那 48 字节。硬件"O2_3 自动发送"路径（bit0/header 全设好）依然不发，
**软件 FIFO 路才是活的**。

### 95.2 msgid 03 = Assign_ONU-ID（旧注释"03=Request_Password"是错的）

- 状态机处理器表在 .data（.rela.data 全是 p_o* 指针）：7 行×29 列、行基址 0x28E8、行距 0xE8；
  `p_operation_state_machine(event,msg)` 查表 `[state][event]`。
- ISR `fi_onu_stack_mngt_isr_api`：msgid3 → 事件3。**O2_3 行事件3 = p_o2_3_assigned_onu_id_msg**。
- 报文：`[03 ff 03 seq][ONU-ID(2B)][SN回显(8B)][...][MIC(8B)]`。
  实测：`03ff031e 0009 534b5957a8d40ba7 ...` ⇒ **ONU-ID = 9**（与 O5 快照 0x090005 吻合）。
  OLT 每收一条 SN 回 3 个 03（重传），累计 18 个。

### 95.3 O2_3 → O4（已现场完成）

`p_o2_3_assigned_onu_id_msg` 流程：校 SN 回显 → 取 ONU-ID → setRegistrationMIC +
setAckMIC → Create_Omci_Channel → `fi_set_ONU_activation_state(4)`。
现场实操（后两步的全局量未知，暂跳过）：**写 0x17059400 = (9<<16)|4 = 0x00090004**，
状态稳定在 O4。0x17059400：bits[4:0]=状态、bits[25:16]=ONU-ID。

### 95.4 完整注册链路（反汇编 + 实测拼齐）

```
O1 --(LOS/LOF 清除, 0x17058414 bit4 须清)--> fi_set_ONU_activation_state(3)+AllocId_Set
O2_3 --每个 Profile 消息--> Set_Profile + setSnMIC(SN/MIC/header=0/0x17059844 bit0)
                            + Upstream_Speed_Para(0x17059408=0x2a86) + Set_Upstream_Enable
     --发 SN(FIFO+59908=1)--> OLT 回 Assign_ONU-ID(03, ONU-ID=9)
     --> 写 0x17059400=(9<<16)|4
O4  --(硬件自动发 ranging 突发, p_o4_ranging_request_event 只打日志)-->
     OLT 回 Ranging_Time(0A) --> 填 EqD(0x17059410) --> O5
```
未走完/未验证：O4→O5（等 0A）、setRegistrationMIC/setAckMIC/OMCI channel 三件套
（依赖未知全局量 g+0x2625/0x2635/0x2645）。

### 95.5 其他确认
- mode>3（0x17000018 低4位=3、0x1705A238=0x20、0x17058474=0xfffff）⇒ header 字节应 0 不 3。
- BWmap 记录 RAM：OLT 持续发 discovery 授权（alloc=0x3ff、PLOAMU=1），从来都有。
- 报文环 pon_ploam_all 仅 8 条，03 稍纵即逝；下轮补丁：加大 ring + 非 01 msgid 直接 printk。

### 95.6 现场复现操作（设备当前在 O4）
```sh
# 激光（开机掉电后要重做）
/tmp/i2cprobe 0 wr 0x51 0x7f 6; /tmp/i2cprobe 0 wr 0x51 0x6e 0x00
# 发 SN
P=/sys/devices/platform/soc/18000000.pp/pon_reg
echo 0x17059908 0x0 > $P
for w in 0x03ff0100 0x534b5957 0xa8d40ba7 0x0001cde3 0 0 0 0 0 0 0x0781fb31 0x6b79cd29; do ... 写 0x17059b08+i*4; done
echo 0x17059908 0x1 > $P
# 进 O4（拿到 03 报文里的 ONU-ID 后）
echo 0x17059400 0x00090004 > $P
```

---

## 96. 第二轮现场（2026-10-20 上午，接手 F7610U_PON上行_任务交接.md）

### 96.1 过程中两起事故（都已恢复）
1. **全量 dump 6144 寄存器把设备搞重启**：用 `zx_peek`+`dmesg -c` 循环读 0x17058000..0x1705E000，
   中途设备重启回落原厂固件（bootzpon，我们的 ITB 是 U-Boot 运行时加载没写 flash）。
   根因（驱动注释 2117 行白纸黑字）：**/dev/mem 直读未映射地址会挂总线、整板掉电无看门狗**。
   ✅ 恢复：`ub_boot.py grab`（telnet root/Zte521 reboot -f + 串口抢 U-Boot）→ `uboot_load.py f7610u-v9.60-snmic.itb`。
   ✅ 安全读法：`pon_reg` 节点（带范围检查，`echo addr > pon_reg; cat pon_reg` 直接回读）。
2. **i2cprobe 重启后丢失**（/tmp 是 tmpfs）：已用 immortalwrt 工具链重新交叉编译
   （`work/i2cprobe`，静态 aarch64），scp 上设备。激光重开成功（tx_bias 8.83mA / +0.75dBm / rx -20.5dBm）。

### 96.2 反汇编补齐的注册链（zte_xgpon.ko）
- `p_o2_3_assigned_onu_id_msg`（0xe590）：校 SN 回显 → ONU-ID（msg[4..5] 大端）→ setRegistrationMIC
  → setAckMIC → Create_Omci_Channel → `fi_set_ONU_activation_state(4)` → `fi_pon_timer_enable`。
- `fi_set_ONU_activation_state` 的 O4 分支（0xcc88）：0x17059400 写 ONU-ID → **0x1705943c = bit14|ONU-ID**
  → 状态=4 → **`fi_Configure_SN_AllocId_Set(0)` → 0x1705947c = 0x03ff0000**（bit30 清）。
- O5 分支（0xcd78）：ONU-ID → 状态=5 → **0x17000024 bit16 置 1**。
- `setRegistrationMIC`（0xdf80）：MIC → **0x17059878/0x1705987c**。
- `setAckMIC`（0xe4a0）：MIC → 0x1705984c/0x17059850 + 0x17059844 bit1（活体已是 3，无需动）。
- `fi_Congigure_Creat_Omcc_Tcont`（0x3e0）：0x1705943c（同上）。
- `Create_Omci_Channel` 尾部：**0x17059200 = (val&0xffff0000)|ONU-ID**；GEM port 创建写 0x17059204..10。
- `fi_Configure_Ponmac_Set_Eqd`（0x5b70）：EqD 编码变换后写 0x17059410（位域 [25:23]/[22:18]/[17:0]）。
- O4 收 0A（`p_o4_ranging_time_msg` 0xf488）：**EqD = msg[5..8] 大端 4 字节** → Set_Eqd → 读 0x17058424 →
  send_ack_msg(msg[3]) → 状态 4 就直接 `fi_set_ONU_activation_state(5)`。
- ISR（fi_Interrupt_PonMAC_Handle）：**IRQ 状态位 bit4 = rg_req_irq → event 6**（O4 ranging 授权中断）；
  O4 event6 = p_o4_ranging_request_event（只打日志 + 计数，突发纯硬件）。
- `p_o4_timer_to1_expire_event`（0xd7d0）：O4 超时 → reset + 回 O2_3 重发 SN（ranging 超时保护）。
- `ptrFunc_turn_on_the_transmitter_upstream`：平台层激光器电源回调（O7 恢复路径），与 ranging 无关。

### 96.3 现场实测（本轮全部结论）
- 驱动默认 SN = `SKYWa8d40ba7`（= 突破当晚 OLT 认的 SN，源码 1181 行）。
- 0x1705940c 延迟 = 0x0001cde3，与硬编码 MIC 匹配 ✓；0x17059844=3、0x170598f4=0x03000000。
- **重启后 OLT 对同 SN 零响应**：O2_3 + 硬件自动 SN（bit30=1，tx88 ~1/3s）+ 手动 FIFO 扳机 7 次，
  20 分钟 03:0。BWmap dump（pon_bwmap dump）：**128/560 条全是 alloc=0x3ff 发现窗口**，从不发 alloc=9。
  〔勘误：见 §97——本条结论成立，但"dump 统计"要用单次口径（`tail -512`），否则 dmesg 累积会虚增。〕
  判断：OLT 早给该 SN 分过 ONU-ID=9 但 ranging 未完成 → 对该 SN 进入沉默（不重发 03、不给 ranging 窗口）。
- 按厂商 O4/O5 序列全量复刻并验证：0x17059400=0x00090004/5、0x1705943c=0x4009、0x1705947c=0x03ff0000、
  0x17059878/7c=0x1ee5fff2/0xc1106b3a（注册 MIC，取 O5 快照）、0x170598f4=0、0x17059200..10=9、
  EqD=0x2046d9f、0x17000024=0x100c8。**OLT 依旧全面无视**（无 alloc=9 授权、无任何 ONU 级报文）。
- SN/TX 区（0x17059800-0x170599a0，104 个）活体 vs O5 快照仅 15 处差，已全部对齐/解释：
  计数器（59980/59988/5998c/59990/59994/59998/5999c）、SN MIC（随 delay 变，非差）、
  profile p1k0/p1k2/p2k3（OLT 轮换未刷到，活动 bank p0 与 O5 完全一致）。
- **硬件在 O4 不发 ranging 突发的根因仍未定位**：寄存器层已与厂商 O4 一致，但 tx88 冻结；
  OLT 不给 ranging 窗口（死结）。下一步方向：OLT 侧沉默/冷却（等）＞ 继续挖 ranging TX 闸门。
- 现场姿态（监视中）：已切回 O2_3 + bit30=1（自动 SN 重武装）+ 手动扳机，等 OLT 重新发现。
  监视日志 `work/watch_olt.log`（45 分钟，命中 03/0A 自动抓现场）。

### 96.4 v9.61 状态门补丁 + OLT 回应解码（2026-10-20 下午）
**补丁**（zx279131-pon.c `gpon_sn_auto`）：只在 O1/O2_3 武装 SN 引擎，O4/O5 直接返回
（照厂商 p_o4/o5_profile_msg 行为 + §95.5 header 规则）。产物 `tftp/f7610u-v9.61-stategate.itb`，已刷机。
**手动 O4/O5 寄存器状态不再被驱动覆盖**（v9.60 时每 0.5s 被 header=0x03 重武装）。

**干净 O4 姿态**（v9.61 下设置后稳定）：
```
0x17059400=0x00090004  0x1705943c=0x00004009  0x1705947c=0x43ff0000
0x17059844=0x00000002（disarm SN 引擎）  0x170598f4=0（header 清 0）
0x17059878/7c=0x1ee5fff2/0xc1106b3a（注册 MIC）  0x17059200..10=0x9
```
⇒ ranging 突发（tx88 ~2.5s/次）后 **30-70 秒，OLT 回应**：
- `03ff03XX 0009 [SN 8B]` ×3 = Assign_ONU-ID（**ONU-ID=9**）
- `000904XX 0100082c 03000000` ×1 = **Deactivate_ONU-ID**（厂商枚举序位确认）
- `000905XX 0000…[MIC]` ×3 = **Disable_SN**
时序：assign → ~70s → deactivate → disable-SN → **之后彻底沉默**（8 分钟 O2_3 重发 SN 零新报文）。
厂商行为：O4 收 04 → O1 → O2_3 重注册（p_o4_deactivate_onu_id_msg 0xd2a8）。

**判断**：OLT 等到了 ranging 但不认（~70s 超时后去激活）。**下一嫌疑**：burst profile
bank1/2/3 活体是静态默认档，原厂 O5 是 set-A 值（p1k0=0x00090404 / p1k2=0x00000000 /
p2k3=0xad4cc30f）——ranging 突发所用 bank 的定界符不对 ⇒ OLT 锁不上 ⇒ 超时。
验证：按 O5 快照手写 bank1/2/3 再打干净 O4，看是否回 0A（Ranging_Time）。

### 96.5 ★★★ msgid 04 = Ranging_Time（用户提供的 pon-debug 包解码，2026-10-20 上午）
**来源**：`~/下载/pon-debug-2026-10-18T19-36-42-324Z.tar.gz` = **515xg（Airoha AN7581，XG-PON）** 的调试包，
同一台华为 OLT（`olt_vendor_id=HWTC`）、**同一 SN `534b5957a8d40ba7`**、ONU-ID=9、
registration_id=`f6633702212`（11 字节；F7610U 的注册码是 `f663702212` 10 字节，两者不同）。
其 dmesg 有成功注册全程：
```
PLOAM: ONU-ID=9 ... entering O4
PLOAMd: dest=1023, msg-id=0x03 ×3            （Assign_ONU-ID）
PLOAM: absolute EqD=0x00082b0f; entered O5   （★ 拿到 EqD 直接进 O5，全程 ~200ms）
PLOAMd: dest=9, msg-id=0x04                  （O5 后仍在发的重测距）
```
**结论（修正 §96.4 的误判）**：
- **msgid 04 = Ranging_Time**（不是 Deactivate！），**EqD = msg[5..8] 大端 4 字节**，flags=msg[4]。
  我们收到的 04 载荷 `01 00 08 2c 03` ⇒ EqD=**0x00082c03**（与 515xg 的 0x00082b0f 只差 0xf4，测距噪声，同一段纤）。
- msgid 05 = Deactivate（我们去激活）；03 = Assign_ONU-ID（ONU-ID=9）。
- 厂商成功流程极快（assign→EqD→O5 ~200ms）；我们此前的「70 秒」是 OLT 在**等我们响应 Ranging_Time**，
  我们没理 ⇒ 超时去激活（05）⇒ 重来。
- 注册应答（msgid 02，[ONU-ID 2B][02][RegID 36B][MIC 8B]，RegID=注册码 ASCII 补零）是**被动**的
  （只由 `p_o5_request_registration_msg` 调 `send_registration_msg`，即收到 OLT 06 才发）——我们从没收到过 06。
**v9.61 干净 O4 姿态实测**（SN 引擎 disarm `0x17059844=2`、header=0、注册 MIC、59200 区=9、bit30=1）：
08:02–08:24 成功引出 OLT **3 批** (03×3+04×1+05×3)，04 里带 EqD=0x00082c03；此后 **OLT 沉默
（3 次失败后退避）**：耐心循环×3、静默 4 分钟、自动响应×4（8 分钟）全部零回应，03/04/05 计数定格
18/6/18。教训：① grep "000904" 会误报（profile 消息 MIC 区含该字节串）——必须行锚定 `^#[0-9]+ 000904`；
② busybox sleep 不支持小数；③ sed -i 在工作区 fs 上 EBUSY，用 Edit 工具。
**O4 姿态机皇位**（下次 OLT 重新 engage 时直接可用）：`work/auto.sh`（clean O4→04→EqD+O5 循环，
EqD 首选原厂 O5 值 0x2046d9f）；`work/watch2.sh` 后台监视（2 小时，engage 即 dump 报文环）。
**EqD 写入值未定**：原厂 O5 = 0x2046d9f（同机同纤经验值，首选）；我的 Set_Eqd 变换实现无法自证
（round-trip 不符，勿迷信）；原始值 0x00082c03 备选。OLT 会持续重发 04（515xg 在 O5 里还收到），可迭代。

### 96.6 原厂复注册实锤 + ONU-ID 租约假设（2026-10-20 下午）

**原厂固件复注册（枪毙「OLT 拉黑」假设）**：切回原厂固件重启，内核日志当场
`the pon mode is 200 → o1-o2-3 → o2_3-o4 → o4-o5 → ppp0 up`。**OLT 完全健康，同 SN 随时可注册**。
此前「华为 OLT 失败退避 10–60 分钟」说法无实证，且沉默远超 60 分钟，已作废。

**原厂 O5 活体 dump**（zxr dump 经 tftp put 回宿主机，`work/fw_o5_live.json`）：
与 10-18 快照 386/416 一致，30 处 diff 全是计数器/统计/随 delay 变的 SN MIC。
配置寄存器全一致：`0x1705947c=0x3ff0000`（bit30=0）、`0x17059844=3`、EqD=`0x2046d9f`、
`0x1705943c=0x4009`、注册 MIC、59200 区=9。**原厂没有 dd/head，用 `tftp/zxr`（静态，`zxr dump <pa> <n>`）。**

**Set_Eqd 编码实锤**（反汇编 5b70 逐指令复刻，bash 算术版见 `work/eqd3.sh`/`eqd4.sh`）：
- flags bit0=1 ⇒ E_new = 报文 EqD 原值（替换）；bit0=0 ⇒ ±当前值（加减）。
- encode(E)：w0=E<<2; x2=(w0*0x1af83441)>>49; x3=((w0>>3)*0x1af83441)>>46;
  low18=(E>>1)-0x25f80*x3; out = low18 | (x2&0x1f)<<18 | ((E&1)*4)<<23。
- 04 报文 EqD=0x00082c03 ⇒ 编码值 **0x0205b681**（三候选：raw → encoded → 原厂 0x2046d9f）。

**ONU-ID 租约假设（解释全部沉默观测）**：
| 事件 | 宿主机时间 |
|---|---|
| 原厂注册（ONU-ID=9） | ~02:3x |
| 我们持续发 SN，OLT 不理 | 02:5x→03:1x |
| OLT engage（3 批 03/04/05） | ~03:2x（≈注册后 50 分钟） |
| 3 批失败后沉默 | 03:4x 起 |
| 原厂又注册一次 | ~05:0x |
| 现在（O2_3/O4 两姿态各 100s） | 仍 03:0/04:0 |
⇒ OLT 给 SN 持 ONU-ID 租约，租约期内等 ONU 自己回来，不理 SN 突发也不授 ranging 窗口。
预计当前租约 ~05:5x–06:0x 过期。

**本轮工具**：`work/eqd3.sh`（干净 O4 等 04→三候选，200s 无 04）；
`work/eqd4.sh`（O2_3/O4 交替+自动响应，单轮 ~4 分钟版已跑：全沉默）；
`work/watch3.sh`（设备端 60 分钟守 O4，04 来自动填 EqD→O5→验证 bwmap alloc=9，日志设备 /tmp/watch3.log）。
**坑**：设备无 pkill（用 ps+grep+awk+kill）；设备端脚本日志必须写 /tmp（宿主机路径不存在）；
`sleep 1.2` busybox 不支持；`sed -i` 设备可用（宿主机 btrfs 才 EBUSY）。

### 96.7 B 方案（LOF 逼去激活）失败 + 激光恢复延迟发现（2026-10-20 下午）

**B 实验结果**：关激光 5 分钟（tx_bias 8.83→0.00mA 确认真关）→ 恢复 → eqd4 两姿态各 100s：
**OLT 仍 03:0/04:0/05:0**。5 分钟 LOF 不足以让 OLT 去激活 ONU 9（或其 LOF→去激活定时器 >5 分钟）。
**加码**：`work/lof_test.sh`（设备端后台）＝关激光 15 分钟 → 恢复（等 15s 偏置爬升）→ eqd4 → watch3 自动响应。

**重要发现——激光恢复延迟**：TX_DISABLE 清除后 DDM tx_bias **不是立即恢复**（2s 时仍读 0.00mA，
~10s 后才到 11.67mA / +0.80dBm）。教训：关激光实验的「开灯」判据必须等 ≥10s 再采 DDM，
否则会误判「激光开不回来」。另：**tx88 计数器增长 ≠ 实际有光**（MAC 级计数，激光灭时照涨），
光链路判据只看 DDM（tx_bias/tx_pwr/rx_pwr）。

**当前光链路**（健康）：tx_bias 11.67mA / tx_pwr +0.80dBm / rx_pwr -20.51dBm / vcc 3.31V。
本次启动（05:1x 至今）激光全程正常，OLT 沉默与光学无关。

**未决外部因素**：515xg（同 SN、同 OLT、ONU-ID=9）若与 F7610U 在同一 PON 口且在线，
OLT 可能握持 ONU 9 不放——需用户侧确认 515xg 当前注册状态。

### 96.8 15 分钟 LOF 编排 + 515xg 排除（2026-10-20 傍晚）

- **515xg 当前离线**（用户确认）⇒ 「515xg 在同 PON 口握持 ONU 9」假设排除。OLT 沉默只能归结为
  它对**本 SN** 的 ONU-ID=9 租约/LOF 定时器未到期。
- ★★ **运维教训：设备端 setsid 后台脚本活不过 ssh 断开**（watch3.sh 跑 5 分钟死、lof_test.sh
  sleep 中途死，laser 差点没恢复——已手动救回）。**长任务必须宿主机编排**：宿主机脚本 ssh 进设备
  跑前台命令（单次 ≤10 分钟），循环由宿主机维持。
- `work/lof2.sh`（宿主机后台编排）：05:45 关激光 15 分钟 → 06:00 恢复（等 12s 偏置爬升）→
  循环跑设备端 `eqd4.sh`（O2_3/O4 交替 + 04 自动响应）最多 15 轮 ≈ 70 分钟，命中 SUCCESS 即停。
  日志 `work/lof2.log`。
- 若 15 分钟 LOF 仍不逼 OLT 去激活 ONU 9，则华为 OLT 的 LOF→去激活定时器 >15 分钟或该 PON 口
  配置了超长 ONU 保号，只能等租约自然过期（今早实测 ≈注册后 50 分钟）或 OLT 侧查。

## §97 bwmap 测量口径勘误（2026-10-20 上午，接手会话新增）

**背景**：接手会话在设备重启 ~2h 后复测，一度误报"OLT 在发 37 条 alloc=9、§96.5 结论错了"，
经三次交叉验证**证伪**——那是测量错误，不是现场变化。**§96.3/§96.5 的主线结论（OLT 基本不发
alloc=9、对 SN 沉默）仍然成立**。特此留档，避免下次重复踩坑。

### 97.1 两处测量错误（都是我犯的）
1. **`echo 1 > pon_bwmap` 静默失败**：该节点子命令只有 `rec`/`stop`/`dump` 三个字符串
   （驱动 `sscanf("%15s")`）。写 "1" 不匹配任何分支 → `-EINVAL`；busybox 有时只吐
   `write error: Invalid argument`，有时完全无输出 ⇒ 让人以为"触发了但没数据"。
   **正确：`echo dump > /sys/devices/platform/soc/18000000.pp/pon_bwmap`**。
2. **dmesg 累积**：每跑一次 `dump` 就追加 512 行 `bwmap[N] ...`。用
   `dmesg | grep bwmap | grep alloc=9 | wc -l` 统计的是**历史所有 dump 之和**，不是单次快照。
   单次口径必须 `dmesg | grep 'bwmap\[' | tail -512 | ...`（或先记 `dmesg | wc -l` 再算增量）。

### 97.2 正确口径下的实测（2026-10-20 上午，设备 O4，重启 ~2h）
- 单次 dump：**~505 条 `alloc=1023`（发现窗口）+ 仅 4 条 `alloc=9`**（占比 <1%）。
- 4 条 alloc=9 形态：`w0=70064000`（数据授权）。
- 下行 msgid 01 稳定（5600+，每 3-4s +4）；`0x17059400=0x00090004`（O4，ONU-ID=9）；
  EqD 寄存器 `0x17059410=0x02046d9f`（原厂 O5 值，已就位）；`0x1705943c=0x4009`（O5 形态）。
- 手工 FIFO 连发 SN 12 次（sent 33→45）：OLT **零 03 回应**；第 13 次起 FIFO 卡死
  （`txstat=0x1`，硬件发送请求不被消费）。
- `run 1`（SN 专用突发引擎 `0x170599bc`，先切 O2 再触发）：**tx_timeout=1**，同样发不出。
- 写 `0x17059400=0x00090005`：**坐住 ~20s 后掉回 `0x03ff0003`**（未注册）。
⇒ 与 §96.3/§96.5 一致：**OLT 对该 SN 沉默，alloc=9 稀少，O4→O5 无法自然完成**。

### 97.3 保留的开放问题（未变）
- alloc=9 是否真为"给 ONU 9 的常态授权"存疑：占比 <1% 且形态是 `w0=70064000` 数据块，
  也可能是发现窗口里的杂项。**不要再基于"OLT 在正常授权"做推断**，先按 §96.5 的
  "租约/沉默"模型走。
- 后续动作仍按 §96.8 的编排（宿主机 lof 循环 + 等租约过期）或「待决策」三条。

## §98 burst profile bank1/2/3 实测（2026-10-20 07:28–07:37，§96.4 嫌疑验证）

### 98.1 三处差异确认（设备 vs 原厂 O5 快照 work/fw_o5_live.json）
| 寄存器 | 设备（改前） | 原厂 O5 | §96.4 代号 |
|---|---|---|---|
| 0x17059894 (bank1 ctl) | 0x001f0408 | 0x00090404 | p1k0 |
| 0x1705989c (bank1 w2) | 0xaaaaaaaa | 0x00000000 | p1k2 |
| 0x170598b4 (bank2 w3) | 0xa56679e0 | 0xad4cc30f | p2k3 |

bank0 / bank3 / 0x17059428(=0xdd) 设备与原厂一致，无差异。

### 98.2 手写实验
- `zx_poke` 写三处到原厂值；**3 秒后复检保持、90 秒后仍保持** ⇒ 驱动的
  gpon_apply_profile_msg 不会覆盖（节流生效或此时消息 idx 不落这几格）。
- 干净 O4 姿态（disarm 0x17059844=2、header=0、0x17059400=0x00090004）
  + set-A bank，宿主机编排 `work/o4watch.sh` 观察 3 分钟（22×8s）：
  **03=15 / 04=1 全程零变化**，OLT 无新 engage。
- 期间设备状态稳在 O4（0x00090004 保持）。
⇒ 结论：bank 三处差异**已按原厂值修复并保持**；但 OLT 处于沉默期（无 03/04 批次），
单边配置变化无从验证。**下次 OLT engage（03 批次再现）时现场已是 set-A 形态 —— 这是
比 §96.4 之前所有尝试更完整的干净 O4 姿态**，届时若仍不发 04，则 bank 嫌疑排除，
矛头回到 §96.6 租约/OLT 侧。

### 98.3 附带观察
- 07:24 前后 msgid 03 从 12→15（OLT 间歇 re-assign ×3），但无 04 —— OLT 的
  assign→ranging 闭环仍不建立。
- 工具坑：宿主机 heredoc `bash -s` 直跑长脚本会无输出；改 nohup 后台 + 日志文件
  （/tmp/o4w.log）再回读，可靠。

## §99 设备挂死与恢复（2026-10-20 07:45–08:01）

- **07:45 设备硬挂**：o4loop 长循环启动后设备 192.168.1.1 二层不通（ARP FAILED），
  宿主机侧正常（USB 网卡 enx00e04c680a76=192.168.1.100 UP）。串口 15s 无输出。
  猜测：o4run.sh 的 set_o4 里某次 `pon_reg` 写触发（或长循环高频写寄存器）打挂 MAC。
  **教训：o4run/eqd4 这类"写状态+轮询"脚本不宜无间隔长循环。**
- **07:5x 用户断电重启 → U-Boot**（原厂 2016.01-rc3，提示符 `=>`）。
- **08:00 重新 tftp 加载 v9.61 并 bootm 成功**（`uboot_load.py f7610u-v9.61-stategate.itb`），
  设备 SSH 可达，`up 0 min`。
- 恢复后基线：`0x17059400=0x03ff0003`（O3）、SerDes `0x598`、SN=`534b5957a8d40ba7` 正常；
  **msgid_hist 全归零**（重启清零）；OLT 下行 01 稳定（44 条/40s，约 1 条/秒）。
- 工具：串口读用 `work/ub_boot.py`（纯 termios，无需 pyserial）；`/tmp/ub_boot.bin` 若被
  root 占需先 `sudo rm -f`。

## §100 全寄存器差分 + 软件 FIFO 扣扳机实测（2026-10-20 09:00–09:15）

> 本轮首次把「设备 O4 姿态」与「原厂 O5 活体」做了 **416 项全量差分**，
> 并首次真正跑通厂商 `fi_Configure_Ponmac_Send_Ploam_Msg` 的软件 FIFO 扣扳机。
> 结论：**静态配置无短板，发射通道能扣响，但每次都被硬件判错（err+6），OLT 零回应。**

### 100.1 全寄存器差分（`work/regdiff.py`，112/416 有差异）

方法：`zxr dump <pa> <n>` 逐块读 0x17058000..0x17059ffc（6144 字），
与 `work/fw_o5_live.json` 逐项比。**工具在设备 `/tmp/zxr`**（`tftp/zxr` 静态二进制，
`scp -O` 推上去，busybox tftp 拉取会 exit 127）。

差异分四类，**没有一类是配置短板**：

| 类别 | 代表寄存器 | 判断 |
|---|---|---|
| alloc-ID 表（整片） | `0x17058040..0x170580fc` dev=`0x4bef` / o5=`0x04444444`,`0x09999999` | O5 才铺，§77 已定「probe 阶段铺会破坏 MAC」 |
| 计数器/统计 | `0x17059988`(tx88) / `0x17059998`(err) / `0x17058420+` | 随行为变，非配置 |
| O5 专属密钥材料 | `0x170599d8..0x170599f4` dev=全0 / o5=8 字随机 | O5 后生成，结果非原因 |
| 状态/范围差异 | `0x17059400`(O4 vs O5)、`0x17059410`(EqD)、`0x1705947c` bit30、`0x17059844`、burst 统计 | 已逐条查过 |

★ 唯一值得记的新差异：**原厂 O5 `0x170599bc = 1`，我们 = 0**。
但 §79.1 实测原厂 O5/O1/O3 全程恒 `0x1`、bit3 一次没置过 ⇒ 这是**常驻使能位**，不是触发位。

### 100.2 ★★ 软件 FIFO 扣扳机：能扣响，但每次判错

厂商 `fi_Configure_Ponmac_Send_Ploam_Msg`（ko 0x4c90）反汇编逐指令复刻（完整序列）：

```
① 轮询 0x17059908 (idx 0x6642) 直到 0        最多 16 次
② 12 字写 0x17059b08..0x17059b34 (idx 0x66c2..) 每字 rev（大端）
③ 写 0x17059908 = 1                           ← 真正的发射请求
④ 再轮询回 0（最多 16 次）；返回码 0x1022/0
```

**实测（`work/fifosend.sh`，干净 O4 下连续 29 次）**：
- 每次 `0x17059908` 写 1 后**立刻读回 0** ⇒ 硬件接受了请求（不是 timeout）。
- 每次 `0x17059998` **精确 +6**（0x1248→0x124e→0x1254…），`0x17059988` +2。
- **OLT 全程零回应**：`hist=0/0/0`，`pon_ploam_all` 里没有任何 03/04。
- `0x1705947c` 始终 `0x43ff0000`（**bit22=1 = 未注册**，§79.1 判定法）。

⇒ **帧确实推出去了，但硬件把它记成错误，且 alloc 位始终未变。**
⇒ 新结论：**问题不在"会不会发"，而在"发的时候有没有落在 OLT 给的 discovery 时隙"**。
   `tx88` 增长 ≠ 被 OLT 收到；它只是上行发射尝试计数（armed=0 时照样涨，已实测）。

### 100.3 OLT 侧现状：纯 discovery，租约彻底过期

- 单次 `echo dump > pon_bwmap` + `tail -512`：**512/512 全部 alloc=1023**（0x3FF）。
- §97 记的「4 条 alloc=9」在这一轮**一条都没有** ⇒ ONU-ID=9 租约已完全消失。
- `regflag/grant` 按 04/09 与 08/1f 交替（= burst profile m[5]/m[16]），四档 bank 全对：

| bank | m[4] | delim | ctl 应写 | 实测 |
|---|---|---|---|---|
| 0 | 0 | ad4cc30f | 0x090404 | ✅ |
| 1 | 1 | a56679e0 | 0x090404 | ✅ |
| 2 | 2 | ad4cc30f | 0x1f0408 | ✅ |
| 3 | 3 | a56679e0 | 0x1f0408 | ✅ |

⇒ **burst profile / bwmap / discovery 窗口三条线全部排除。**

### 100.4 msgid=0x01 是原厂用法（反驳我自己的怀疑）

`setSnMIC`（ko 0xe058）反汇编构造报文：
```
e0d4: strb w3,[x29,#48]   w3=3    => buf[0]=0x03
e0c4: strb w1,[x29,#49]   w1=-1   => buf[1]=0xff
e0c8: strb w0,[x29,#50]   w0=1    => buf[2]=0x01   ← msgid
e0fc: strb wzr,[x29,#51]          => buf[3]=0x00
```
**原厂 SN 报文 msgid 就是 0x01**，驱动 `frame[2]=0x01` 正确。不要按 G.987.3 表去改成 0x02。
（其余：`0x16639`=0x170598f0 写 0；`0x1663d`=0x170598f4 header；`0x1663e/f`=MIC `rev` 后写；
 `0x16611`=0x17059844 `|=bit0` = setSnMIC 最后一步。**驱动全部做对。**）

### 100.5 剩余唯一方向

静态配置、burst profile、bwmap、SN 帧构造、发射触发**五条线全部验证过**。
现在只剩一个未解释的矛盾：

> 干净 O4 姿态下，帧被推上光纤（FIFO 扣响、tx88 涨），硬件却记 err+6，
> 且 `0x1705947c` bit22 始终为 1（未注册）——**硬件自己不认为这次发射是合法的 discovery 突发**。

下一轮该查的不是"再加什么寄存器"，而是**硬件判定"discovery 突发合法"的前置条件**：
- `0x17059910`(burst, 原厂 O5=`0x1b01`) 与 `0x17059980`(burst stat) 的联动；
- `0x17059404`（bwmap 记录控制的 bit0，厂商 Hw_Init 自己置）是否影响 discovery 判定；
- 反汇编里 `fi_Configure_Ponmac_Set_Profile` 之后是否还有一步"启用 discovery 窗口"未翻到。
- 串口长时间旁路抓物理层（用户已授权保守路线，建议先用串口 + 光功率判断是否真有上行光）。

### 100.6 §100.5 各方向逐个排查结果（09:15–09:25）

| 检查项 | 设备值 | 原厂 O5 | 结论 |
|---|---|---|---|
| `0x17059910` burst | `0x00001b01` | `0x00001b01` | ✅ 一致 |
| `0x17059404` bwmap ctl | `0x00010000` | `65536` | ✅ 一致（bit16 记录模式本来就是 1，不是我们搞坏的） |
| `0x17059414/18/1c/20` | `0x080001ff`/`1`/`0`/`0` | `0`/`1`/`0`/`0` | 差异仅 59414=记录索引残留，无害 |
| `0x17058400` rx_en | `0x0000063b` | `1595=0x63b` | ✅ 一致 |
| `0x17058414` | `0x00000002` | `2` | ✅ 一致（bit4 LOS 已清） |
| `0x17058418` | `0x00010000` | `65536` | ✅ 一致 |
| `0x1705841c` | `0x00000100` | `256` | ✅ 一致 |
| `0x17059480` alloc 表 | `0x03fe03fd` | `0x3fe03fd` | ✅ 一致 |
| DDM 光功率 | tx +0.75dBm / rx **-20.7dBm** / bias 9.24mA / `0x6e=0` | — | ✅ 激光开、有下行光 |

⇒ §100.5 列的四个方向（burst 寄存器 / bwmap ctl / rx_en 组 / 物理层）**全部排除**。

### 100.7 ★ 本轮新增的硬结论（比 §96 更收敛）

1. **`tx88` ≠ "OLT 收到了"**。实测 armed=0 时它照样涨，disarm 到全 0 才停 ⇒ 它是上行发射尝试计数。
2. **软件 FIFO 扣扳机是有效的**（`0x17059908` 写 1 立刻自清），但每次 `0x17059998` **+6**、
   `0x1705947c` bit22 恒 1 ⇒ **硬件自己判这次发射不合法**。
3. **原厂 msgid=0x01 已由反汇编确证**，不要按协议表改成 0x02。
4. **OLT 当前是纯 discovery**（512/512 alloc=1023，alloc=9 一条不剩）⇒ 租约模型已不适用，
   现在是"任何时刻都该能注册"的状态，而我们连续 29 次主动发射 + 10 分钟 O4 驻留都零回应。
5. 反过来说：**§96.4 那次能引出 03×3，说明物理上行通路过**。现在引不出来，
   三条可能：① 我们这轮 O4 姿态少了某个当时碰巧对的寄存器；② OLT 侧对该 SN 有更长退避；
   ③ 光模块上行波长/消光比在这块板子的 SerDes 配置下不稳定（DDM 看不到上行质量）。

**下一步建议（需要用户选）**：
- **A**：放弃寄存器盲试，把 08-24 那次"成功引出 03×3"的完整寄存器现场从 PON-PLAN 逐条复原，
  与我们现在的 O4 姿态做 **逐字节 diff**（这是唯一已知能出 03 的状态）。
- **B**：先做一次"原厂固件 vs 我们固件"同时刻的 **上行光波形对比**（需要示波器/光功率计，用户侧设备）。
- **C**：换 PON 口或换一台 ONU 交叉验证，判断是不是这个 OLT 端口对该 SN 做了持久隔离。

### 100.8 ★★★ 决定性对照：§96.4 九项全对，OLT 仍沉默

把 §96.4「干净 O4 姿态」的**全部九项**在与现在逐个比对，**一字不差**：

```
59400 = 0x00090004 ✅   5943c = 0x00004009 ✅   5947c = 0x43ff0000 ✅
59844 = 0x00000002 ✅   598f4 = 0x00000000 ✅   59878 = 0x1ee5fff2 ✅
5987c = 0xc1106b3a ✅   59200 = 0x00000009 ✅   599bc = 0x00000001 ✅
```

而 §96.4 在**完全相同的这九项**下，08:02–08:24 成功引出 OLT **3 批**（03×3 + 04×1 + 05×3）。

**所以：配置侧不再是变量。** 同一套寄存器现场，
- 08:0x–08:2x：OLT 回应（03/04/05 共 18/6/18）
- 09:0x–09:2x：OLT **零回应**（29 次 FIFO + 10 分钟 O4 驻留）

⇒ **差异只在 OLT 侧 / 链路侧，不在我们固件。**

### 100.9 §97.2 的一个被我忽略的强证据

§97.2 记录（今早）：手工 FIFO 连发 12 次后 **第 13 次起 FIFO 卡死**（`txstat=0x1`，
"硬件发送请求不被消费"）。**这一轮 29 次 FIFO 全部立刻自清、一次没卡。**

`txstat` 卡死说明「硬件手里有请求但上行时隙一直不给它」——那才是**硬件在等 OLT grant**
的正常表现。现在次次自清，说明**硬件根本没在等 grant 就直接丢弃了**（对应 `err+6`）。

⇒ 反过来印证：**08 点那轮硬件确实在抢 discovery 窗口（所以会卡、会超时），
09 点这轮硬件直接不认这个窗口了。**

### 100.10 结论与建议（本轮终局）

**这不是固件/寄存器能修的问题。** 五条硬件线（静态配置 / burst profile / bwmap /
SN 帧构造 / 发射触发）全部验证一致或已排除，唯一已知能工作的寄存器现场也已精确复刻。

剩下的变量全在 OLT / 光纤侧。建议按此优先级推进：

1. **确认 OLT 是否对该 SN 做了持久隔离**。今早同一 OLT 对该 SN 还回 03，之后连续失败
   （3 批 fail + 本轮 29 次）——华为 OLT 的 ONU 黑名单/退避可以长达小时级。
   → **换一台不同 SN 的 ONU 插同一 PON 口**：能注册 ⇒ OLT 只针对这个 SN；
   不能注册 ⇒ OLT 端口/业务配置侧问题（更省事，直接找运营商或换口）。
2. **等足够长的静默窗口**（建议 ≥2 小时完全不动设备，不发任何 SN），再试一次干净 O4。
   若只是退避定时器，静默是最有效的解法。
3. **物理层交叉验证**：同一条纤上原厂固件能注册（§96.6 已实锤），说明纤和光模块都好；
   再用光功率计看我们发 SN 瞬间有没有上行光跳变，可彻底分清"发没发出去"。

---

## §101 ★★★ 根因定论：O4 状态机缺了「Ranging_Time 应答」整条路径（2026-10-20 18:30）

> 用户 15:42 反馈：「回原厂是可以注册上的」⇒ OLT/光纤/光模块/SN 全部排除，
> 问题必在我们固件。本节把厂商 `zte_xgpon.ko` 的 O4 状态机完整拆开，
> 与我们 `zx279131-pon.c` 逐项对照，**找到三个我们完全没有实现的动作**。

### 101.1 厂商 O4 收到 Ranging_Time 后做的 5 件事（`p_o4_ranging_time_msg` @0xf488）

反汇编逐指令复刻（`work/zte_xgpon.dis` 14925 行起）：

```
f4ac: ldrb w0,[x19]        ; m[0]
f4b0: ldrb w1,[x19,#1]      ; m[1]
f4b4: ldur w20,[x19,#5]     ; m[5..8] → EqD（4 字节）
f4b8: orr w0,w1,w0,lsl#8    ; (m[0]<<8)|m[1] = 目的 ONU-ID
f4bc: rev w20,w20           ; 大端 → EqD 值
f4c0: sxth w0,w0
f4c4: cmp w0,#0x3ff         ; 目的不是广播 ⇒ 才处理
f4c8: b.eq f570
f4cc: bl fi_pon_timer_disable
f4d8: ldrb w1,[x19,#4]      ; m[4] = flags
f4e0: bl fi_Configure_Ponmac_Set_Eqd   ← ① 填 EqD（0x17059410）
f4f4: ldrb w0,[x19,#3]      ; m[3]
f4fc: bl send_ack_msg                    ← ② ★★ 回 ACK 帧（我们完全没有）
f538: ldrb w1,[x21,#1610]    ; 当前状态
f53c: cmp w1,#0x4           ; 状态==O4 ?
f5c0: mov w0,#0x5
f5c4: bl fi_set_ONU_activation_state     ← ③ ★★ 切 O5（我们只改状态寄存器）
```

**我们驱动的差距（`zx279131-pon.c`）**：

| # | 厂商动作 | 我们 | 后果 |
|---|---|---|---|
| ① | `Set_Eqd(Eqd, flags)` 写 0x17059410 | 手工写死 0x2046d9f | EqD 不对，OLT 不认 ranging |
| ② | **`send_ack_msg(m[3])` 回 ACK** | **完全没有** | **OLT 等不到应答 ⇒ 超时去激活（05）** |
| ③ | `fi_set_ONU_activation_state(5)` 走完整序列 | 只写 0x17059400 状态位 | O5 迁移不完整 |

### 101.2 ★★ `send_ack_msg`（@0xd838）= 我们从未做过的上行 ACK

反汇编（`work/zte_xgpon.dis` 13201 行起）：

```
d83c: mov x2,#0x2600
d840: mov w4,#0x9            ; ★ msgid = 9（PLOAM ACK）
d864: ldurh w2,[x2,#75]      ; 取 pon_base+0x264b 的 u16
d86c: strb w2,[x29,#33]     ; buf[1] = 低字节
d874: lsr w2,w2,#8
d880: strb w4,[x29,#34]     ; buf[2] = 9   ★ msgid
d884: strb w0,[x29,#35]     ; buf[3] = arg0（= m[3]，原样回抄）
d888: strb w1,[x29,#36]     ; buf[4] = arg1（= 0）
d88c: strb w2,[x29,#32]     ; buf[0] = 高字节
d89c: bl Drv_PloamuMic_Generate_Activation   ; ★ 算 MIC
d8ac: bl fi_Configure_Ponmac_Send_Ploam_Msg  ; ★ 走软件 FIFO 发出
```

**ACK 帧结构（48 字节）**：
```
buf[0..1] = pon_base+0x264b 的 u16（大概率 = 目的 ONU-ID 字段）
buf[2]    = 0x09            ★ msgid = ACK
buf[3]    = 收到的 m[3]      （原样回抄，OLT 用它匹配请求）
buf[4]    = 0
buf[5..39]= 0
buf[40..47]= CMAC(0x55×16, 0x02 || buf[0..39])
```

**MIC 算法我们已经有了**（`Drv_PloamuMic_Generate_Activation` @0x3548）：
```
buf41 = [0x02] || frame[0..39]      ; 41 字节
mic   = AES_CMAC(key, buf41)          ; key 见下
frame[40..47] = mic
```
★ **key 完全一致已实锤**：厂商 `fi_reset_database`（@0xc9c0）里
`memset(pon_base+0x2681, 0x55, 16)` 与 `memset(pon_base+0x2691, 0x55, 16)`；
我们驱动 `gpon_build_sn_frame()` 也是 `memset(key, 0x55, 16)`。
⇒ **MIC 密钥不是问题**，问题是我们**从来没发过 ACK**。

### 101.3 为什么 §96.4 观测到「70 秒后去激活」

厂商流程：OLT 发 04(Ranging_Time) ⇒ ONU 必须回 ACK ⇒ 才继续。
我们只填了 EqD、**没回 ACK** ⇒ OLT 等不到 ⇒ 超时发 05(Deactivate) + 06(Disable_SN) ⇒ 沉默。
**这解释了 §96.4/§96.5 观察到的全部现象**：03×3 → 04×1 → 05×3 → 沉默，
以及 §96.6「OLT engage 后 3 批失败即退避」。

★ **修正**：§96.5 结论「我们没理 Ranging_Time ⇒ 超时去激活」方向正确，
但**根因不是没理，而是理了一半**（只填 EqD 不回 ACK）。

### 101.4 待办（下一步）

1. 在驱动里实现 `gpon_send_ack(m[3])`：
   - 造 48 字节帧（buf[2]=9, buf[3]=m[3]），CMAC(0x55×16, 0x02||body[0..39])，
   - 走 `gpon_fifo_send()`（已有：12 大端字写 0x17059b08 + 写 1 到 0x17059908）。
2. 在 `pon_ploam_drain()` 里分发 msgid：**04 → Set_Eqd + ACK + 切 O5**。
3. `Set_Eqd` 编码变换按厂商 5b70 复刻（§96.6 已有 bash 版 `work/eqd3.sh`/`eqd4.sh`）。
4. 出 v9.62 固件刷机验证。

### 101.5 本轮排除的线索

- **SerDes `0x10e40040` bit29/bit30**（gen_en/check_en）：反汇编确认只被
  `serdes_set_tx_prbs_mode`（PRBS 自环测试）调用，**正常通路不碰** ⇒ 该线作废。
- **SerDes 寄存器文件**：原厂 `work/stock_serdes.txt` 与我们设备
  `0x10e40000..0x10e400fc` **逐字节一致**（含 0x40=0x003c0000、0x68=0x598）⇒ 非变量。
- **burst profile 4 个 bank**：我们与原厂 O5 **逐字节一致**（bank0={00090404,aa,0,ad4cc30f,0}…）⇒ 非变量。
- **PON MAC 状态区**：0x17059400/59410/5943c/59428 与原厂一致（除状态本身）⇒ 非变量。
