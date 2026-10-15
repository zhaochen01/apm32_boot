# APP 与 Boot Loader 对接说明

> ⚠️ **本文档已过时，勿按它对接（2026-10-14 标注）。**
>
> 它描述的是**旧的两层布局**：运行区 `0x08006000`（224KB）、保底区 `0x0803E000`、
> 结构体里的 `fw[3]`、以及**早已取消的 trial 回退契约**（`status=0x01` / `up_count`）。
>
> 现行是**三层架构**（引导区 Stage-1 + 升级 boot 24KB A 槽 + APP 214KB A/B 槽），
> 地址与字段语义一律以源码为准：
>   · `App/iap.h` —— `APP_ADDR`(0x0800F000) / `UP_BACKUP_ADDR`(0x08044800) /
>     `APP_MAX_SIZE`(0x35800=214KB) / `UPGRADE_STATUS_NONE|OK|FAILED`
>   · `App/iap.c` —— 启动决策、两槽判据（与引导区必须同构）
>   · `App/upgrade_ctrl.h` —— 升级状态机、动作码
> 整体重写本文档是待办事项（见 `CLAUDE.md` 的版本记录）。

> 适用 Boot Loader 版本：**v2.4.0.106**（`VERSION`，见 `Include/main.h`）。
> ⚠️ 但如上所述，本文的布局与契约部分与现行版本**对不上**，仅作历史参考。
> 本文档描述 APP（运行在 APP_ADDR 的应用程序）与 Boot Loader 之间的接口契约。
> 所有地址、结构体定义均来自当前 boot 工程源码（`App/iap.h`、`App/iap.c`、`App/upgrade_ctrl.h`），APP 侧必须与之完全一致。

---

## 1. Flash 内存布局（硬契约）

| 地址 | 大小 | 用途 | 归属 |
|------|------|------|------|
| 0x08000000 | 24KB | Boot Loader | Boot |
| 0x08006000 | **224KB**（0x06000~0x3E000） | **运行区**（APP 镜像，固定链接地址） | APP |
| 0x0803E000 | **224KB**（0x3E000~0x76000） | **保底区**（最后一个已知良好版本） | Boot |
| 0x08076000 | 32KB | 预留空间 | — |
| 0x0807E000 | 2KB | 升级元数据 `Upgrade_t` | **APP 写，Boot 读/写** |
| 0x0807E800 | 2KB | 升级配置 `Upgrade_cfg_t` | Boot 初始化一次 |

- Flash 页大小：**2048 字节**（`FLASH_PAGE_SIZE`）
- 芯片：**APM32E103RE，512KB Flash / 128KB RAM**（Keil 工程 IROM 已配置全片）
- **固件文件大小上限 = 运行区容量 = 224KB**（`APP_MAX_SIZE`，须 ≥200KB 且 ≤224KB）
- 设计说明：升级时 Boot 先将运行区（旧固件）复制到保底区，再把新固件直接写入运行区。保底区始终持有"上一个已知良好版本"，任何升级/启动失败都可回退

## 2. 升级元数据结构（APP 侧必须原样复制）

```c
#include <stdint.h>

#define UPGRADE_ADDR     0x807E000
#define UPGRADE_CFG_ADDR 0x807E800
#define APP_ADDR         0x8006000
#define UP_BACKUP_ADDR   0x803E000

/* 升级状态 */
#define UPGRADE_STATUS_TRIAL  0x01   /* trial 期: APP 请求升级时写入=启用 trial 回退 */
#define UPGRADE_STATUS_OK     0x02   /* 稳定运行: APP 自检通过后写入 */

typedef struct
{
  uint32_t size;   // 文件大小
  uint32_t crc;    // 文件 CRC32
} Firmware;

#pragma pack(push, 1)   // 必须 1 字节对齐，与 Boot 一致
typedef struct
{
  uint8_t     flag;       // 0x5A=有效/跳转  0x3A=网络升级  0xAA=本地回退  0xFF=首次启动
  uint8_t     up_count;   // trial 启动尝试计数（Boot 维护）
  uint8_t     fw_flag;    // 保留字段，固定为 1
  uint8_t     status;     // 0x01=TRIAL  0x02=OK（见 5.3）
  uint32_t    size;       // 预期固件大小（APP 写入，Boot 校验）
  uint32_t    crc;        // 预期固件 CRC32（APP 写入，Boot 校验）
  char        url[128];   // 下载 URL（仅记录，Boot 不使用）
  char        name[64];   // 文件名（仅记录，Boot 不使用）
  Firmware    fw[3];      // [0]=运行区，[1]=保底区，[2]=保留
} Upgrade_t;
#pragma pack(pop)
```

字段语义：
- `flag`/`size`/`crc`：**由 APP 写入**，Boot 在复位后读取并执行
- `status`：**trial 回退契约**（见 5.3）。APP 请求升级时写 `0x01` 可启用"新固件启动失败自动回退"；APP 稳定启动后写 `0x02` 解除。不写（旧 APP）则无 trial 行为
- `up_count`：Boot 维护（trial 启动计数，上限 3）
- `fw[0]`：运行区元数据（Boot 维护）。升级成功后 = 新固件；回退后 = 保底区固件。APP 可据此判断"当前生效固件"
- `fw[1]`：保底区元数据（Boot 维护）。0xAA 回退与启动自动回退均取自该字段
- `fw[2]`：保留，APP 不应使用
- `url`/`name`：Boot 不使用，APP 可自由填写作为记录

## 3. 升级触发协议

### 3.1 网络升级（flag = 0x3A）

```
APP：将固件文件放入模块 UFS（文件名必须为 UFS:iot_update.bin）
     → 写入 Upgrade_t：flag=0x3A, size=文件大小, crc=文件CRC32
       （可选启用 trial 回退：再写 status=0x01）
     → 软复位 NVIC_SystemReset()
Boot：上电 → iap_init() 读到 0x3A → upgrade_run()
     → ① 保底：运行区 CRC 有效 ? 复制运行区 → 保底区，回读校验
        校验失败 → 中止升级，旧固件继续运行（运行区未被触碰）
     → ② 擦除运行区 → 模块上下电、等 RDY → AT+QFOPEN → AT+QFLST（校验 size 一致）
       → AT+QFREAD 分块下载（每块 40KB）直接写入运行区 → CRC32 校验
     → ③ 提交：写回 flag=0x5A（fw[0]=新版本, fw[1]=旧版本）→ 复位
     → 失败：最多重试 5 次（含模块复位+5 秒排水），仍失败写 flag=0xAA → 复位走回退
```

### 3.2 本地回退（flag = 0xAA）

```
APP：写 Upgrade_t：flag=0xAA → 软复位
Boot：读 0xAA → 校验 fw[1] 的 size/crc 有效 + 保底区 CRC 预检
     → 复制保底区 → 运行区 → 回读校验
     → 成功写 0x5A；复制失败重试 5 次；保底区 CRC 不符则复位重试
```

用途：运行区被写坏、或新固件无法启动时，用保底区的**上一个已知良好版本**恢复。

### 3.3 首次启动 / 正常启动

- `flag == 0x5A`：校验运行区 CRC 与 `fw[0]` 一致后跳转 APP；**CRC 不符则自动从保底区恢复再跳转**；双区均无效则停在 Boot（打印错误，不跳转）
- `flag == 0xFF`（Flash 空白）：运行区首 4 字节非 `0xFFFFFFFF` 则跳转 APP，否则复位
- 其他值：复位（可用来做"看门狗重启"场景）

## 4. 升级完整时序（网络升级为例）

```
时间    动作
t0      APP 完成文件下载（文件已放入模块 UFS）并写元数据
t1      APP: NVIC_SystemReset()
t2      Boot: 打印 "Boot Loader... 2.3.0.105" + 元数据摘要
t3      Boot: 保底复制（运行区 → 保底区，224KB 约 3~6 秒，回读校验）
t4      Boot: 模块上下电（约 0.5s）→ 等 RDY（约 1~5s）
t5      Boot: QFOPEN → QFLST → 分块下载（最大 224KB ≈ 6 块，每块约 3.6s）
t6      Boot: CRC 校验 → 写回 flag=0x5A（含 fw[0]/fw[1]/trial）
t7      Boot: 复位（Delay 200ms 后）
t8      Boot: iap_init 读到 0x5A → 运行区校验通过 → 跳转 APP
```

- 升级期间 APP 无运行时间约 **30~70 秒**，APP 侧需考虑数据缓存/状态保存
- Boot 完成升级后必然复位一次再跳 APP，APP 可通过 `fw[0].crc` 是否变化判断升级结果

## 5. APP 侧实现要求

### 5.1 工程配置

- 烧录起始地址：**0x08006000**，大小 ≤ **224KB**
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

### 5.3 trial 回退契约（新固件启动失败自动回退）

Boot 在升级成功后给新固件 3 次启动机会：每次复位启动，若 `status` 仍为 `0x01`（TRIAL），`up_count` 递减；减到 0 仍未确认 → Boot 自动从保底区恢复旧固件。

**APP 必须在自检通过后主动"确认存活"**，否则会被误判为启动失败：

```c
/* 放在 APP 自检（外设/数据校验）通过之后 */
up.status = UPGRADE_STATUS_OK;
up.up_count = 0;
/* 写回 Flash 0x0807E000（需关中断，与触发升级相同的写法） */
```

- 请求升级时写 `status=0x01`（随 `flag=0x3A` 一并写入）→ 启用 trial 回退
- 不写 status（旧 APP）→ 升级成功后 Boot 直接置为 OK，无 trial 行为（向后兼容）
- 确认时机：必须晚于"自检通过"，越早越好（3 次机会 ≈ 3 × 15 秒 WDT 周期）
- 0xAA 回退成功后 Boot 会自动置 `status=OK`，旧固件无需再确认

### 5.4 触发网络升级（代码示例）

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
  up.status = UPGRADE_STATUS_TRIAL;   /* 可选：启用 trial 回退 */
  /* 可选记录：strncpy(up.url, "...", 127); strncpy(up.name, "...", 63); */

  /* 写 Flash 期间必须关中断（与 Boot 的 iap_upgrade_state_save 一致） */
  __disable_irq();
  flash_erase(UPGRADE_ADDR, FLASH_PAGE_SIZE);
  flash_write(UPGRADE_ADDR, &up, sizeof(up));
  __enable_irq();

  NVIC_SystemReset();      /* 进入 Boot 执行升级 */
}
```

触发回退：`up.flag = 0xAA;`，其余字段无需填写（Boot 使用 `fw[1]` 的已记录值）。

### 5.5 升级结果判断

复位回 APP 后读取 `Upgrade_t`（Flash 0x0807E000）：

- `flag == 0x5A` 且 `fw[0].crc != 上次记录的 crc` → 升级成功
- `flag == 0x5A` 且 `fw[0].crc == 上次` → 本次无升级（或升级失败已回退到上一版本）
- 读取注意：Flash 空白为 `0xFFFFFFFF`，判断有效数据需同时 `> 0` 且 `!= 0xFFFFFFFF`

### 5.6 固件文件要求

| 项 | 要求 |
|----|------|
| 存放位置 | 4G 模块 UFS 文件系统 |
| 文件名 | `iot_update.bin`（Boot 硬编码，见 `UPGRADE_FILE_NAME`） |
| 大小 | 与 `upgrade.size` 完全一致（Boot 用 QFLST 严格校验，不符即放弃） |
| 最大大小 | **224KB**（运行区容量 = 保底区容量） |
| CRC | 与 `upgrade.crc` 一致（IEEE 802.3，见 5.2） |
| 内容 | 可直接从 0x08006000 执行的 APP 固件镜像（含向量表） |

## 6. 调试串口

- **USART1，115200，8N1**（Boot 全部诊断输出）
- Boot 启动打印：`Boot Loader... 2.3.0.105`
- 升级过程关键打印（供联调对照）：
  - `upgrade.flag=0x3A, fw_flag=1, status=0x1, up_count=3` —— 启动时元数据摘要
  - `preserve: copy run -> backup` / `backup already current, skip preserve` —— 保底步骤
  - `modem init successful!` / `modem no RDY, retry x/y` —— 模块初始化
  - `QFOPEN fd:0`、`up file size 204800`、`up file size ok!` —— 文件操作
  - `flash write offset:0 size:40960` —— 分块写进度
  - `up crc:0xXXXX`、`up file crc ok!` —— 下载校验
  - `state save flag:0x5A fw0:0xXXXX/204800 fw1:0xXXXX/204800` —— 提交
  - `QFREAD fail ret:x offset:y` —— 分块读取失败（会重试）
  - `app invalid, auto rollback` / `trial exhausted, auto rollback` —— 自动回退

## 7. 注意事项与常见问题

1. **升级文件必须由 APP 提前放入模块 UFS**。Boot 只负责"读 UFS 文件 → 写 Flash"，不负责联网下载固件。文件获取流程（HTTP/MQTT/OTA 等）在 APP 侧实现
2. **`upgrade.size/crc` 必须真实**：Boot 用 QFLST 严格比对大小、用 CRC32 严格校验内容，任一不符都会放弃升级（重试 5 次后走 0xAA 回退复位）
3. **跳转前状态**：Boot 不关 SysTick、不复位外设；APP 启动应自行初始化全部外设。IWDT 在跳转时未使能（`main()` 中使能代码在 `iap_init()` 之后），APP 如自行使能 IWDT 则需自行喂狗
4. **结构体对齐**：`Upgrade_t` 是 1 字节打包（`#pragma pack(1)`），APP 侧编译器必须同样打包，否则与 Boot 读写字节错位
5. **写元数据的时机**：只能在"准备好文件且确认 size/crc"之后写 0x3A 并复位；中途复位（如看门狗）会导致 Boot 重复执行升级——这是设计行为（保底区只会在新固件下载前被更新，重复升级无害）
6. **APP 固件最大 224KB**：超出会写入失败（Flash 越界写入 FMC 错误 → CRC 不符 → 重试耗尽）
7. **trial 回退必须主动确认**：启用 trial（写 `status=0x01` 请求升级）后，新固件若在 3 次启动内未写 `status=0x02`，会被回退到上一个版本。确认代码不要放在可能被跳过的路径上
8. **版本配套**：本文档针对 Boot **v2.4.0.106**（`VERSION`，见 `Include/main.h`，改版本号时以文件为准）；若 Boot 版本变化，以对应版本源码中的 `iap.h`/`upgrade_ctrl.h` 为准
