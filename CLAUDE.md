# CLAUDE.md

本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指导。

## 项目概览

APM32E103 Boot Loader：通过 Quectel CAT-1 4G 模块进行远程固件升级。从模块 UFS 文件系统下载固件并写入 MCU 内部 Flash。

- **MCU**：APM32E103RE（ARM Cortex-M3，72MHz，512KB Flash / 128KB SRAM）
- **4G 模块**：Quectel CAT-1 系列（EC800/EG800K），AT 指令集
- **编译器**：ARMCC（Keil MDK v5）；GCC 亦可通过 `#ifdef` 块支持
- **版本**：v2.1.0.100（见 `Include/main.h:50` 的 `VERSION`）
- **字符编码**：C 源码注释与字符串使用 GBK/GB2312

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
  hw_flash.c/h   → FMC 页擦除 / 字写入 / 字节读取
  iap.c/h        → 启动决策、槽判据、升级元数据初始化/保存、结构体定义
  hw_board.c/h   → RT-Thread 板级信息（可能与 APP 共享，不在 boot 编译内）
  http.c/h       → 占位文件（空实现，预留给将来使用）
```

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
**跳 APP 由引导区负责**，本工程只做"装 APP / 修 APP"，三个出口全部终止：

- **路径① `flag == 0x3A`（APP 主动升级）**：`upgrade_preserve_backup()` 先把 A 槽
  复制到 B 槽并回读校验（失败则 abort，A 槽保持不动，**该步成功后立即落盘 `fw[1]`**，
  堵住"下载中途掉电丢回退能力"的窗口）；再擦 A 槽、QFREAD 40KB 分块**直接下载进 A 槽**；
  CRC 通过 → 删模组 UFS 文件 → 写 `fw[0]`=新 + `status=OK` + `flag=0x5A` → 复位。
- **路径② 被动修复（`flag != 0x3A` 且 A 槽无效）**：B 槽有效 → 就地接管回退
  （拷贝 B → A，写 `fw[0]` + `status=FAILED` + `flag=0x5A` → 复位）；
  B 也无效 → `IAP_ACT_NONE`，由 main 探测模组 UFS，没有文件则 park。
- **路径③ APP 有效且无升级请求**：理论不可达（引导区不会把我们跳进来）。
  真发生了说明**判据漂移**，熔断为 park —— 复位必然成环。

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

### 重试模式

下载（0x3A）和保底/回退复制（0xAA）阶段均使用显式的 while 循环重试计数器（`dl_retry`/`app_retry`），上限为 5（`UPGRADE_MAX_DL_RETRIES`/`UPGRADE_MAX_APP_RETRIES`）。不使用 goto。

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

Flash 写入需要关闭所有中断——写入期间若 IRQ 从 Flash 执行代码会导致硬件错误：

```c
__disable_irq();
hw_flash_erase(addr, size);
hw_flash_write(addr, data, size);
__enable_irq();
```

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
| 0x08009000 | 24KB | 升级 boot B 槽（备份槽，暂无自升级写入者） |
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
引导区 `boot.h` 的 `BOOT_*`。

引导区保留**旧布局回退判据**（末尾 4 字节 = 整段 CRC），使引导区与升级 boot
可以**分批烧录**，中间断电不变砖（两侧误判概率约 2⁻³²）。

## 关键数据结构

```c
// Upgrade_t @ UPGRADE_ADDR (0x0807E000)，紧凑打包
typedef struct {
  uint8_t   flag;        // 0x3A=升级请求  0x5A=无请求/已提交  0xAA=回退请求  0xFF=空白
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
| `at_file_delete()` | `AT+QFDEL` | 删除 UFS 文件（安装成功后清理固件，失败忽略） |
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
6. **处处喂狗**：片内 IWDT 已移除，改由**外部 WDT**承担（PB0 翻转，`WDT_FEED()`）。每次 Flash 操作、`at_wait()`/`at_wait_raw_done()` 轮询、5 秒恢复等待、模组 RDY 等待都必须喂。漏掉一次就会静默复位。
7. **`Delay_ms()` 依赖**：依赖 `SysTick_Handler` 递减 `cntMs`。若 `SysTick_Handler` 损坏或向量表未建立，`Delay_ms()` 将永远挂起 → 看门狗复位。
8. **`hw_board.c` 双重用途**：该文件引用了 RT-Thread API（`rt_kprintf`、`rt_thread_create`、`INIT_DEVICE_EXPORT`），并定义了与 `main.c` 中相同的 `hw_gpio_init()`/`Hw_WDT_Cfg`。它似乎是 APP 工程共享的代码——修改前请确认它是否真的参与 boot loader 编译。
9. **`LTE_READ_CHUNK_SIZE = 40960`**：40KB 的缓冲区（加上 2KB 传输层行队列）约占 SRAM 的 35%（APM32E103RE 为 128KB，IRAM 已配置全片）。未先检查链接器 map 之前不要增大。完整构建的 ZI-data 约 45KB。
10. **Keil 工程是构建真相**：没有 Makefile。源文件包含、头文件路径、编译器宏全部配置在 `.uvprojx` XML 中。增删文件时必须同步更新工程文件。
11. **编码现状混乱（待统一）**：本工程 `.c`/`.h` **既有 GBK 也有 UTF-8**，且部分文件
    （`App/iap.h` 48 处、`Source/main.c` 208 处）的中文注释已被 `U+FFFD` 替换字符**永久损坏**
    —— 某次 GBK→UTF-8 转换用了 `errors='replace'`。编辑前先确认目标文件的编码，
    **按原编码写回**。损坏注释可用 git 历史恢复（参考 IoT-4G-v2 的 `hw_board.c` 修复过程）。
12. **槽判据必须两边同构（最高优先级）**：`iap.c` 的 `slot_usable()` 与引导区
    `apm32_e103_bootstrap/App/boot.c` 的 `boot_app_valid()` 是**同一份判据的两份拷贝**
    （MSP 落 RAM、复位向量落本槽、Thumb 位、无记录放行、按记录长度全量 CRC）。
    改一边必须同时改另一边 —— 两边一旦分歧，"引导区判无效 → 跳升级 boot → 升级 boot
    判有效 → 复位"就是无限复位环。`iap.c` 里那条 `verdict mismatch -> PARK` 分支是
    漂移的熔断器，**删不得**。

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
