# CLAUDE.md

本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指导。

## 项目概览

APM32E103 Boot Loader：通过 Quectel CAT-1 4G 模块进行远程固件升级。从模块 UFS 文件系统下载固件并写入 MCU 内部 Flash。

- **MCU**：APM32E103RE（ARM Cortex-M3，36MHz，512KB Flash / 128KB SRAM）
- **4G 模块**：Quectel CAT-1 系列（EC800/EG800K），AT 指令集
- **编译器**：ARMCC（Keil MDK v5）；GCC 亦可通过 `#ifdef` 块支持
- **版本**：v1.8.1（见 `Include/main.h:50` 的 `VERSION`）
- **字符编码**：C 源码注释与字符串使用 GBK/GB2312

## 构建

工程使用 Keil MDK（ARMCC）。工程文件位于 `MDK/apm32e103_boot.uvprojx`。

**命令行构建**（若已安装 Keil）：
```bash
# 在 MDK 目录下执行：
UV4.exe -b apm32e103_boot.uvprojx -j0 -o build.log
```

**烧录目标**（通过 J-Link 等）：
- Boot Loader：`0x08000000`（24KB）
- 生产烧录通常使用 Keil IDE 的下载按钮或 J-Flash。

工程没有 Makefile 或 CMake。`.uvprojx` 文件是源文件包含、头文件路径及编译器/链接器设置的唯一依据。

## 架构

```
main.c
  ├── iap_init()          → 读取 Flash 中的升级元数据，决定动作
  └── upgrade_run(ctx)    → 升级状态机（下载 + 校验 + 写 Flash）
        ├── lte_modem_init()      → 4G 模块上下电、RDY 等待（可重试）、ATE0
        ├── at_file_open()        → AT+QFOPEN
        ├── at_file_get_size()    → AT+QFLST（校验大小与预期值）
        ├── at_file_read()        → AT+QFREAD 循环（每块 40KB）
        ├── at_file_close()       → AT+QFCLOSE
        ├── CRC32()       → 软件 CRC32（IEEE 802.3，多项式 0xEDB88320）
        ├── flash_erase_with_wdt() → 页对齐擦除，带 IWDT 刷新
        └── flash_write_with_wdt() → 字对齐写入，带 IWDT 刷新
  └── Jump_to_App()       → 关闭所有 IRQ，设置 MSP，跳转到 APP_ADDR

支撑模块（自底向上分层）：
  hw_uart3.c/h   → USART3 HAL：ISR（行模式 + 原始模式）、回调、故障诊断
  at_transport.c/h → AT 命令引擎：发送/等待、行队列、CONNECT 原始数据启动
  at_file_ops.c/h → 4G 模块 UFS 文件操作（QFOPEN/QFLST/QFREAD/QFCLOSE）
  lte_modem.c/h  → 模块生命周期：电源引脚、RDY 重试、按型号配置
  upgrade_ctrl.c/h → 升级状态机、重试策略、40KB 分块缓冲区
  bsp_delay.c/h  → SysTick 延时（1ms 时基）、g_sys_tick 计数器
  hw_uart.c/h    → USART1（调试，115200）+ USART3（4G 模块，115200）引脚配置
  hw_flash.c/h   → FMC 页擦除 / 字写入 / 字节读取
  iap.c/h        → 升级元数据初始化/保存、Jump_to_App、结构体定义
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

### 双槽位固件策略

两个固件槽位（FW1 @ `0x802A000`、FW2 @ `0x804E000`，各 144KB）提供回退保护：

- **0x3A（网络升级）**：切换到*另一个*槽位（`fw_flag` 1↔2），将新固件下载到该槽位，CRC 校验通过后复制到 APP 区并标记成功
- **0xAA（本地回退）**：使用*当前*槽位中已验证过的固件，复制到 APP 区（无需下载）。若 APP 区写入后 CRC 失败，最多重试 5 次，然后触发 `0xAA` 回退。

`upgrade.fw[0]` 镜像当前运行固件的 CRC/大小，供 APP 参考。

### 重试模式

下载（0x3A）和 APP 写入两个阶段均使用显式的 while 循环重试计数器（`dl_retry`/`app_retry`），上限为 5（`UPGRADE_MAX_DL_RETRIES`/`UPGRADE_MAX_APP_RETRIES`）。不使用 goto。

```c
while (dl_retry < UPGRADE_MAX_DL_RETRIES) {
  // 擦除槽位、打开文件、分块下载循环
  if (dl_failed) { dl_retry++; continue; }   // 完整模块上下电 + 恢复等待
  if (CRC32(slot) == expect_crc) break;      // 下载成功
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

### 看门狗（IWDT）

- 在 `main()` 中配置：分频 256，重装载值 2343 → 36MHz 下约 15 秒超时
- 所有长循环（Flash 擦除、文件下载、QFREAD 恢复等待）中必须调用 `IWDT_Refresh()`
- `Delay_ms()` 依赖 `SysTick_Handler` 递减 `cntMs` —— 若 SysTick 停止，看门狗将复位 MCU

### Printf 重定向

`main.c` 实现 `fputc()`（ARMCC）/ `_write()`（GCC）将 printf 重定向到 USART1（调试串口，115200 波特）。所有诊断输出都走同一串口——保持 printf 内容简洁，避免在临界区内拖长时序。

## Flash 内存布局

| 地址 | 大小 | 用途 |
|---------|------|---------|
| 0x08000000 | 24KB | Boot Loader |
| 0x08006000 | 144KB | APP 镜像 |
| 0x0802A000 | 144KB | 固件槽位 1（FW1） |
| 0x0804E000 | 144KB | 固件槽位 2（FW2） |
| 0x0807E000 | 2KB | 升级元数据（`Upgrade_t`） |
| 0x0807E800 | 2KB | 升级配置（`Upgrade_cfg_t`） |

Flash 页大小：2048 字节（`FLASH_PAGE_SIZE`）；0x08072000~0x0807DFFF 为预留空间（48KB）。

## 关键数据结构

```c
// Upgrade_t @ UPGRADE_ADDR (0x0807E000)，紧凑打包
typedef struct {
  uint8_t   flag;        // 0x5A=有效/APP  0x3A=网络升级  0xAA=本地回退  0xFF=首次启动
  uint8_t   up_count;    // 重试计数器
  uint8_t   fw_flag;     // 活动槽位：1 或 2
  uint8_t   status;
  uint32_t  size;        // 预期固件文件大小（由 APP 写入；Boot 校验）
  uint32_t  crc;         // 预期固件 CRC32（由 APP 写入；Boot 校验）
  char      url[128];    // 下载 URL
  char      name[64];    // 文件名
  Firmware  fw[3];       // [0]=APP 镜像，[1]=FW1 元数据，[2]=FW2 元数据（{size, crc}）
} Upgrade_t;

// Upgrade_cfg_t @ UPGRADE_CFG_ADDR (0x0807E800)
typedef struct {
  uint32_t  config_addr;  // UPGRADE_ADDR
  uint32_t  app_addr;     // APP_ADDR
  uint32_t  fw1_addr;     // UP_FILE_ADDR_1
  uint32_t  fw2_addr;     // UP_FILE_ADDR_2
  uint32_t  data_crc;     // 以上字段的 CRC32
} Upgrade_cfg_t;
```

**重要字段语义**：
- `upgrade.size`/`upgrade.crc`：由 APP 在触发 boot 前写入；Boot 读取并用于 0x3A 校验
- `upgrade.fw[x].size`/`upgrade.fw[x].crc`：Boot 在下载成功后写入；用于 0xAA 回退和 APP 侧校验
- `upgrade_cfg`：自带 CRC 保护；首次启动时初始化一次，提供基地址以便无需重新编译即可修改
- 空白 Flash 状态为 `0xFFFFFFFF`——校验时始终同时检查 `> 0` 且 `!= 0xFFFFFFFFU`

## AT 命令函数（lte_modem.h / at_file_ops.h）

| 函数 | AT 命令 | 用途 |
|----------|-----------|---------|
| `lte_modem_init(cfg)` | — | 模块上下电、等待 `RDY`（可重试）、`AT`+`ATE0` 检查 |
| `lte_modem_reset()` | — | 使用上次配置完整上下电 + 重新初始化 |
| `at_file_open()` | `AT+QFOPEN` | 从 UFS 打开文件，返回 fd |
| `at_file_get_size()` | `AT+QFLST` | 获取文件大小，通过 `strrchr` 校验 |
| `at_file_read()` | `AT+QFREAD` | 读取分块（最大 40KB），CONNECT 头 + 原始数据流经传输层 |
| `at_file_close()` | `AT+QFCLOSE` | 关闭文件描述符 |
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

1. **`upgrade.fw[x]` 初始状态**：Flash 默认值为 `0xFFFFFFFF`。将其视为有效数据前必须同时检查 `> 0U` 且 `!= 0xFFFFFFFFU`。`upgrade_run()` 同样校验 `expect_size`/`expect_crc`。
2. **CONNECT 到原始数据的竞态**：`AT+QFREAD` 数据从行模式切换到原始模式必须发生在 *ISR 回调内部*（at_transport.c 的 `at_on_line`，在 `CONNECT <len>` 行完成时）。不要把这个切换移到轮询侧（`at_wait`），否则头部之后紧接着到达的原始字节会丢失——旧代码也是在 ISR 层处理这一点的。
3. **行队列溢出**：`s_line_queue` 只能容纳 8 行。在 `at_wait()` 消费之前若涌入超过 8 行，最旧的行会被丢弃。对 boot 流程无影响（`OK`/`+QFOPEN:` 等目标行各自独立到达），但 QFREAD 的 CONNECT 行若被丢弃会超时并重试。
4. **重新打开前先 QFCLOSE**：0x3A 重试时，重新打开前必须调用 `at_file_close()`。遗留的文件句柄会导致 4G 模块资源泄漏。
5. **CRC 来源取决于标志类型**：
   - 0x3A：预期大小/CRC 来自 `upgrade.size`/`upgrade.crc`（APP 提供）
   - 0xAA：预期大小/CRC 来自 `upgrade.fw[fw_flag].size`/`.crc`（Boot 在之前成功下载时记录）
6. **处处喂狗**：IWDT 约 15 秒（分频 256，重装载 2343）。每次 Flash 操作、`at_wait()`/`at_wait_raw_done()` 轮询、5 秒恢复等待都必须调用 `IWDT_Refresh()`。漏掉一次就会静默复位。
7. **`Delay_ms()` 依赖**：依赖 `SysTick_Handler` 递减 `cntMs`。若 `SysTick_Handler` 损坏或向量表未建立，`Delay_ms()` 将永远挂起 → 看门狗复位。
8. **`hw_board.c` 双重用途**：该文件引用了 RT-Thread API（`rt_kprintf`、`rt_thread_create`、`INIT_DEVICE_EXPORT`），并定义了与 `main.c` 中相同的 `hw_gpio_init()`/`Hw_WDT_Cfg`。它似乎是 APP 工程共享的代码——修改前请确认它是否真的参与 boot loader 编译。
9. **`LTE_READ_CHUNK_SIZE = 40960`**：40KB 的缓冲区（加上 2KB 传输层行队列）约占 SRAM 的 35%（APM32E103RE 为 128KB，IRAM 已配置全片）。未先检查链接器 map 之前不要增大。完整构建的 ZI-data 约 45KB。
10. **Keil 工程是构建真相**：没有 Makefile。源文件包含、头文件路径、编译器宏全部配置在 `.uvprojx` XML 中。增删文件时必须同步更新工程文件。
11. **编码不匹配风险**：C 源文件为 GBK 编码。如果用 UTF-8 编辑器编辑，中文注释会乱码。`.c`/`.h` 文件保持 GBK；CLAUDE.md 为 UTF-8。新模块文件（hw_uart3、at_transport、at_file_ops、lte_modem、upgrade_ctrl）使用英文注释以避免此问题。
12. **新增 4G 模块型号**：创建一个新的 `LTE_MODEM_CFG_T` 常量（如 `LTE_MODEM_CFG_XXX`）并传给 `lte_modem_init()` 即可——除非 AT 文件 API 有差异，否则无需其他改动。
