# APM32 Boot Loader

## 项目概述

基于 APM32E103 的 Boot Loader，用于通过 移远 CAT-1 系列 4G 模块从 UFS 文件系统读取升级固件并写入 MCU 内部 Flash。

- **MCU**: APM32E103 (ARM Cortex-M3, 36MHz)
- **4G 模块**: 移远 CAT-1 系列 (EC800/EG800K 等多种型号)，AT 命令驱动
- **编译器**: ARMCC (Keil MDK), 也支持 GCC
- **版本**: v1.5.0

## 架构概览

```
Boot Loader
├── main.c             入口 + iap_upgrade 升级主逻辑
├── iap.c/h            IAP 元数据初始化与状态保存
├── lte_eg800k.c/h     AT 命令引擎 + USART3 ISR 状态机
├── bsp_delay.c/h      SysTick 延时与时间戳
├── hw_uart.c/h        USART 初始化与发送
├── hw_flash.c/h       Flash 擦除/读写（FMC 接口）
├── hw_board.c/h       GPIO 定义与板级接口
└── http.c/h           预留（空文件）
```

## Flash 分区

| 地址         | 大小   | 用途                     |
|-------------|--------|-------------------------|
| 0x8000000   | 24KB   | Boot Loader              |
| 0x8006000   | 480KB  | APP 区域                 |
| 0x8020000   | 128KB  | 固件缓存槽 1 (FW1)       |
| 0x8040000   | 128KB  | 固件缓存槽 2 (FW2)       |
| 0x807E000   | 2KB    | 升级元数据 (Upgrade_t)    |
| 0x807E800   | 2KB    | 升级配置 (Upgrade_cfg_t)  |

Flash 页大小: 2048 字节 (FLASH_PAGE_SIZE)

## 升级流程

### 正常升级 (flag=0x3A)

```
App 下载文件 → 写入 upgrade.size/crc/flag → 复位进入 Boot
    ↓
Boot: iap_init() 读取 upgrade → iap_upgrade(0x3A)
    ↓
切换槽位 (1?2) → eg800k_init() → AT/ATE0
    ↓
QFOPEN → QFLST(校验大小) → 循环 QFREAD(40KB/次) → QFCLOSE
    ↓
CRC32 校验(与 upgrade.crc 比较) → 写 APP 区 → 再次 CRC
    ↓
iap_upgrade_state_save(0x5A) → 复位 → Jump_to_App
```

### 回退升级 (flag=0xAA)

```
校验槽位固件 CRC → 写 APP 区 → iap_upgrade_state_save(0x5A)
```

### 升级元数据结构

```c
// Upgrade_t @ UPGRADE_ADDR (0x807E000), packed
typedef struct {
  uint8_t   flag;        // 0x5A=有效 0x3A=需升级 0xAA=需回退
  uint8_t   up_count;    // 重试计数
  uint8_t   fw_flag;     // 当前活跃槽位 1/2
  uint8_t   status;
  uint32_t  size;        // 本次下载文件大小
  uint32_t  crc;         // 本次下载文件 CRC32
  char      url[128];    // 下载地址
  char      name[64];    // 文件名
  Firmware  fw[3];       // 各槽位固件校验信息 {size, crc}
} Upgrade_t;

// Upgrade_cfg_t @ UPGRADE_CFG_ADDR (0x807E800)
typedef struct {
  uint32_t  config_addr;
  uint32_t  app_addr;
  uint32_t  fw1_addr;
  uint32_t  fw2_addr;
  uint32_t  data_crc;
} Upgrade_cfg_t;
```

**字段语义**:
- `upgrade.size/crc`: 本次下载的升级文件信息（App 写入，Boot 读取并校验）
- `upgrade.fw[x].size/crc`: 各槽位已落盘固件的校验信息（Boot 写入，供回退 0xAA 使用）

## AT 命令引擎

### 关键函数

- `LTE_SendCmdWaitResp(cmd, target_resp, timeout_ms)`: 发送 AT 命令并等待响应
  - 发送前会 `__disable_irq` 清空 `uart3_rx_buf`
  - 内部原子快照 `uart3_rx_buf` 到 `local_buf`，按行（CRLF）解析匹配
  - `target_resp=NULL` 时不等待，直接返回 0

- `CheckResp_loop(target_resp, delay_ms)`: 固定 5 次轮询，每次 delay_ms 延时

### USART3 接收状态机 (RX_FLAG)

| RX_FLAG | 状态     | 描述                              |
|---------|---------|-----------------------------------|
| 0       | 空闲     | 发 QFREAD 后置 1                   |
| 1       | 等 CONNECT | 逐行解析，遇 CONNECT 切换到 2        |
| 2       | 收数据   | 按 get_data_len() 返回长度收字节     |
| 3       | 完成     | 主循环处理: flash_write → 置 0      |

### 读取块大小

`LTE_READ_CHUNK_SIZE = 40960` 字节 (约 20 页 Flash)

`up_file_buf[40960]` 需注意 SRAM 占用 (APM32E103 通常 20~64KB)

## 关键全局变量

| 变量           | 位置        | 说明                    |
|---------------|------------|------------------------|
| `upgrade`      | iap.c      | 升级元数据（读自 Flash）   |
| `upgrade_cfg`  | iap.c      | 升级配置（读自 Flash）    |
| `RX_FLAG`      | lte_eg800k.c | USART3 接收状态机标志    |
| `FILE_SIZE`    | lte_eg800k.c | 当前分片期望接收长度      |
| `uart3_rx_buf` | lte_eg800k.c | 串口接收缓冲 512 字节     |
| `uart3_rx_len` | lte_eg800k.c | 缓冲区有效长度           |
| `up_file_buf`  | lte_eg800k.c | 固件分片缓冲 40960 字节   |
| `g_sys_tick`   | bsp_delay.c | SysTick 毫秒计数        |

## 编码规范

- 字符集: GBK/GB2312 (文件注释含中文)
- C 标准: ANSI C (C99)
- 缩进: 空格 (混合，2/4/tab 均有出现，建议统一为 4 空格)
- 命名: 模块前缀 + 下划线命名 (`hw_flash_erase`, `iap_upgrade_state_save`)
- 结构体: `_T` 后缀 (`HW_GPIO_INIT_T`, `Upgrade_t`)
- 宏: 全大写 (`FLASH_PAGE_SIZE`, `UART3_RX_BUF_SIZE`)

## 已知问题与预防

1. `upgrade.fw[x]` 擦除态为 `0xFFFFFFFF`，判断有效性需同时检查 `> 0U` 且 `!= 0xFFFFFFFFU`
2. USART3 解析应做临界区快照 (`__disable_irq` 拷贝到 local buffer)，防止 ISR 并发覆盖
3. `flag_0x3A` 重试前应先 `QFCLOSE` 旧句柄，避免模块文件资源泄露
4. 升级时 `expect_size` 和 `expect_crc` 来源规则:
   - 0x3A: 使用 `upgrade.size` / `upgrade.crc`
   - 0xAA: 使用 `upgrade.fw[fw_flag].size` / `upgrade.fw[fw_flag].crc`
5. 看门狗: IWDT 2.5s 超时，长操作(擦除/写入 CRC)中需频繁 `IWDT_Refresh()`
6. `Delay_ms()` 依赖 SysTick handler 递减 `cntMs`，确保 `SysTick_Handler` 正确安装
