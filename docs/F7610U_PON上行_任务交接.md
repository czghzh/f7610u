# F7610U_PON上行 · 任务交接（接手先读本文件）

> 生成于 2026-10-20 19:30，基于当时磁盘状态。读者＝看不到本次对话的 AI，结论自包含。

## 一、这是什么
F7610U（ZX279131）GPON 上行注册移植：让 OLT 完成 O1→O5。**本轮已定位根因：厂商 O4 收到 Ranging_Time 后要回 ACK 帧（msgid 09）+ 切 O5，我们驱动两件都没做**（PON-PLAN §101）。
## 二、文件（根 = `…/zx/f7610u/`）
主驱动 `zx279131-pon.c`（v9.62 ACK 已实现）｜权威源 `PON-PLAN.md` §101＝根因｜待刷 `f7610u-v9.62-rtack.itb`｜清单见附录。

## 三、已拍板口径（勿改）
1. 命令 `ssh -F /dev/null wen@127.0.0.1`（空密码）；设备 `root@192.168.1.1`；禁 `run boota`。
2. msgid：03=Assign_ONU-ID；**04=Ranging_Time（EqD=msg[5..8] 大端，flags=msg[4]）**；05=Deactivate；**09=ACK（上行必须回）**；SN=`SKYWa8d40ba7`；ONU-ID=9。
3. MIC 密钥＝0x55×16（厂商与本驱动一致已实锤）；EqD 编码＝厂商 `Set_Eqd` mode1：`0x00082c03→0x0205b681`。
4. 原厂**能注册**已实锤 ⇒ OLT/光纤/光模块/SN 排除，问题必在我们固件。磁盘最新版>会话记忆。

## 四、风格（用户明确）
中文、简洁重实证、结论优先、表格+具体命令；交代什么做什么，做完立刻停。

## 五、当前状态
- 已完成：★根因＝厂商 O4 收 Ranging_Time 后须**回 ACK（msgid 09）+ 切 O5**，我们两件都没做。已写 `gpon_send_ack()`/`gpon_encode_eqd()`/`gpon_rt_worker`+msgid 04 分发；内核已编、v9.62 已打包。
- **进行中：v9.62 刷机未完成**——设备已 `reboot -f` 停 U-Boot（`=>`），那次 `uboot_load.py` 没跑成功。**下一步＝重跑 `uboot_load.py` 刷 v9.62**。
- 已证伪勿再查（SerDes bit29/30、SerDes 寄存器、burst profile、PON MAC 状态区、416 项全差分）。

## 六、编辑纪律（铁律）
1. 磁盘最新版就地改，禁整篇重生成；改前备份 `过程/`；设备 /tmp 重启即失。
2. 增量编 `make … Image`→`mkimage -f f7610u-bringup.its`；刷机 `ub_boot.py keep`+`reboot -f`→`uboot_load.py`（串口只开一次）。
3. **长任务宿主编排**；**寄存器只读 `pon_reg` 节点**（`/dev/mem` 直读未映射地址会挂总线掉电）。

## 七、坑（完整版 `F7610U_踩坑卡.md`）
激光 A2=0x51、`0x6e bit6` 掉电重置｜SN 触发写 1 到 `0x17059908`｜禁写 5 个寄存器区｜grep "000904" 误报须行锚定｜busybox sleep/tftp/sed 限制｜`zx_peek` 需 0x 前缀。

---
接手先读本文件。

接手口令（复制下面一行到新任务输入框即可启动）：
接手 F7610U_PON上行_任务交接.md

## 附录（按需再读）

- **§101 根因全文**：`PON-PLAN.md` §101.1–101.5（厂商 `p_o4_ranging_time_msg` @0xf488 逐指令、`send_ack_msg` @0xd838 帧结构、MIC 密钥实锤）。
- 厂商 ko 反汇编：`work/zte_xgpon.dis`（PON 驱动 1948 符号）、`work/plat.dis`（平台层，含 SerDes）。
- 寄存器金矿：`work/fw_o5_live.json`（原厂 O5 活体快照）、`work/o5_regs.json`（6144 项）。
- 工具：`tftp/zxr`（设备端 dump 寄存器，`scp -O` 推）、`work/i2cprobe`（激光/DDM）、`work/ub_boot.py`/`uboot_load.py`、`work/gpon_mic.py`（纯 python CMAC 自测）。
- 现场脚本：`work/auto.sh`（clean O4→04→EqD+O5 循环）、`work/eqd3.sh`（三候选 EqD）、`work/eqd4.sh`（交替姿态 30 分钟）、`work/watch3.sh`。
- 原厂取证：telnet `root/Zte521`、`tftp/gpondrv.ko`/`bspdriver.ko`/`optical.ko`/`zx_ponreg.ko`/`plat-zxylzb_9131.ko`、`work/klogdump.c`（原厂内核日志）。
- 干净 O4 姿态配方：PON-PLAN §96.4；EqD 编码算法：§96.6 + `work/eqd3.sh`。
- 固件约定（拍板）：zh-cn/Asia/Shanghai/firewall4/不要 argon/要 iStore；刷机归用户。
- 历史检查点：`F7610U_PON上行_检查点.md`（压缩恢复用，内容较本文件旧）；记忆 `.workbuddy/memory/2026-10-20.md`。

### 回补记录（读侧追加；写侧修订时消费并清空，≤5 行）
> 只记「因交接件缺失而真出错」的项。本轮无。
