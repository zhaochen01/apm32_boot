# APP 与 Boot Loader 对接说明

> 适用 Boot Loader 版本：**v1.8.1**（`VERSION`，见 `Include/main.h`）
> 本文档描述 APP（运行在 APP_ADDR 的应用程序）与 Boot Loader 之间的接口契约。
> 所有地址、结构体定义均来自当前 boot 工程源码（`App/iap.h`、`App/iap.c`、`App/upgrade_ctrl.h`），APP 侧必须与之完全一致。

---

## 1. Flash 内存布局（硬契约）

| 地址 | 大小 | 用途 | 归属 |
|------|------|------|------|
| 0x08000000 | 24KB | Boot Loader | Boot |
| 0x08006000 | 144KB（0x06000~0x2A000） | APP 镜像 | APP |
| 0x0802A000 | 144KB | 固件槽位 1（FW1） | Boot（下载目标） |
| 0x0804E000 | 144KB | 固件槽位 2（FW2） | Boot（下载目标） |
| 0x0807E000 | 2KB | 升级元数据 `Upgrade_t` | **APP 写，Boot 读/写** |
| 0x0807E800 | 2KB | 升级配置 `Upgrade_cfg_t` | Boot 初始化一次 |

- Flash 页大小：**2048 字节**（`FLASH_PAGE_SIZE`）
- 芯片：**APM32E103RE，512KB Flash / 128KB RAM**（Keil 工程 IROM 已配置全片）
- 0x08072000 ~ 0x0807DFFF 为预留空间（48KB）
- **固件文件大小上限 = APP 区容量 = 槽位容量 = 144KB**（下载后要同时装入槽位和 APP 区）

## 2. 升级元数据结构（APP 侧必须原样复制）

```c
#include <stdint.h>

#define UPGRADE_ADDR     0x807E000
#define UPGRADE_CFG_ADDR 0x807E800
#define APP_ADDR         0x8006000
#define UP_FILE_ADDR_1   0x802A000   /* APP(144KB) 之后 */
#define UP_FILE_ADDR_2   0x804E000   /* FW1(144KB) 之后 */

typedef struct
{
  uint32_t size;   // 文件大小
  uint32_t crc;    // 文件 CRC32
} Firmware;

#pragma pack(push, 1)   // 必须 1 字节对齐，与 Boot 一致
typedef struct
{
  uint8_t     flag;       // 0x5A=有效/APP  0x3A=网络升级  0xAA=本地回退  0xFF=首次启动
  uint8_t     up_count;   // 重试计数（Boot 维护）
  uint8_t     fw_flag;    // 活动槽位：1 或 2
  uint8_t     status;
  uint32_t    size;       // 预期固件大小（APP 写入，Boot 校验）
  uint32_t    crc;        // 预期固件 CRC32（APP 写入，Boot 校验）
  char        url[128];   // 下载 URL（仅记录，Boot 不使用）
  char        name[64];   // 文件名（仅记录，Boot 不使用）
  Firmware    fw[3];      // [0]=APP 镜像，[1]=FW1 元数据，[2]=FW2 元数据
} Upgrade_t;
#pragma pack(pop)
```

字段语义：
- `flag`/`size`/`crc`：**由 APP 写入**，Boot 在复位后读取并执行
- `fw_flag`：Boot 维护（0x3A 时切换槽位 1↔2；0xAA 时使用当前槽位）
- `fw[1]`/`fw[2]`：Boot 下载成功后写入（size/crc），供 0xAA 回退和 APP 校验
- `fw[0]`：Boot 在升级成功（写 0x5A）时从 `fw[fw_flag]` 复制，APP 可用它判断"当前生效固件"
- `url`/`name`：Boot 不使用，APP 可自由填写作为记录

## 3. 升级触发协议

### 3.1 网络升级（flag = 0x3A）

```
APP：将固件文件放入模块 UFS（文件名必须为 UFS:iot_update.bin）
     → 写入 Upgrade_t：flag=0x3A, size=文件大小, crc=文件CRC32
     → 软复位 NVIC_SystemReset()
Boot：上电 → iap_init() 读到 0x3A → upgrade_run()
     → 切换槽位（当前 1→2，当前 2→1）
     → 模块上下电、等 RDY → AT+QFOPEN → AT+QFLST（校验 size 一致）
     → AT+QFREAD 分块下载（每块 40KB）到目标槽位 → CRC32 校验
     → 校验通过：复制槽位 → APP 区 → 校验 → 写回 flag=0x5A → 复位
     → 失败：最多重试 5 次（含模块复位+5 秒排水），仍失败写回 0x5A 放弃 → 复位
```

### 3.2 本地回退（flag = 0xAA）

```
APP：写 Upgrade_t：flag=0xAA → 软复位
Boot：读 0xAA → 校验 fw[fw_flag] 的 size/crc 有效
     → CRC 预检当前槽位内容 → 复制槽位 → APP 区 → 校验
     → 成功写 0x5A；写失败重试 5 次；槽位 CRC 不符则复位重试
```

用途：APP 区被写坏后，用 Boot 记录的**上一次成功下载**的槽位固件恢复。

### 3.3 首次启动 / 正常启动

- `flag == 0x5A`：直接跳转 APP
- `flag == 0xFF`（Flash 空白）：APP 区首 4 字节非 `0xFFFFFFFF` 则跳转 APP，否则复位
- 其他值：复位（可用来做"看门狗重启"场景）

## 4. 升级完整时序（网络升级为例）

```
时间    动作
t0      APP 完成文件下载（文件已放入模块 UFS）并写元数据
t1      APP: NVIC_SystemReset()
t2      Boot: 打印 "Boot Loader... 1.8.1.101" + 元数据摘要
t3      Boot: 模块上下电（约 0.5s）→ 等 RDY（约 1~5s）
t4      Boot: QFOPEN → QFLST → 分块下载（最大 144KB ≈ 4 块，每块约 3.6s）
t5      Boot: CRC 校验 → 复制到 APP 区 → 写 flag=0x5A
t6      Boot: 复位（Delay 200ms 后）
t7      Boot: iap_init 读到 0x5A → 跳转 APP
```

- 升级期间 APP 无运行时间约 **30~60 秒**，APP 侧需考虑数据缓存/状态保存
- Boot 完成升级后必然复位一次再跳 APP，APP 可通过 `fw[0].crc` 是否变化判断升级结果

## 5. APP 侧实现要求

### 5.1 工程配置

- 烧录起始地址：**0x08006000**，大小 ≤ 144KB
- **向量表重定位**：Boot 跳转时**不设置 VTOR**，APP 必须在启动早期执行：

```c
SCB->VTOR = 0x08006000;   /* 或用启动文件/分散加载的 __VECTOR_TABLE 机制 */
```

- 跳转前 Boot 已禁用全部 128 个 NVIC 中断并清 pending，APP 需要自行重新初始化所有使用的外设（UART/GPIO/定时器等）。注意 TMR5、SysTick、USART 外设仍处于运行状态，建议 APP 启动时统一复位初始化

### 5.2 CRC32 算法（必须与 Boot 完全一致）

IEEE 802.3：初值 `0xFFFFFFFF`、多项式 `0xEDB88320`（反射表形式）、结果按位取反：

```c
uint32_t CRC32(uint8_t *data, uint32_t length)
{
  uint32_t crc = 0xFFFFFFFF;
  uint32_t poly = 0xEDB88320;

  for (uint32_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
    {
      crc = (crc >> 1) ^ ((crc & 0x01) ? poly : 0);
    }
  }
  return ~crc;
}
```

### 5.3 触发网络升级（代码示例）

```c
#include <stdint.h>
#include <string.h>
#include "cmsis_compiler.h"  /* 或所用内核头文件 */

#define UPGRADE_ADDR     0x807E000
#define FLASH_PAGE_SIZE  2048

/* 本结构定义与 Boot 一致（见第 2 节） */

void app_request_upgrade(uint32_t file_size, uint32_t file_crc)
{
  Upgrade_t up;

  memset(&up, 0, sizeof(up));
  up.flag = 0x3A;          /* 网络升级 */
  up.size = file_size;
  up.crc  = file_crc;
  /* 可选记录：strncpy(up.url, "...", 127); strncpy(up.name, "...", 63); */

  /* 写 Flash 期间必须关中断（与 Boot 的 iap_upgrade_state_save 一致） */
  __disable_irq();
  flash_erase(UPGRADE_ADDR, FLASH_PAGE_SIZE);
  flash_write(UPGRADE_ADDR, &up, sizeof(up));
  __enable_irq();

  NVIC_SystemReset();      /* 进入 Boot 执行升级 */
}
```

触发回退：`up.flag = 0xAA;`，其余字段无需填写（Boot 使用 `fw[fw_flag]` 的已记录值）。

### 5.4 升级结果判断

复位回 APP 后读取 `Upgrade_t`（Flash 0x0807E000）：

- `flag == 0x5A` 且 `fw[0].crc != 上次记录的 crc` → 升级成功
- `flag == 0x5A` 且 `fw[0].crc == 上次` → 本次无升级（或升级失败被放弃）
- 读取注意：Flash 空白为 `0xFFFFFFFF`，判断有效数据需同时 `> 0` 且 `!= 0xFFFFFFFF`

### 5.5 固件文件要求

| 项 | 要求 |
|----|------|
| 存放位置 | 4G 模块 UFS 文件系统 |
| 文件名 | `iot_update.bin`（Boot 硬编码，见 `UPGRADE_FILE_NAME`） |
| 大小 | 与 `upgrade.size` 完全一致（Boot 用 QFLST 严格校验，不符即放弃） |
| 最大大小 | 144KB（APP 区容量 = 槽位容量） |
| CRC | 与 `upgrade.crc` 一致（IEEE 802.3，见 5.2） |
| 内容 | 可直接从 0x08006000 执行的 APP 固件镜像（含向量表） |

## 6. 调试串口

- **USART1，115200，8N1**（Boot 全部诊断输出）
- Boot 启动打印：`Boot Loader... 1.8.1.101`
- 升级过程关键打印（供联调对照）：
  - `upgrade.flag=0x3A, upgrade.fw_flag=1` —— 启动时元数据摘要
  - `selected fw_flag:2 up_file_addr:0x804E000` —— 选中槽位
  - `modem init successful!` / `modem no RDY, retry x/y` —— 模块初始化
  - `QFOPEN fd:0`、`up file size 104000`、`up file size ok!` —— 文件操作
  - `flash write offset:0 size:40960` —— 分块写进度
  - `up crc:0xXXXX`、`up file crc ok!` —— 下载校验
  - `app crc:0xXXXX`、`app crc ok!` —— APP 区写入校验
  - `QFREAD fail ret:x offset:y` —— 分块读取失败（会重试）

## 7. 注意事项与常见问题

1. **升级文件必须由 APP 提前放入模块 UFS**。Boot 只负责"读 UFS 文件 → 写 Flash"，不负责联网下载固件。文件获取流程（HTTP/MQTT/OTA 等）在 APP 侧实现
2. **`upgrade.size/crc` 必须真实**：Boot 用 QFLST 严格比对大小、用 CRC32 严格校验内容，任一不符都会放弃升级（重试 5 次后写 0x5A 复位）
3. **跳转前状态**：Boot 不关 SysTick、不复位外设；APP 启动应自行初始化全部外设。IWDT 在跳转时未使能（`main()` 中使能代码在 `iap_init()` 之后），APP 如自行使能 IWDT 则需自行喂狗
4. **结构体对齐**：`Upgrade_t` 是 1 字节打包（`#pragma pack(1)`），APP 侧编译器必须同样打包，否则与 Boot 读写字节错位
5. **写元数据的时机**：只能在"准备好文件且确认 size/crc"之后写 0x3A 并复位；中途复位（如看门狗）会导致 Boot 重复执行升级——这是设计行为（重复升级到同一槽位，无害）
6. **APP 固件最大 144KB**：超出会写入失败（Flash 越界写入 FMC 错误 → CRC 不符 → 重试耗尽）
7. **版本配套**：本文档针对 Boot v1.8.1；若 Boot 版本变化，以对应版本源码中的 `iap.h`/`upgrade_ctrl.h` 为准
