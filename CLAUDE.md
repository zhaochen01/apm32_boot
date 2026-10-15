# CLAUDE.md

本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指导。

## 项目概览

APM32E103 Boot Loader：通过 Quectel CAT-1 4G 模块进行远程固件升级。从模块 UFS 文件系统下载固件并写入 MCU 内部 Flash。

- **MCU**：APM32E103RE（ARM Cortex-M3，72MHz，512KB Flash / 128KB SRAM）
- **4G 模块**：Quectel CAT-1 系列（EC800/EG800K），AT 指令集
- **编译器**：ARMCC（Keil MDK v5）；GCC 亦可通过 `#ifdef` 块支持
- **版本**：**v2.4.0.118**（**权威值在 `Include/main.h` 的 `VERSION`** —— 这份文档里的数字只是快照，改版本号时以文件为准）
- **字符编码**：C 源码统一 **UTF-8**（2026-09-18 由混合状态全量转换而来，与 IoT-4G-v2、引导区一致）

## 版本记录

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-10-15 | **v2.4.0.118** | **编译档位 `-O2` → `-Oz`：app 22568B → 17992B（省 4576B，20%），槽余量 2008B → 6584B**。源码零改动（只动 `MDK/apm32e103_boot.uvprojx` 的 C 侧 Misc Controls），最大栈用量 844B → 836B。**换档位改变全部代码的生成方式 → 必须真机把串口下载与 4G OTA 两条路各跑一遍**，详见"编译档位 `-Oz`"一节。 |
| 2026-10-15 | **v2.4.0.117** | 本批**唯一实质改动是版本号**（同 2.3.0.105 的先例）—— 为了走通平台下发的 boot 升级全链路。上一轮平台报"升级失败"的真因不是代码：平台上挂的 `IoT-Boot-2.4.0.116.bin` 与设备 A 槽里那份**逐字节相同**（`size=22568`、`crc=0xDC4DB298` 两边一致）→ APP 的 `app_ota_boot_current_match()` 直读 A 槽尾部即命中 → 走"已是最新"捷径，**一个字节都不下载**，回 `upgrade_result state=2`（`http_code:200` 是该路径写死的常量，不代表发过 HTTP）；平台只认 `state=3` 就判成失败。**平台侧建议：`state=2`（已是最新/无需升级）应判为成功**，否则每次推"当前版本"都会误报失败。<br>平台记录要填：`size=22568`、`crc=3834080009 (0xE4876B09)`，文件名必须以 `IoT-Boot` 开头（APP 按此前缀判定目标槽）。 |
| 2026-10-15 | **v2.4.0.116** | **缩体积批：app 24136B → 22568B，槽余量 440B → 2008B**（此前为加 DMA 反复撞 L6220E）。做法是替掉 SDK 的 FMC 驱动（1878B）——本工程只用四个操作，`Hw/hw_flash.c` 自带一份与 `ram_flash.c` 逐条相同的寄存器序列（详见"自带 FMC 原语"）。三条纪律：三处 `noinline` + 循环关展开（否则被内联/展开复制成十来份，反而更大）、`hw_flash_erase()` 改为自己每页喂狗（原 `flash_erase_with_wdt()` 是它的重复实现，已删）、`hw_flash_copy()` 的写入也开始查返回码（原来只查擦除）。**行为零变化**，待真机刷一次验证（擦/写/回读 CRC 全走这条路）。 |
| 2026-10-15 | **v2.4.0.115** | 串口高速下载**真机验证通过**（921600 可用）。这一轮共修掉三个只在真机上才露头的 bug：<br>① **OVRE 锁存 → 接收端永久失聪**（`serial_dl_getc` 在 RXBNE 未置位时也要无条件读一次 DATA 清标志，见已知坑 #21）；<br>② **高波特率下轮询收字节偶发丢字节** → 收页改走 **USART1 RX DMA**（协议零改动）；<br>③ **确认行冗余污染页数据**（切档时上位机重发 3 条、设备只收 1 条，后两条落进收页窗口被当页首 → 整条流错位；**115200 不协商所以一直正常**）—— 现在上位机只发 1 条、设备收下后等 350ms 再 flush。<br>失败路径保留 flash 头 24 字节（定位"收满但内容错"最有用的一条）；分块指纹与"握手流量"守卫均用完即删。 |
| 2026-10-14 | **v2.4.0.109** | 串口高速下载真机调试批（.106 是首次提交的版本号，.109 才是这轮调试完的形态）：<br>① **修 `serial_dl_getc()`/`flush()` 的 OVRE 锁存** —— RXBNE 没置位时也要无条件读一次 DATA，否则一次偶发溢出让接收端**永久失聪**（实测 `got=0/2048 ore=257万`，见已知坑 #21）。<br>② **收页改走 USART1 RX DMA**（DMA1 通道 5）—— 460800 下轮询会偶发丢 1~2 字节/166KB，导致整轮在最后一页报废；**协议零改动**。<br>③ 失败时把设备的高波特率日志**先抓出来再切回 115200**（不然读成乱码，实测只看到 `! DV 1 DV`）。<br>④ 上位机拆成两个波特率控件（端口波特率 / 下载波特率），并支持新旧两种 boot 自动协商 + 旧 boot 自动回落。<br>⑤ 删掉 0x3A 分段计时腾空间（boot 槽余量只剩 **232B**）。 |
| 2026-10-14 | **v2.4.0.106** | 串口下载新增**波特率协商**（`--baud`，缺省 115200 = 行为与老版本逐字节一致）：请求行可选第三段 + 新控制字 `#SYNC` / `#ERR baud` + `Hw/hw_uart.c` 的 `user_uart_set_baud()`。实测基线 8.6 KB/s（三条日志一致），提到 921600 后**约 3.1 倍**（61 页 14.0s → ~4.5s），Flash 擦写成为新瓶颈。<br>另加 **0x3A 路径分段计时**（临时诊断，约 424B，取够数据后应删）。<br>本批**未做**：4G 链路波特率（链路必须保持 115200）、`LTE_READ_CHUNK_SIZE` 提升（模组 QFREAD 上限未验证，见已知坑 #20）、流程重构（速度中性）。 |
| 2026-09-20 | **v2.3.0.105** | `VERSION` `2.2.0.104` → `2.3.0.105`。本批**唯一实质改动就是这个版本号**，其余（`App/iap.h` / `App/upgrade_ctrl.c` / `App/upgrade_ctrl.h`）均为**注释** —— 说明本套升级 boot 与引导区 Stage-1 同时服务 **IoT-4G-v2** 与 **Collector-4G** 两个产品，不是只有 IoT。<br>已通过 `set_upgrade` boot 自升级在 Collector-4G 真机验证：下载 → CRC 自检 → A 槽整槽存档到 B → RAM 例程擦写 A 槽 → 引导区 CRC/向量检查通过 → 新 boot 打印 `Boot Loader... 2.3.0.105` |

> **权威值在 `Include/main.h` 的 `VERSION`** —— 本表只是快照，改版本号时以文件为准。
> ⚠️ 两个产品**共用这一份 boot**：改这里等于同时改 IoT-4G-v2 与 Collector-4G 的升级行为，
> 且 `Upgrade_t` 的尺寸/布局在三处各有一份定义（本工程 `App/iap.h`、引导区 `App/boot.h`、两个 APP 侧），**任何改动必须三个工程同一提交内同步**。

## 构建

工程使用 Keil MDK（ARMCC）。工程文件位于 `MDK/apm32e103_boot.uvprojx`。

**命令行构建**（若已安装 Keil）：
```bash
# 在 MDK 目录下执行：
UV4.exe -b apm32e103_boot.uvprojx -j0 -o build.log
```

**烧录目标**（2026-09-17 三层架构起）：
- 引导区 Stage-1：`0x08000000`（12KB，独立工程 `apm32_e103_bootstrap`，永不变）
- **升级 boot A 槽：`0x08003000`（24KB）—— 只能烧 `out/apm32e103_boot_slot.bin`**
- APP A 槽：`0x0800F000`（214KB）

> ⚠️ **不能烧 `apm32e103_boot.bin`**：那是 fromelf 原始产物，末尾没有 CRC32/size
> 尾部，引导区会判槽无效并停在 production 模式。`_slot.bin`（24576 字节）由
> Keil 的 AfterMake UserProg2 自动调用 `tools/postbuild_boot_slot.py` 生成。
> 生成的 `.hex`（`CreateHexFile=1`）同样**没有尾部，不可用于烧录**。
>
> 生产烧录通常使用 Keil IDE 的下载按钮或 J-Flash，或用 APP 工程的
> `flash_boot_app.jlink`（一次烧三段）。

工程没有 Makefile 或 CMake。`.uvprojx` 文件是源文件包含、头文件路径及编译器/链接器设置的唯一依据。

## 架构

```
main.c
  ├── iap_init(&ctx)      → 读元数据 + 两槽判据，只返回"要做什么"（IAP_ACT_*）
  │                         安装参数由 ctx 带回
  ├── upgrade_run(ctx)    → 升级状态机（备份 + 下载 + 校验 + 提交 / 回退）
  │     ├── upgrade_preserve_backup() → 复制 A 槽 → B 槽并回读校验，成功后立即落盘
  │     ├── upgrade_self_run()    → boot 自升级（0x3B）：UFS 镜像 → RAM → 存档 A→B
  │     │                             → 写结果 → BAKPR 试用计数 → ram_flash_program()（永不返回）
  │     ├── lte_modem_init()      → 4G 模块上下电、RDY 等待（可重试）、ATE0
  │     ├── at_file_open/get_size/read/close → QFOPEN/QFLST/QFREAD/QFCLOSE
  │     ├── at_file_delete()      → QFDEL（安装成功后清理模组 UFS 里的固件）
  │     ├── CRC32()       → 软件 CRC32（IEEE 802.3，多项式 0xEDB88320）
  │     ├── hw_flash_copy() → 页对齐擦除 + 逐页复制（备份/回退共用），带 WDT 喂狗
  │     └── flash_write_with_wdt() → 下载分块写入 A 槽，带 WDT 喂狗
  ├── upgrade_park()      → 无法自愈的故障：喂狗死循环，绝不复位
  └── （不再有跳转实现）   → 跳 APP 由引导区负责，干完 NVIC_SystemReset() 交回

  ★ 三条不变量（见 iap.c 顶部注释）：
    1. 复位只允许发生在"持久状态已改变"之后——不存在裸复位
    2. slot_usable() 必须与引导区 boot_app_valid() 逐条同构
    3. 没有可执行的安装动作且 APP 不可用 → park，绝不靠复位"再试一次"

支撑模块（自底向上分层）：
  hw_uart3.c/h   → USART3 HAL：ISR（行模式 + 原始模式）、回调、故障诊断
  at_transport.c/h → AT 命令引擎：发送/等待、行队列、CONNECT 原始数据启动
  at_file_ops.c/h → 4G 模块 UFS 文件操作（QFOPEN/QFLST/QFREAD/QFCLOSE）
  lte_modem.c/h  → 模块生命周期：电源引脚、RDY 重试、按型号配置
  upgrade_ctrl.c/h → 升级状态机、重试策略、40KB 分块缓冲区
  bsp_delay.c/h  → SysTick 延时（1ms 时基）、g_sys_tick 计数器
  hw_uart.c/h    → USART1（调试，115200）+ USART3（4G 模块，115200）引脚配置
  hw_flash.c/h   → 自带 FMC 原语（解锁/擦页/写字/上锁，替掉 SDK 的 1878B）
                   + hw_flash_erase/write/copy（页对齐擦除 + 逐字写入 + 拷贝）
  iap.c/h        → 启动决策、槽判据、升级元数据初始化/保存、结构体定义
  ram_flash.c/h  → RAM 常驻的 Flash 擦写例程（`.ramfunc` 段）：boot 自升级专用
```

> 清单里 10 个 `.c` 与 `MDK/apm32e103_boot.uvprojx` 登记的自建源文件逐一对应，
> 另加 `Source/main.c`、`Source/system_apm32e10x.c`，共 12 个 —— 这就是全部会参与
> 编译的代码。**磁盘上有、工程里没有的 `.c` 一律视为不存在**：不参与编译，改它没有
> 任何效果。曾经的 `hw_board.c` 就是这种坑，详见已知问题 #8。

### USART3 接收路径（hw_uart3 + at_transport）

下载机制分为两层：

**hw_uart3**（`Hw/hw_uart3.c`）— 拥有 ISR，所有状态均为文件内 static：
- `UART3_MODE_LINE`：按字节累积，直到遇到 `\r\n`，然后调用 `on_line(line, len)` 回调（在 ISR 上下文调用）
- `UART3_MODE_RAW`：将 `expect_len` 字节直接流入调用方提供的缓冲区（`hw_uart3_start_raw_rx(buf, size, expect_len)`），完成后调用 `on_raw_done(actual_len)`
- 发生 OVRE/FE/NE 或失步时：记录故障快照、复位到行模式、调用 `on_fault()`

**at_transport**（`App/at_transport.c`）— 生产者-消费者行队列：
- `on_line` ISR 回调将行推入 8 项环形队列；`at_wait()` 消费并匹配（EXACT 前缀 / CONTAINS 子串 / ANY）
- **CONNECT 竞态处理**：发送 `AT+QFREAD` 前先调用 `at_arm_raw_rx()` 注册接收缓冲区。当 ISR 回调看到完整的 `CONNECT <len>` 行时，在**同一次中断调用**内切换到原始模式——头部与数据流之间不会丢失任何字节
- `at_wait_raw_done()` 轮询等待原始数据流完成，带 IWDT 刷新

故障恢复：发生 UART 错误或大小不匹配时，ISR 复位到行模式；`at_take_fault_snapshot()` 读取诊断信息（recv/expect/OVRE/FE/NE）。调用方（`upgrade_ctrl`）最多重试 5 次，每次尝试之间对模块执行上下电。

### 双区策略（A/B 双 APP 槽）

两个 214KB 固件槽（A @ `0x0800F000`、B @ `0x08044800`）提供回退保护。
**跳 APP 由引导区负责**，本工程只做"装 APP / 修 APP / 装自己"，出口全部终止：

- **路径① `flag == 0x3A`（APP 主动升级）**：`upgrade_preserve_backup()` 先把 A 槽
  复制到 B 槽并回读校验（失败则 abort，A 槽保持不动，**该步成功后立即落盘 `fw[1]`**，
  堵住"下载中途掉电丢回退能力"的窗口）；再擦 A 槽、QFREAD 40KB 分块**直接下载进 A 槽**；
  CRC 通过 + 向量表 sanity → 写 `fw[0]`=新 + `status=OK` + `flag=0x5A` → 复位
  （模组 UFS 文件**不删**，见"模组 UFS 固件文件的生命周期"）。
- **路径② 被动修复（`flag != 0x3A` 且 A 槽无效）**：B 槽有效 → 就地接管回退
  （拷贝 B → A，写 `fw[0]` + `status=FAILED` + `flag=0x5A` → 复位）；
  B 也无效 → `IAP_ACT_NONE`，由 main 探测模组 UFS，没有文件则 park。
- **路径③ APP 有效且无升级请求**：理论不可达（引导区不会把我们跳进来）。
  真发生了说明**判据漂移**，熔断为 park —— 复位必然成环。
- **路径④ `flag == 0x3B`（boot 自升级）**：`upgrade_self_run()` —— 把模组 UFS 的
  `boot_update.bin` 装进 boot A 槽（本工程自己）。详见下文"boot 自升级"。
  **路径②的判断对 0x3B 不适用**：0x3B 走 case 分支，不会落到 default 的被动修复。

**下载重试**：`UPGRADE_MAX_DL_RETRIES = 2`；耗尽 → 写 `flag=0xAA` + `status=FAILED`
→ 下次启动走回退。

**trial 自动回退已取消**（2026-09-17）。原机制：APP 写 `status=0x01` 启用，
Boot 每次启动递减 `up_count`，耗尽未确认则回退。**但 APP 从未实现 `status=OK` 确认**
（`CLAUDE.md` 旧版本已记录），所以判定永假、机制从未生效。现改为**结果回传**：
Boot 写 `status`（`0x02`=安装成功 / `0x03`=安装失败），APP 下次启动读走上报云端后清 0。
`up_count` 字段保留但恒 0。

**APP 侧判定升级结果只能看 `status`，不能看 `flag`** —— APP 的 `hw_board_init()`
一旦发现 `flag==0xFF` 就整页改写规范化成 `0x5A`，abort 与安装成功在 flag 上无法区分。

`upgrade.fw[0]` 镜像 A 槽固件的 CRC/大小，`upgrade.fw[1]` 镜像 B 槽。

### 两槽判据不一样（2026-09-17 实测踩坑）

| 槽 | 判据函数 | 检查内容 | 无记录时 |
|---|---|---|---|
| A（运行槽） | `slot_usable()` | 向量表 + （有记录时）全量 CRC | **放行**（首次 J-Link 裸烧） |
| B（备份槽） | `slot_intact()` | **只验记录 + 全量 CRC** | **不可用**（没有备份就是没有） |

> ⚠️ **"无记录"只认 `crc == 0xFFFFFFFF`**（2026-09-18 收窄）。
> 原判据是 `!FW_VALID(fw)` —— 它把 `size` 为 `0` / `0xFFFFFFFF` / 越界也一并
> 算作"无记录"，于是**放行**。可那三种是【记录损坏】，不是"没有记录"：
> 放行等于跳过 CRC 校验跳进一段没验过的代码，坏了就是 HardFault → NRST →
> 同样放行 → **无限复位环**。
> 现在：`crc == 0xFFFFFFFF` 才放行；`size == 0 || size > APP_MAX_SIZE` 一律判无效。
> **与引导区 `App/boot.c` 的 `boot_app_valid()` 必须逐条同构**（两处注释互指），
> 只改一边会制造判据漂移 —— 那会撞上 `iap_init()` 的 PARK 熔断器（设备停住
> 而不是跑 APP）。
> `FW_VALID` 宏本身**没动** —— 它在 `slot_intact()`（B 槽"无记录=不可用"）
> 和 `upgrade_preserve_backup()` 里的语义仍然正确。

> ⚠️ **B 槽绝不能套用 `slot_usable()`**。B 存的是 A 槽镜像的**拷贝**，
> 它的复位向量指向 A 的链接地址（`0x0800F000`），永远落不进 B 槽范围
> `[0x08044800, 0x0807A000)` —— 用"可运行槽"判据必然返回 0，
> **备份会静默失效**（实测日志：`slot check: app=0 bak=0` →
> `0xAA but backup unusable -> drop request`）。代码看着完全合理，只有
> 端到端跑一次才会暴露。

### 「无记录时补录」（adopt，2026-09-17）

产线 J-Link 直烧后 `Upgrade_t` 空白 → 第一次 OTA 走 `upgrade_preserve_backup()`
的第一道 early-exit（`!FW_VALID(fw[0])`）→ `run region invalid, skip preserve`
→ **这次升级没有回退点**。新固件若有问题，B 空 + UFS 也空 → 只能 production
park 现场救。

现在改为：**A/B 都无记录**时，只要 A 槽向量表合法，就按**整槽长度
`APP_MAX_SIZE`（214KB）**补录 `fw[0]` 当"最后已知良好"，然后照常拷贝到 B。

- 代价：`fw[0].size` 记成整槽容量，之后每次启动的 CRC 要覆盖 214KB
  （而非实际镜像长度），约多 80ms —— **首次 OTA 提交后会被云端下发的
  真实 size 覆盖回去**。
- 副作用：`fw[0].size` 与云端下发的镜像长度对不上 → APP 侧的同文件校验
  需要一个退化路径，见 `IoT-4G-v2/CLAUDE.md`。

### 模组 UFS 固件文件的生命周期（2026-09-17 定案）

```
APP 下载前（app_ota.c Step 1）→ QFDEL 删旧
  → QHTTPGET/QHTTPREADFILE 下载新
  → 升级 boot 安装
  → 【安装成功不删】保留至下次下载
```

| 删除点 | 位置 | 条件 |
|---|---|---|
| 1 | `IoT-4G-v2/app_ota.c` Step 1 | 无条件（下载前清残留） |
| 2 | `upgrade_ctrl.c` 提交前 | 固件**与槽地址不匹配**（向量表检查失败） |
| 3 | `upgrade_ctrl.c` 下载重试耗尽 | 固件**装不上**（已证明不可用） |

**装完不删的理由**：A/B 双坏时 UFS 里还留一份可用的恢复源；APP 每次下载前
都会清旧的，不会积累，也不会把陈旧固件当恢复源。

**删除点 2/3 是"断环器"，删不得**：这两处删的都是**已证明不可用**的固件。
不删的话，A/B 双坏时引导区会把升级 boot 一直送回来重装同一份坏件，
每轮 ~60s（两次模组初始化 + 下载），**无限循环**。删完下一轮 UFS 探测
就找不到文件 → production park 终止。云端可随时重推，不丢东西。

（保留删除点 1 的副作用：装完不删后，下次 OTA 的 Step 1 能正常删到文件，
`QFDEL CME error` 那条噪音日志自然消失。）

**boot 自升级的 `boot_update.bin` 走同一套生命周期**：APP 在 `app_ota.c` Step 1
按目标文件名 QFDEL 清残留，升级 boot 在向量表自检失败 / 与 `upgrade.size/crc`
比对失败时删除（删除点 2 的自升级版），安装成功同样不删。

### boot 自升级（A 槽自升级 + B 槽回退源，2026-09-17）

升级 boot 自己也能远程升级：APP 把新 boot 镜像下载到模组 UFS 的 `boot_update.bin`
（与 APP 的 `iot_update.bin` 可共存），写 `Upgrade_t.flag = 0x3B`（`UPGRADE_ACTION_SELF`），
引导区按 `flag ∈ {0x3A, 0x3B}` 把我们跳进 A 槽。

`iap_init()` 认 `0x3B` → 返回 `IAP_ACT_INSTALL` + `UPGRADE_ACTION_SELF` → `upgrade_self_run()`：

| # | 动作 | 要点 |
|---|------|------|
| ① | `QFREAD` 整个镜像进 RAM | 复用 40KB `up_file_buf`；长度上限 `BOOT_SLOT_MAX_IMAGE`（24564） |
| ② | 镜像向量表自检（`boot_image_ok()`） | MSP 落 SRAM、复位向量落 A 槽、Thumb 位 = 1 —— 挡住链接地址不对的固件 |
| ③ | A 槽整槽存档到 B 槽 | `hw_flash_copy()` 纯 Flash 拷贝，**这是唯一的回退源** |
| ④ | 写 `flag=0x5A` + `status=UPGRADE_STATUS_OK(0x02)` | 走 `iap_state_save()`，**只落盘、不复位** |
| ⑤ | 置试用计数 `BAKPR_DATA17 = 1` | RAM 例程只碰 Flash/GPIO、不碰 BAKPR，只能写在这里 |
| ⑥ | `ram_flash_program()` 擦写 A 槽 | RAM 常驻例程，末尾写 `SCB->AIRCR` 复位，**永不返回** |

**为什么 ④ 必须在 ⑥ 之前**：不清 `0x3B` 的话，新 boot 下次启动又读到它 → 再读 UFS 再装一遍 → **无限重装**。
**为什么用 RAM 例程**：本工程跑在 A 槽里，自升级要擦的正是 A 槽 —— 擦除那一刻 Flash 里的代码就没了，只有 RAM 里的副本能把剩下的擦除 + 写入跑完（IAP 经典做法）。
失败路径统一走 `upgrade_abort()`（写 `flag=0xFF` + `status=FAILED` 后复位）；向量表自检 / 与 `upgrade.size/crc` 比对失败的固件会被 `at_file_delete()` 删掉，断掉重装环。

**RAM 例程（`App/ram_flash.c` / `App/ram_flash.h`）**

- 调用约定（破坏任意一条都会变砖）：`src` 镜像必须**已在 RAM**；`crc` 由调用方先算好（例程内不能再调 `CRC32()`，那在 Flash 里）；**永不返回**；只能写不是自己正在跑的槽
- 例程内不访问任何全局变量（看门狗用调用方算好的 `&Hw_WDT_Cfg.port->ODATA` + `Hw_WDT_Cfg.pin`），字面量池随段一起进 RAM —— 否则擦完 A 槽后再取全局地址/字面量会 HardFault
- 段归属由手写 scatter `MDK/apm32e103_boot.sct` 的 `ER_RAMFUNC 0x2001F000 0x800 { *(.ramfunc) }` 承载，启动时由 `__main` 从 Flash 拷到 RAM
- ⚠️ **`MDK/apm32e103_boot.uvprojx` 的 `<umfTarg>` 必须为 `0`** —— 它是 "Use Memory Layout from Target Dialog"，为 `1` 时 Keil 会无视手写的 `<ScatterFile>`，每次构建重新生成自己的 scatter 覆盖到 `Objects\`，`.ramfunc` 就被 `.ANY (+RO)` 抓进 Flash 而不是 RAM

**栈 1KB → 4KB**（启动文件 `Stack_Size 0x400` → `0x1000`）：原布局里 STACK(`0x2000AD00..0x2000B100`) 紧贴 40KB 镜像缓冲 `up_file_buf`(`0x20000C0C..0x2000AC0C`) 的尾部，中间只有 244 字节空隙，栈一溢出就静默改掉即将烧进 boot 槽的镜像；boot 有 81KB RAM 没用上，扩到 4KB 零成本。

**试用计数 `BAKPR_DATA17`**（`BOOT_TRIAL_REG`，与引导区 `App/boot.c` 同址）：

> ⚠️ **写 BAKPR 之前必须先使能后备域访问**（2026-09-18 硬件实测挖出的真根因）
>
> ```c
> RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU | RCM_APB1_PERIPH_BAKR);
> PMU_EnableBackupAccess();     /* 置 PMU_CTRL.BPWEN */
> ```
>
> ST 系 MCU 备份域复位后，**BKP 数据寄存器是写保护的**。SDK 的
> `BAKPR_ConfigBackupRegister()` **自己不做这件事**，所以没这两句的话写就是
> **空操作**（读不受影响，照常返回旧值）。
> **两个时钟都要开** —— 只开 `PMU` 不够，没有 BKP 外设时钟写同样不落地
> （实测：只加 PMU 那版仍无限复位循环，补上 BAKR 才通过）。
>
> 本工程与引导区原先**都没开**，只有 APP 的 `hw_rtc_init` / `hw_board_init` 开了
> —— 于是**试用计数机制从上线起就没生效过**，读回来永远是 APP 上次留下的 0。
> 症状：`flag=0x3A` + 一份"CRC 合法但一跑就崩"的 boot → 无限跳、无限复位。
> 现在 `upgrade_self_run()` 写 trial 前开，引导区在 `boot_bakpr_init()` 里开。

| 角色 | 位置 | 行为 |
|------|------|------|
| 写者 | 本工程 `upgrade_self_run()` | 自升级成功时置 `1`（"新 boot 进入试用期"） |
| 递增者 | 引导区 `boot_stage2_trial()` | **每次真正要跳进 boot 槽之前** `+1`；`> BOOT_TRIAL_LIMIT(5)` 就拒绝跳入 → 落到"跳 APP" |
| 清者 | APP `app_ota_boot_slot_check()` | 每次启动只要读到非 `0`/非 `0xFFFF` 就无条件清 `0` |

**一次「试用」= 引导区实际把控制权交给 boot 一次**，不是"启动一次"。普通重启（`flag=0x5A` 且 APP 有效）根本不跳 boot 槽，就不该消耗次数。

新 boot 即使 CRC 完好也可能一跑就崩 —— 镜像本身查不出毛病，**计数超限是唯一证据**：APP 发现计数 `> 5` 时，即便 CRC 完好也强制从 B 槽回退 A。

> ⚠️ **2026-09-18 修 —— 原实现是无限复位环的环源**
>
> 递增原本写在 `boot_pick_stage2()` 里，**每次启动都 +1**，而且被关在
> `if (trial != 0 && trial != 0xFFFF)` 守卫**里面**。APP 又每次启动都清 `0`，
> 于是计数永远在 `0`/`1`/`2` 之间摆动，`> 5` 永不成立；更糟的是清零后
> `trial == 0` 让整个累加块被跳过，计数从此再也涨不上去。
>
> 后果链条：boot 自升级后 `flag` 已是 `0x5A` → 下次启动引导区**直接跳 APP，
> 新 boot 从未被执行** → APP 却把它当成"已验证"清 `0` → 之后某次 OTA 写
> `flag=0x3A`，引导区跳进这个 CRC 合法但一跑就崩的 boot → **无限复位环，
> 设备砖死只能 J-Link 救**。
>
> 修法：把累加挪到两个 `boot_jump(stage2)` 调用点之前（引导区 `App/boot.c` 的
> `boot_stage2_trial()`），使 APP 的清零不再有破坏性 —— 计数总能从 `0` 重新武装。
> 修后实测行为：`flag=0x3A` + 崩溃 boot，跳入 5 次后第 6 次改跳 APP，
> APP 见 `trial=6 > 5` 强制回滚 A，闭环恢复。

**槽常量**（与引导区 `App/boot.h`、`tools/postbuild_boot_slot.py`、APP 侧 `IoT-4G-v2/App/app_ota.h`/`app_ota.c` 逐字节一致）：`BOOT_SLOT_SIZE`(24576)、`BOOT_SLOT_MAX_IMAGE`(24564)、`BOOT_A_ADDR`/`BOOT_B_ADDR`、`BOOT_UPDATE_FILE_NAME`(`"UFS:boot_update.bin"`)、`UPGRADE_ACTION_SELF`(0x3B)。

### 串口下载（USART1 调试口刷机，2026-09-18）

不接 J-Link 也能刷 APP：走调试口 USART1，覆盖**产线首刷**和**现场砖机救援**。
实现分两层——`App/serial_dl.c`（字节层：USART1 收发 + 请求行状态机 + BAKPR 标志，
**不碰 Flash、不链 stdio**）与 `upgrade_ctrl.c` 的 `upgrade_serial_run()`（状态机：
preserve / 逐页擦写 / 回读校验 / 提交，复用现成的 `flash_*_with_wdt`、`CRC32`、
`iap_vector_ok`、`upgrade_preserve_backup`）。

**触发入口（两条，都汇到 `UPGRADE_ACTION_SERIAL = 0x3C`）：**

| 入口 | 位置 | 判据 |
|---|---|---|
| A 引导区魔数 | `iap_init()` 的 flag 分派**之前** | BAKPR `DATA18 == 0x5A5A`（引导区上电 150ms 内收到 `0x55 0xAA`） |
| B 砖机兜底 | `Source/main.c` 的 `if(act != IAP_ACT_PARK)` 块首 | 8 秒内收到请求行。只在 `IAP_ACT_NONE`（A/B 双坏）时才走得到 |
| （APP 命令） | `IoT-4G-v2` 的 MSH 命令 `serial_dl` | 置 `flag=0x3A` + BAKPR 标志 + 复位 → 走入口 A |

> 入口 A 的优先级**必须**高于 flag 分派：砖机且 B 槽可用时，`default` 分支会判
> `bak_ok` 直接就地回退，操作员就再也进不了串口模式。

**协议**（PC 端 `tools/serial_dl.py`，只依赖 pyserial + zlib）：

```
PC   "#DL <size> <crc32hex> [baud]\r\n"   每 100ms 重发，上电前就开
DEV  校验 -> 排空 RX -> "#RDY\r\n"         恒 115200（与引导区魔数侦听同档）
[可选] 请求行里带了 baud 就切档（缺省 115200 = 整段跳过，行为与老版本一致）：
DEV  排空 -> 切波特率 -> 等 PC 在新波特率的确认行（2s）
PC   重发同一条请求行（size/crc 必须一致）
DEV  校验通过 -> "#SYNC\r\n" / 失败 -> "#ERR baud\r\n" 后复位
DEV  preserve（A 备份到 B，214KB 满槽约 7s）
DEV  排空 RX -> "#GO\r\n"
PC   第 0 页（≤2048B）
DEV  擦 1 页 + 写 -> ACK(0x06)
...  每页等一个 ACK ...
PC   末页（可不足 2048B）
DEV  擦 1 页 + 写 -> 回读全量 CRC32 + 向量表检查
DEV  ACK(0x06) = 提交+复位 / NAK(0x15) = 失败
```

**波特率协商（2026-10-14 新增，`--baud`）**：串口刷机的瓶颈是**传输本身** ——
三条实测（61/76/81 页）都精确落在 **8.6 KB/s**，即每页 2048B 用掉 178ms 传输
+ 52ms 擦写。提到 921600 后传输降到 22ms/页，整轮约 3 倍（之后 Flash 擦写
51ms/页 成为新瓶颈，属硬件下限）。

三条时序硬约束（每条都对应一次失败模式）：

1. **PC 必须在读到 `#RDY` 时就切**，不能等 `#GO` —— 设备在 `#RDY` 之后的
   一切输出（preserve 日志、`#GO`）都是新波特率，用旧档等只会永远等不到。
2. **确认行必须排在 preserve 之前** —— 切档失败时 A/B 一个字节都还没动，
   设备回 `#ERR baud` 后干净复位（只写过元数据页），等于什么都没发生。
3. **设备侧改 BR 前必须等 `TXC` 而不是 `TXBE`** —— 末字节还在移位寄存器里
   时切档会把在途字节切坏，而设备刚发完的正是控制字 `#RDY`。

**【确认行只发一条 + 设备等 350ms】**（2026-10-15 实测）切档时上位机只发**一条**确认行；
设备收下后**等 350ms** 再走（覆盖旧版上位机"3 条 × 100ms"的跨度），随后第 ④ 步的 flush
把冗余排掉。原因：冗余行会晚到 0~200ms，正好落进收页窗口被 DMA 当【页首数据】写进
flash，整包 CRC 全错却每页都"收满、零超时"。**这是协议级约束，三个实现要同步改。**

**依赖边界（已逐项审计）**：切档只发生在会话内部且**不落盘**（不写 Flash、
不写 BAKPR）—— 引导区 150ms 魔数侦听、三个 APP 的 `serial_dl` MSH 命令、
旧版工具/脚本全都在 115200 上完成握手与复位；任何复位都会回到引导区重新
以 115200 初始化。设备侧对白名单外的值一律回落 115200，PC 侧直接拒绝。

**上位机怎么自动适配新旧两种 boot（`tools/serial_dl.py` 与
`SerialDlGui/SerialDlClient.cs` 同策略）**：

```
握手恒 115200 → 先发三段式（请求最高档）
  新 boot 认三段式 → 立刻回 #RDY → 双方切档 → #SYNC → 快 3 倍
  旧 boot 不认（crc 后见空格就重新找 '#'，永不响应）→ 见到设备说话后再等 4s
        → 自动改用两段式 → 照旧 115200 刷完
```

- **降级窗口从"见到设备说话"起算**，不是从会话开始起算：工具的用法是
  "先开工具、再给板子上电"，板子可能十几秒后才出现；从一开始计时的话，
  新 boot 上电时看到的已经是降级后的请求行了。
- **降级只影响这一轮的快慢，不会把能刷的板子刷废** —— 新 boot 也认两段式。
  这条不对称性是敢设 4s 短窗口的底气（见 `TFallback` / `FALLBACK_S` 注释）。
- **切档失败（等不到 `#SYNC`）**：多半是 USB 转串口撑不住该波特率，
  上位机提示改回 115200 重试。设备侧此时已干净复位（只写过元数据页）。

**收页改走 DMA（2026-10-14，`serial_dl.c`）**：460800 下用轮询收 2048 字节/页
实测会偶发丢字节 —— 一次 166KB 传输丢 1~2 个，整包 CRC 因此不过、**整轮下载
在最后一页报废**（实测记录：82 页全收完，最后校验失败）。机制是"轮询循环被饿到
超过 1 字节时间（21.7µs）就触发一次 OVRE"。

现在收页由 **USART1_RX → DMA1 通道 5**（F1 固定映射）搬到 RAM，CPU 完全不参与，
从根上消掉这一类。**协议一个字没改**（上位机与旧工具零影响），握手阶段（等请求行）
仍走轮询 getc —— 那一次只有几十字节，且状态机本来就要逐字节看内容。

配套：`serial_dl_recv()` 在 arm DMA 之前先 `serial_dl_flush()` —— 滞留在 DATA 里的
残字节会被 DMA 当成**页首**搬进来，整页错位（那正是"收满 2048 但内容不对"的形态）。


**四条不变量（每条都是实测踩出来的）：**

1. **按页 ACK 是整个协议的灵魂。** ACK 天然制造"PC 一定不在发数据"的窗口，
   擦除/写入全塞进这个窗口 → 1 字节接收寄存器 + 无硬件流控的问题自动消失。
   擦一页要 20~40ms，没有 ACK 的话这段时间正好够 PC 把接收寄存器冲爆。
   收页循环内**禁止 printf**（混进一个字节整包就废），全部 printf 都在循环外
   或终止分支上。
2. **末页不回"页 ACK"** —— 它的应答就是最终裁决。多发一个会让上位机把页 ACK
   当成裁决（CRC 失败也报成功）。
3. **任何终止路径上，裁决字节必须先于日志。** 曾经把 `printf("[serial] verifying")`
   放在裁决之前，工具读到了 `[` (0x5B) 当 ACK —— 61 页全传完却报"协议错位"。
4. **控制字词表**：请求行 / `#RDY` / `#GO` 一律带 `#` 前缀，ACK/NAK 用
   `0x06`/`0x15` 控制字节。上位机的接收窗口里混着设备日志（先开工具、板子后
   上电，复位前 APP 还在打日志），裸 `"RDY"` 会被 `[.. 4G] RDY confirmed`
   这类日志行误命中 —— 实测就这么失败过一次。

**掉电安全性**：拿到请求后 / `#RDY` 后掉电 → 只写了元数据，A 未动；
preserve 中掉电 → A 完好、`fw[1]` 未更新 → B 判不可用（丢回退点，设备仍能正常启动）；
收页中 / 回读中掉电 → `fw[0]` 是旧记录或占位记录 → 引导区 CRC 失败判 A 无效 →
回退 B；提交中掉电 → `Upgrade_t` 页被擦空 → `fw[0].crc = 0xFFFFFFFF` → 放行，
**但此刻 A 已经过全量 CRC + 向量表检查，放行是安全的**。

**实测记录（2026-09-18，硬件）**：完整刷写 61 页 14.0s（8.6 KB/s）；
工具中途掐断 → 3s 超时 NAK → abort → 复位 → 引导区判 A 无效 → 从 B 回退 →
正常启动 → 云端收到 `upgrade_result state=4`（安装失败），无砖无复位环；
送复位向量指向槽外的镜像 → 被向量表检查拒绝且未提交。

### BAKPR（备份寄存器）分配表

BAKPR 由 VBAT 维持、**跨掉电存活**，加新寄存器前必须全工程 grep 一遍
`BAKPR_DATA`。三个工程的宏必须一致。

| 寄存器 | 用途 | 定义处 |
|---|---|---|
| `DATA1` | RTC 初始化标志 `0x5A5A` | `IoT-4G-v2/Hw/hw_rtc.c` |
| `DATA2` | RTC 时间设置标志 / **同时**是崩溃转储 magic | 两者冲突（既有，未修） |
| `DATA2`~`DATA16` | APP 崩溃转储（`CRASH_REG_*`，15 个寄存器 30 字节） | `IoT-4G-v2/Hw/hw_board.h` |
| `DATA17` | boot 试用计数 | `boot.h` / `upgrade_ctrl.h` / `app_ota.c` |
| `DATA18` | **串口下载请求标志 `0x5A5A`** | 同上三处 |
| `DATA19`~`DATA42` | 空闲 | |

> ⚠️ 串口标志最初选了 `DATA16`，正好撞上 `CRASH_REG_R1_H`。APP 每次
> HardFault 都会把 `r1 >> 16` 写进去，约 1/65536 的概率等于 `0x5A5A` ——
> 凭空造出一个串口请求，且没人清，会一直留到把某次云端 OTA 吞掉为止。
> 已在 2026-09-18 迁到 `DATA18`。

### 重试模式

下载（0x3A）和保底/回退复制（0xAA）阶段均使用显式的 while 循环重试计数器（`dl_retry`/`app_retry`）：前者上限 `UPGRADE_MAX_DL_RETRIES`(2) = 首次 + 1 次重试，后者上限 `UPGRADE_MAX_APP_RETRIES`(5)。不使用 goto。

### 4G 安装路径的分段计时：**已删**（2026-10-14 同日加、同日删）

它本来是为了给 0x3A 拿第一份实测时间账（此前全是估算）。同一天串口路径要上
**USART1 RX DMA**，boot 槽 flash 超了 192 字节 —— 计时块的 424B 正好抵掉，于是
删掉换 DMA（DMA 净增约 700B：都是 SDK 库函数的固定开销）。

**日后要重新加**：`upgrade_run()` 的 NETWORK 分支按"阶段边界打点 + 提交前打印"
加，关键是打印【recv 累计】与【纯线速折算 `bytes×125/1440`】的差值 —— 那就是每块
QFREAD 的固定开销（AT 往返 + 模组起流延迟），它决定要不要把 `LTE_READ_CHUNK_SIZE`
从 40KB 提上去（见"已知坑 #20"）。

⚠️ **boot 槽余量已只剩 232B**（24344 / 24576）—— 再加东西前先想好删什么。

```c
while (dl_retry < UPGRADE_MAX_DL_RETRIES) {
  // 擦除运行区、打开文件、分块下载循环
  if (dl_failed) { dl_retry++; continue; }   // 完整模块上下电 + 恢复等待
  if (CRC32(run) == expect_crc) break;       // 下载成功
  dl_retry++;
}
```

QFREAD 失败重试前，`upgrade_recover_wait_with_wdt(5000)` 先排水 UART（带 IWDT 刷新）5 秒，让 4G 模块回到 AT 命令模式，因为原始二进制数据可能仍滞留在串口管线中。`lte_modem_reset()` 还会对模块执行完整的上下电。

### 临界区模式

```c
__disable_irq();
hw_flash_erase(addr, size);
hw_flash_write(addr, data, size);
__enable_irq();
```

> ⚠️ **2026-09-18 更正 —— 这个包裹其实不提供它所声称的保证。**
>
> 上层 `__disable_irq()` 在**第一次进 FMC 调用返回时就被撤销了**：每次擦/写
> 内部自己无条件 `__set_PRIMASK(1)` … `__set_PRIMASK(0)`（现为 `hw_flash.c`
> 的 `fmc_prog_word()`；2026-10-15 之前是 SDK 的 `FMC_ProgramWord`，
> `apm32e10x_fmc.c:254/283/307/321`，寄存器序列一字未改）。Cortex-M3 的
> PRIMASK 没有嵌套计数，所以外层的"全程关中断"只是**假象**。
>
> 真正提供保证的是那两句 —— 每次擦/写**内部**确实关了中断，
> Flash 的按页原子性由此而来。外层的包裹既不多做事、又给人错误的安心感。
> **`upgrade_serial_run()` 的逐页擦写因此不再包裹**（与 UFS 那条直接调用
> `flash_write_with_wdt` 的路径保持一致）；`iap_state_flush()` 里的包裹是
> 既有代码，保留但同样不提供额外保证。
>
> 仍然成立的两条：**不许在（真正关中断期间）`Delay_ms()`** —— 它是 SysTick
> 计数忙等，会死等；**不许调用自带 `__enable_irq()` 的函数**改变中断状态。

**返回值（2026-09-18 起才如实报错）**：`hw_flash_erase()` 原返回**页数**、
`hw_flash_write()` 恒返回 `0`，底层 FMC 擦/写的结果一律丢弃 —— 擦写失败对
调用方完全不可见（同文件的 `hw_flash_copy()` 反而检查了，不一致）。
现在两者都返回 `0` 成功 / `-1` 失败，`hw_flash_copy()` 的写入也一并查了
（2026-10-15：原来只查擦除）。

### 自带 FMC 原语（2026-10-15，为省 Flash 而收）

SDK 的 `apm32e10x_fmc.c` 编进镜像占 **1878B**（`FMC_ProgramWord` 1218 +
`FMC_ErasePage` 612 + Unlock/Lock），而本工程只用得到"解锁 / 擦一页 / 写一个
字 / 上锁"四件事。现由 `Hw/hw_flash.c` 里自带的 `fmc_wait` / `fmc_erase_page`
/ `fmc_prog_word` / `fmc_unlock` / `fmc_lock` 承担，**寄存器序列与 SDK 逐条
相同**（也就是与 `App/ram_flash.c` 那份已在真机跑通 boot 自升级的 RAM 例程
相同），返回值语义也保持 `0 = COMPLETE / -1 = 超时·PEF·WPEF`（别简化成
"只看 BUSY"，那会把编程错误吞成成功）。

三条实现要点，改这个文件前先读：

1. **三个 `static` 都加了 `FMC_NOINLINE`** —— 不是为了性能，是体积：`fmc_wait`
   的超时循环会被 `fmc_erase_page`(2 处) 和 `fmc_prog_word`(3 处) 内联展开，
   而这些函数又各有调用点，-O2 会把它复制成十来份（实测把 hw_flash.o 从
   344B 顶到 2270B）。同理循环上带 `#pragma clang loop unroll(disable)`，
   否则它把 12B 的检查块展开 8 份（194B）。**SDK 那 1218B 的大半就是这么来的。**
2. `hw_flash_erase()` 现在**每页喂狗** —— 整槽 214KB = 107 页 ≈ 2.7s，超过
   外部 WDT 的窗口。原先的喂狗在 `upgrade_ctrl.c` 的 `flash_erase_with_wdt()`，
   该函数已删（它就是 hw_flash_erase 的重复实现）。
3. 调用方（`upgrade_preserve_backup` / 回退路径）全都自己**回读 CRC** 复核，
   所以早返回 -1 不改变判决，只是更快失败。

> 收益实测：app 24136B → **22568B**，槽余量 440B → **2008B**。

### 编译档位 `-Oz`（2026-10-15）—— 单次收益最大的一条

`MDK/apm32e103_boot.uvprojx` 的 **C 编译器 Misc Controls 里加了 `-Oz`**（写在 `-O2` 之后，
clang 取最后一个 `-O`，所以实际生效的是 `-Oz`）。**源码零改动**，实测：

| | `-O2`（原先） | `-Oz` | 差 |
|---|---|---|---|
| app 镜像 | 22568B | **17992B** | **−4576B（−20%）** |
| 槽余量 | 2008B | **6584B** | |
| 最大栈用量 | 844B | **836B** | 略降（栈 4KB，本就宽裕） |

三条注意：

1. **只加在 C 侧**：`<Cads>` 里那份。`<Aads>`（汇编器）那份**必须留空** —— armasm 不认 `-Oz`。
2. **它改变的是全部代码的生成方式**（含 SDK 目标文件），所以换档位后**必须真机把两条路各跑一遍**
   （串口下载 + 4G OTA），不能只看编译通过。要回退就把那一格清空。
3. 本项目**没有**依赖具体指令数的忙等（所有硬件轮询都走 SDK 的 `__IOM` volatile 类型，
   超时都是计数循环 —— `-Oz` 只会让它更慢=更安全）。真正要盯的是 UART ISR 的耗时，
   115200 下每字节有 87µs 预算，远不到边。

> ⚠️ **`iap_state_flush()` 失败时绝不能复位**。元数据没真正写进去 = 持久状态
> 没改变，而调用方紧接着 `NVIC_SystemReset()`：引导区读到旧的 `flag`（比如
> 还是 `0x3A`）会把我们跳回来，每轮重来一次完整下载 → **无限重装环**。
> 它现在**回读校验**（`memcmp` Flash 与 RAM 中的 `upgrade`），不通过就 park。
> 用回读而不是查返回码：回读是唯一能同时覆盖"擦除失败"和"写入失败"的判据。
> 违反 `iap.c` 顶部不变量 1（"复位只允许发生在持久状态已改变之后"）的路径
> 一律 park，绝不复位。

`at_transport.c` 中读取/写入队列状态（ISR 会修改）时也使用同样的模式——`at_wait()` 消费队列时通过 `__disable_irq()` 保护队列指针。

### 看门狗（外部 WDT，2026-09-16 起）

**片内 IWDT 已全部移除**。它一旦使能无法软件关闭，跳转 APP 后会因无人喂而 15s 复位。
现在统一由**外部 WDT 芯片**承担，喂狗 = PB0 电平翻转：

```c
#define WDT_FEED()   (Hw_WDT_Cfg.port->ODATA ^= Hw_WDT_Cfg.pin)
```

- 所有长循环（Flash 擦除、文件下载、QFREAD 恢复等待、模组 RDY 等待）中必须调用 `WDT_FEED()`
- 模组初始化里的 `delay_feed(ms)` 每 100ms 喂一次，覆盖最长 1500ms 的 `Delay_ms()`
- `Delay_ms()` 依赖 `SysTick_Handler` 递减 `cntMs` —— SysTick 停止则 `Delay_ms()` 挂起
  → 主流程不喂狗 → 外部 WDT 拉 NRST

### Printf 重定向

`main.c` 实现 `fputc()`（ARMCC）/ `_write()`（GCC）将 printf 重定向到 USART1（调试串口，115200 波特）。所有诊断输出都走同一串口——保持 printf 内容简洁，避免在临界区内拖长时序。

## Flash 内存布局

| 地址 | 大小 | 用途 |
|---------|------|---------|
| 0x08000000 | 12KB | 引导区 Stage-1（独立工程 `apm32_e103_bootstrap`，永不变） |
| **0x08003000** | **24KB** | **升级 boot A 槽 ← 本工程的链接地址** |
| 0x08009000 | 24KB | 升级 boot B 槽（**A 槽的自升级回退源**，由 `upgrade_self_run()` 在写 A 前整槽拷贝；引导区不会跳它，只被拷回 A） |
| 0x0800F000 | 214KB | APP A 槽（`APP_ADDR`，固定链接地址） |
| 0x08044800 | 214KB | APP B 槽（`UP_BACKUP_ADDR`，最后一个已知良好版本） |
| 0x0807E000 | 2KB | 升级元数据（`Upgrade_t`）← 本工程独享整页 |
| 0x0807E800 | 2KB | 升级配置（`Upgrade_cfg_t`） |

Flash 页大小：2048 字节（`FLASH_PAGE_SIZE`）；固件大小上限 214KB
（`APP_MAX_SIZE = 0x35800`，两个 APP 槽均为此容量）。

### 升级 boot 槽的尾部布局（24KB = 24576 字节）

```
[0, size)          镜像本体（fromelf 输出，未填充）
[size, 24564)      0xFF 填充
[24564]            crc32(镜像本体)  小端
[24568]            size             小端
[24572, 24576)     0xFF  ← 旧布局的整段 CRC 放这里，留空才能让 legacy 判据拒绝
```

**四个地方必须逐字节一致，改这里 = 改协议**：引导区 `boot_stage2_valid_v2()`、
`tools/postbuild_boot_slot.py`、`iap.h` 的 `APP_MAX_SIZE`/`APP_RAM_*`、
引导区 `boot.h` 的 `BOOT_*`。（boot 槽容量与尾部偏移在 APP 侧另有一份副本：
`IoT-4G-v2/App/app_ota.h` 的 `APP_OTA_BOOT_*` 与 `app_ota.c` 的 `OTA_BOOT_*`，
同样要跟着改。）

引导区保留**旧布局回退判据**（末尾 4 字节 = 整段 CRC），使引导区与升级 boot
可以**分批烧录**，中间断电不变砖（两侧误判概率约 2⁻³²）。

## 关键数据结构

```c
// Upgrade_t @ UPGRADE_ADDR (0x0807E000)，紧凑打包
typedef struct {
  uint8_t   flag;        // 0x3A=APP 升级请求  0x3B=boot 自升级请求  0x5A=无请求/已提交
                         //   0xAA=回退请求  0xFF=空白
  uint8_t   up_count;    // 废弃（trial 已取消），恒 0
  uint8_t   fw_flag;     // 保留字段，固定为 1
  uint8_t   status;      // 升级结果回传：0x00=无  0x02=安装成功  0x03=安装失败
                         //   （升级 boot 写，APP 下次启动读走上报后清 0）
  uint32_t  size;        // 云端下发的固件大小（由 APP 写入；Boot 校验）
  uint32_t  crc;         // 云端下发的固件 CRC32（由 APP 写入；Boot 校验）
  char      url[128];    // 下载 URL（历史字段，Boot 已不读——它只从模组 UFS 取）
  char      name[64];    // 文件名（同上，Boot 用编译期宏 UPGRADE_FILE_NAME）
  Firmware  fw[2];       // [0]=APP A 槽，[1]=APP B 槽（{size, crc}）
} Upgrade_t;

// Upgrade_cfg_t @ UPGRADE_CFG_ADDR (0x0807E800)
typedef struct {
  uint32_t  config_addr;  // UPGRADE_ADDR
  uint32_t  app_addr;     // APP_ADDR
  uint32_t  fw1_addr;     // UP_BACKUP_ADDR（保底区）
  uint32_t  fw2_addr;     // 未使用（保留字段，填充为 UP_BACKUP_ADDR）
  uint32_t  data_crc;     // 以上字段的 CRC32
} Upgrade_cfg_t;
```

**重要字段语义**：
- `upgrade.size`/`upgrade.crc`：**云端下发**的固件大小/CRC，由 APP 在触发 boot 前写入；
  Boot 读取并用于 0x3A 校验
- `upgrade.fw[0]`：**A 槽已装固件**的元数据（升级成功后 = 新版本；回退后 = B 槽版本）
- `upgrade.fw[1]`：**B 槽已装固件**的元数据（备份复制成功后**立即落盘**，不等提交）；
  回退路径取自该字段
- `upgrade.status`：**升级结果回传**（取代 trial）。Boot 写 `0x02`/`0x03`，
  APP 下次启动读走上报云端后清 `0x00`。详见"双区策略"
- ⚠️ **两对 size/crc 别搞混**：顶层的 `size`/`crc` 是*待安装*的，`fw[n]` 是*已装进槽位*的
- `upgrade_cfg`：自带 CRC 保护；首次启动初始化一次，提供基地址以便无需重新编译即可修改；**地址与当前布局不符（旧版本迁移）时自动重新初始化**
- 空白 Flash 状态为 `0xFFFFFFFF`——校验时始终同时检查 `> 0` 且 `!= 0xFFFFFFFFU`（`FW_VALID` 宏）

## AT 命令函数（lte_modem.h / at_file_ops.h）

| 函数 | AT 命令 | 用途 |
|----------|-----------|---------|
| `lte_modem_init(cfg)` | — | 模块上下电、等待 `RDY`（可重试）、`AT`+`ATE0` 检查 |
| `lte_modem_reset()` | — | 使用上次配置完整上下电 + 重新初始化 |
| `at_file_open()` | `AT+QFOPEN` | 从 UFS 打开文件，返回 fd |
| `at_file_get_size()` | `AT+QFLST` | 获取文件大小，通过 `strrchr` 校验 |
| `at_file_read()` | `AT+QFREAD` | 读取分块（最大 40KB），CONNECT 头 + 原始数据流经传输层 |
| `at_file_close()` | `AT+QFCLOSE` | 关闭文件描述符 |
| `at_file_delete()` | `AT+QFDEL` | 删除 UFS 文件（**只删已证明不可用的**：向量表不匹配 / 下载重试耗尽），失败忽略 |
| `at_send_wait()` | 通用 | 发送任意 AT 命令，等待响应子串（EXACT/CONTAINS/ANY） |

## 关键全局变量

| 变量 | 位置 | 用途 |
|----------|----------|---------|
| `upgrade` | iap.c | 升级元数据（映射到 Flash 结构体） |
| `upgrade_cfg` | iap.c | 升级配置（映射到 Flash 结构体） |
| `up_file_buf[40960]` | upgrade_ctrl.c | 固件分块缓冲区（约 20 个 Flash 页），文件内 static；ISR 通过 `hw_uart3_start_raw_rx()` 直接写入 |
| `s_line_queue[]` | at_transport.c | AT 响应行队列（8 项 × 256B），生产者 = ISR 回调 |
| `s_uart3` | hw_uart3.c | 全部 USART3 ISR 状态（line_idx、原始计数、故障诊断）——文件内 static |
| `g_sys_tick` | bsp_delay.c | 系统毫秒计数器（在 SysTick ISR 中递增） |

## 代码规范

- **命名**：模块前缀 + 下划线（`hw_flash_erase`、`iap_upgrade_state_save`）
- **结构体**：`_T` 后缀（`Upgrade_t`、`HW_GPIO_INIT_T`）
- **宏**：全大写（`FLASH_PAGE_SIZE`、`UART3_RX_BUF_SIZE`）
- **缩进**：4 空格（旧代码存在混合风格，新代码优先 4 空格）
- **C 标准**：C99 + ARMCC 扩展
- **模块划分**：`/App` = 应用逻辑，`/Hw` = 硬件驱动，`/Source` = BSP/系统，`/Lib` = 厂商 SDK

## 已知坑

1. **`upgrade.fw[x]` 初始状态**：Flash 默认值为 `0xFFFFFFFF`。将其视为有效数据前必须同时检查 `> 0U` 且 `!= 0xFFFFFFFFU`（用 `FW_VALID` 宏，还须 `<= APP_MAX_SIZE`）。`upgrade_run()` 同样校验 `expect_size`/`expect_crc`。
2. **CONNECT 到原始数据的竞态**：`AT+QFREAD` 数据从行模式切换到原始模式必须发生在 *ISR 回调内部*（at_transport.c 的 `at_on_line`，在 `CONNECT <len>` 行完成时）。不要把这个切换移到轮询侧（`at_wait`），否则头部之后紧接着到达的原始字节会丢失——旧代码也是在 ISR 层处理这一点的。
3. **行队列溢出**：`s_line_queue` 只能容纳 8 行。在 `at_wait()` 消费之前若涌入超过 8 行，最旧的行会被丢弃。对 boot 流程无影响（`OK`/`+QFOPEN:` 等目标行各自独立到达），但 QFREAD 的 CONNECT 行若被丢弃会超时并重试。
4. **重新打开前先 QFCLOSE**：0x3A 重试时，重新打开前必须调用 `at_file_close()`。遗留的文件句柄会导致 4G 模块资源泄漏。
5. **CRC 来源取决于动作类型**：
   - 0x3A：预期大小/CRC 来自 `upgrade.size`/`upgrade.crc`（APP 提供）
   - 0xAA / 自动回退：预期大小/CRC 来自 `upgrade.fw[1].size`/`.crc`（保底区元数据，Boot 在保底复制时更新）
   - 0x3B（boot 自升级）：同样来自 `upgrade.size`/`upgrade.crc`（APP 提供），只是上限换成
     boot 槽 —— 判据 `expect_params_ok_boot()` 用 `BOOT_SLOT_MAX_IMAGE`(24564) 而非 `APP_MAX_SIZE`
6. **处处喂狗**：片内 IWDT 已移除，改由**外部 WDT**承担（PB0 翻转，`WDT_FEED()`）。每次 Flash 操作、`at_wait()`/`at_wait_raw_done()` 轮询、5 秒恢复等待、模组 RDY 等待都必须喂。漏掉一次就会静默复位。
7. **`Delay_ms()` 依赖**：依赖 `SysTick_Handler` 递减 `cntMs`。若 `SysTick_Handler` 损坏或向量表未建立，`Delay_ms()` 将永远挂起 → 看门狗复位。
8. **不要从 APP 工程拷 `hw_board.c` 进来（该文件已于 2026-09-18 删除）**：
   它当时在磁盘上但不参与编译，且用了 22 处 RT-Thread API
   （`rt_kprintf`/`rt_thread_create`/`INIT_DEVICE_EXPORT`/`rt_mutex_take`…）
   ——升级 boot 是裸机，这份文件本来就不可能编过。真正的风险是它定义了与
   `Source/main.c` **同名**的 `hw_gpio_init()` / `Hw_WDT_Cfg`：谁想改 boot 的
   看门狗 GPIO，很容易在这个副本上动手，改了没有任何效果。
   同类教训见"未编译的孤儿文件"——留在磁盘上但不进工程的代码比删掉更危险。
9. **`LTE_READ_CHUNK_SIZE = 40960`**：40KB 的缓冲区（加上 2KB 传输层行队列）约占 SRAM 的 35%（APM32E103RE 为 128KB，IRAM 已配置全片）。**RAM 侧已核算**（2026-10-14，`MDK/Listings/apm32e103_boot.map`）：`RW_IRAM1` 用 48384B / 上限 126976B，加到 96KB 后仍剩 20.75KB（栈顶距 `ER_RAMFUNC` 0x2001F000），理论上限 119552B —— 但**能不能加不由 RAM 决定，由模组的 `AT+QFREAD` 单块上限决定**（见 #20）。
10. **Keil 工程是构建真相**：没有 Makefile。源文件包含、头文件路径、编译器宏全部配置在 `.uvprojx` XML 中。增删文件时必须同步更新工程文件。
11. **编码已统一为 UTF-8（2026-09-18）**：原先 `.c`/`.h` 既 GBK 又 UTF-8，现已全量转为
    UTF-8，三个工程一致。转换前做过扫描 + 编译产物逐字节比对，确认**对运行时零影响**
    （详见 `IoT-4G-v2/CLAUDE.md` 的"文件编码"一节，含两个排查陷阱）。
    遗留：部分文件的注释里仍有历史 `U+FFFD`/`?` 损坏（`App/iap.h`、`Source/main.c` 等），
    那是早先某次 `errors='replace'` 转换留下的，**转换不修复它**，可用 git 历史恢复。
    **新写文件一律 UTF-8，且绝不用 `errors='replace'`。**
12. **槽判据必须两边同构（最高优先级）**：`iap.c` 的 `slot_usable()` 与引导区
    `apm32_e103_bootstrap/App/boot.c` 的 `boot_app_valid()` 是**同一份判据的两份拷贝**
    （MSP 落 RAM、复位向量落本槽、Thumb 位、无记录放行、按记录长度全量 CRC）。
    改一边必须同时改另一边 —— 两边一旦分歧，"引导区判无效 → 跳升级 boot → 升级 boot
    判有效 → 复位"就是无限复位环。`iap.c` 里那条 `verdict mismatch -> PARK` 分支是
    漂移的熔断器，**删不得**。
    （2026-09-18：两边同步收窄了"无记录"的定义，见"两槽判据不一样"一节。）

### 2026-09-18 稳定性批次（传输层 + Flash 原语）

一次专查"闭环性"的审查修掉的 6 处。共同点：**都不是新写的 bug，而是临界区画小了
或返回值被吞了**，且大多能在同文件里找到已经写对的范本。

| 位置 | 问题 | 范本 |
|---|---|---|
| `at_transport.c` `at_wait()` | 队列满时 `head == tail`，消费者却在中打开中断**之后**才读 `entry->data`；ISR 正好写回刚取走的那个槽 → 撕读。修法：临界区内先 `memcpy` 到局部 | 无（本条是源码级修复） |
| `at_transport.c` `at_transport_init()` | 重置 head/tail/count **无** `__disable_irq`。它会被 `lte_modem_reset()` 再次调用，那时模组还上电、中断还开着 | `at_flush_rx()` |
| `at_transport.c` `at_flush_rx()` | `__enable_irq()` 早于 `hw_uart3_flush_rx()`，窗口内 ISR 入队的行活过 flush → 上条命令迟到的 `OK` 被当下条的响应 | 无 |
| `at_transport.c` `parse_connect_len()` | 数字解析无溢出检测，超长串回绕成小值绕过上界 | `at_file_ops.c` 的 `parse_file_len()` |
| `Hw/hw_flash.c` erase/write | `FMC_*` 返回值全丢，`hw_flash_write` 恒返回 0 | `hw_flash_copy()` |
| `Hw/hw_uart.c` `uart_send_data()` | `while(TXBE == RESET);` 无超时无喂狗 —— 它是**所有 printf 与 AT 发送**的唯一通道 | 无 |

> `uart_send_data()` 顺带把返回类型 `uint32_t` → `int32_t`：原实现已经在返回
> `-1`（无符号下即 `0xFFFFFFFF`），调用方写 `if (ret < 0)` 会静默失效。
> 与 IoT-4G-v2 那个 ARMCLANG 无符号 `char` 哨兵是同一类坑。


13. **新增 4G 模块型号**：创建一个新的 `LTE_MODEM_CFG_T` 常量（如 `LTE_MODEM_CFG_XXX`）并传给 `lte_modem_init()` 即可——除非 AT 文件 API 有差异，否则无需其他改动。
14. **备份槽不能用运行槽的判据**：见"双区策略"的对照表。这类"两个判据看着都
    合理、但语义不同"的错误**只靠读代码发现不了** —— 必须端到端跑一次，
    看 `slot check: app=? bak=?` 的实际取值。

15. **所有复位环必须有终止路径（铁律）**：本次调试一次性找出 5 处无限复位环
    （详见下）。新增任何"失败 → 复位 → 重试"的路径前，先问：
    **复位后引导区会做出不同的决策吗？** 不会就是环。

    | # | 环源 | 修法 |
    |---|---|---|
    | 1 | `iap.c` 的 "unverified jump"（fw[0]/fw[1] 无效但运行区非空 → 返回"跳 APP"） | 删除该分支 |
    | 2 | `iap.c` `flag==0xFF` 分支判据与引导区不一致（结构体首字 vs 向量表） | 统一为 `iap_vector_ok()` |
    | 3 | `flag=0x3A` + 参数坏（空片）→ 复位后 flag 仍是 0x3A | `drop_request()` 先改状态再复位 |
    | 4 | `upgrade_ctrl.c` 两处裸 `NVIC_SystemReset()` | `upgrade_park()` |
    | 5 | 坏固件装不上但 UFS 文件不删 → 每轮重装同一份 | 重试耗尽即删（删除点 3） |

    无法自愈的故障统一走 `upgrade_park()`（喂狗死循环）—— 停在可诊断、
    可现场服务的状态，**绝不复位**。

16. **4G 模块硬件控制（与 Collector-4G 参考工程硬件相同，2026-08 实测确认）**：
    - **PB1 → 4G_PWR-EN（LDO 使能）：高电平 = 开电**。`modem_power_cycle` 必须"先拉低（关）→ 再拉高（开）并保持高"。曾因极性写反（拉高后拉低保持 = 上电即断电）导致长时间 "modem no RDY"
    - **PC0 → PCIE_RESET：须保持高电平 = 模组运行态**。`lte_modem_init()` 上电前先置 PC0 高，此后一直保持；不控制此引脚模块会停在复位态
    - **USART3 = PB10/PB11 默认映射，无需重映射**。注意别与 Collector-4G 参考工程的 UART4（PC10/PC11，GPS 相关）混淆
    - **RDY 后延迟约 1500ms 再发 AT**：RDY 只是 UART 就绪信号，AT 通道尚未就绪时首条 AT（ATE0）会被吞（参考工程实测 500ms 不够）

17. **boot 自升级（0x3B）必须自己初始化模组（2026-09-17 实测踩坑）**：
    `upgrade_run()` 里唯一一次 `lte_modem_init()` 在 **0x3A 分支内部**，而
    `UPGRADE_ACTION_SELF` 的分支排在它**之前**。漏掉这一步 AT 通道根本没通，
    症状是 `[boot-self] open UFS:boot_update.bin failed` —— 而文件其实存在
    （用 APP 的 `atx AT+QFOPEN="UFS:boot_update.bin",2` 手工能开，ret=0）。
    SELF 分支开头必须自己调一次 `lte_modem_init(&LTE_MODEM_CFG_EG800K)`。
18. **`ctx->expect_*` 是共用参数检查的输入（2026-09-17 实测踩坑）**：
    `upgrade_run()` 开头有一段**所有动作共用**的校验
    （`expect_size==0 || >APP_MAX_SIZE || expect_crc==0xFFFFFFFF` -> abort），
    它排在 SELF 分支**之前**。`iap_init()` 的每个 case 都必须把
    `ctx->expect_size`/`ctx->expect_crc` 一起填上 —— 只设 `ctx->action`
    会让这段检查恒判 invalid，症状是 `expect param invalid, abort`，
    而此时 `iap_init` 自己的 `expect_params_ok_boot()` 明明是通过的。
    这两个坑（17/18）编译期都发现不了，只有端到端跑一次才会暴露。
19. **写 BAKPR 前必须使能后备域访问（2026-09-18 硬件实测）**：ST 系 MCU 备份域
    复位后 BKP 数据寄存器**写保护**，`BAKPR_ConfigBackupRegister()` 自己不使能 ——
    漏了就是**静默空操作**（读照常返回旧值，所以查不出异常）。要两句：

        RCM_EnableAPB1PeriphClock(RCM_APB1_PERIPH_PMU | RCM_APB1_PERIPH_BAKR);
        PMU_EnableBackupAccess();

    **两个时钟都要开**：只加 `PMU` 仍不生效（实测：只加 PMU 那版依旧无限复位
    循环，补上 `BAKR` 才通过）。APP 侧一直是对的（`hw_rtc_init`/`hw_board_init`），
    引导区与升级 boot 原先都没开 —— 于是**试用计数机制从上线起就没生效过**，
    详见"试用计数"一节。同类教训：**外设寄存器的写保护/时钟门控漏了不会报错**，
    只会让写悄悄失效 —— 涉及 BKP/RTC/PMU 这类后备域外设时先确认门控。
    诊断手法：用 J-Link 直接 `mem32` 读该寄存器，看写入前后是否真的变了。
20. **`AT+QFREAD` 的单块上限未经验证（2026-10-14）**：`LTE_READ_CHUNK_SIZE`
    现在 40KB —— 这是**实测验证过**的值。把它提到 96KB 能把 214KB 从 6 块降到
    3 块（RAM 够，见 #9），但仓库里没有 EG800K 的 AT 手册，**单次 `data_size`
    上限查不到**。改之前先真机发一条 `AT+QFREAD=?` 问出支持范围（最便宜的判据）。
    改的门槛：只有"分段计时里的 `recv - wire` > 150ms/块"才值得为它冒未验证的
    模组限制；同时必须把 `UPGRADE_QFREAD_TIMEOUT_MS` 从 10s 提到 15~20s
    （96KB @115200 传输本身就要 8.53s，10s 只剩 1.47s 余量）。
21. **切档握手：确认行冗余会污染页数据（2026-10-15 真机实测，最贵的一次）**：
    串口刷机加"下载阶段波特率协商"后出现"**115200 正常、230400 及以上必失败**"，
    且症状是"每页都收到 2048 字节、零超时，但整包 CRC 全错"。真因不在高波特率、
    也不在 DMA，而在**握手时序**：上位机原先把确认行每 100ms 重发 3 次（想压过
    设备"排空 + 切档"的窗口），**设备只收下第一条就往下走** —— 后两条晚到
    0~200ms，正好落进紧接着的收页窗口，被 DMA 当成**页首数据**搬进 flash，
    整条字节流从此错位，而且**每页都错**。
    铁证：flash 头 24 字节 = `55 AA #DL 166360 14cfa28c 921600`（就是请求行本身）；
    离线用"前插 1 条确认行 + 镜像"重算，页 0 的 CRC 与设备报的**完全一致**。
    为什么只有 ≥230400 中招：**115200 走两段式请求行、根本不协商，也就没有确认行**。
    修法（两边都堵，见 serial_dl.h 的协议注释）：上位机**只发一条**确认行；
    设备收下后**等 350ms**（旧版上位机 3 条的跨度）再让第 ④ 步的 flush 排掉冗余。
    诊断武器：失败时打 flash 头 24 字节（`[serial] run head:`）—— 就是它一眼看出
    页首是请求行。**当时还加过一个"每页扫 55 AA #DL 特征、命中就 NAK"的守卫，
    已删**：它防的是"外部第二个发送者"这个**错误假设**（真因是工具自己的确认行），
    提示语还把人往"另一个工具占了 COM"上引 —— 别再加回来，根因已经堵在两端。

22. **轮询收 UART 必须无条件清 OVRE/FE/NE（2026-10-14 硬件实测，串口提速时暴露）**：
    `serial_dl_getc()` 原实现只在 RXBNE 置位时才读 DATA —— 但 **RXBNE 没置位
    ≠ 什么都没发生**。APM32 在 OVRE 置位期间不再置 RXBNE，而清 OVRE 的规定
    序列是"读 STS + 读 DATA"，只读 STS 永远清不掉它 ⇒ **一次偶发溢出 = 接收端
    永久失聪**，轮询循环还在一圈圈把同一个标志记成新的溢出。

    实测症状（460800 串口刷机，第 2 页）：
    `[serial] rx timeout at off=2048 got=0/2048 ore=2569752 fe=0 ne=0`
    —— `fe=0 ne=0` 说明波特率对、线路干净；字节确实在进（否则 OVRE 不会置位），
    但一个也读不出来，而上位机确实把那 2048 字节完整发了出去。

    修法：RXBNE 没置位但任一错误标志置位时，**无条件再读一次 32 位 `DATA`**
    （不是 `USART_RxData()` —— 那位域只有 9 位，编译器可能发窄读），返回值丢掉。
    这样一次溢出最多丢 1 个字节（由整包 CRC 兜住），而不是整轮下载报废。
    **USART3 那条路没这个问题**：它的 ISR 里本来就是 `STS`/`DATA` 成对读的
    （`hw_uart3.c` 的 `uart3_mark_fault` 之前那两行）。
    同类教训：**"标志位读一次就够了"是个陷阱 —— 有些标志的清除要求【读两个寄存器】，
    少了第二个，标志就永久锁存，而且没有任何报错**。
