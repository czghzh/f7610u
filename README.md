# f7610u — ZTE F7610U (ZX279131) ImmortalWrt 移植源码

ZTE F7610U 光猫（ZX279131 SoC）的 ImmortalWrt 移植与 GPON 驱动开发。
本仓库是**源码快照**：只收录本项目自己写的代码、补丁与文档，
不含完整内核树 / ImmortalWrt 构建树（本地约 15GB，含上游历史）。

## 目录结构

| 目录 | 内容 |
|---|---|
| `kernel/` | 内核侧新增源码：GPON/网络驱动（`drivers/net/ethernet/zte/zx279131-*.c`）、设备树（`arch/arm64/boot/dts/zte/zx279131*`）、I2C（`i2c-zx279133.c`）、NAND（`hyf.c`）、FIT 打包描述（`*.its`） |
| `kernel/mainline-mods.patch` | 对 mainline 6.18.38 的修改补丁（Kconfig/Makefile/spi-nand/serial 等，共 245 行） |
| `immortalwrt/` | ImmortalWrt 侧：`target/linux/zte/zx279131/`（config-6.18、base-files、firmware）、image Makefile、`configs/`、`scripts/`、`package/istore/` |
| `docs/` | `PON-PLAN.md`（权威技术档案，按 §编号）、踩坑卡、任务交接件、对账清单 |
| `tools/` | 排障工具源码：`i2cprobe.c`（I2C_RDWR 读光模块 DDM）、`gpon_mic.py`（GPON MIC 验证）、串口/U-Boot 脚本（`ub_boot.py`/`uboot_load.py`）、`o5_regs.json`（原厂 O5 寄存器快照）、状态机表等 |

## 构建（在完整源码树上）

```sh
# 内核（外部树 linux-mainline-zte-zxslc-sr1010/linux-6.18.38）
make ARCH=arm64 CROSS_COMPILE=aarch64-openwrt-linux-musl- -j8 Image
# 打包 ITB（内核 load 地址 0x8C000000）
mkimage -f f7610u-bringup.its out.itb
```

完整构建树与 tftp 产物在本地工作区，不入库。

## 当前进度（2026-10-20）

GPON 注册已走到 **O4（OLT 分配 ONU-ID=9）**，两个根因已定位：
① 光模块激光器未使能（A2 页 I2C 0x51，0x6e bit6=TX_DISABLE 上电默认置位，
须先写 0x7f=6 模块密码再清 bit6）；② SN 突发须软件 FIFO 触发
（清 0x17059908 → 12 大端字写 0x17059b08.. → 写 1 到 0x17059908）。
详见 `docs/PON-PLAN.md` §94/§95。
