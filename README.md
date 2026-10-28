# CAN 温湿度传感器与 S2 Mini 网页工程

当前硬件：LAUNCHXL-F2800157、WEMOS S2 Mini、用户提供协议页的 CAN 温湿度传感器。

数据路径：**传感器自定义 CAN → F2800157 → UART → ESP32-S2 → Wi-Fi 热点 → 手机网页**。

传感器使用厂家自定义 CAN 协议。本工程按该协议接收，**不包含 CANopen 协议栈**。热点沿用名称 CANopen-Demo，该名称不表示传感器使用 CANopen。

## 本次适配内容

| 部分 | 变化 |
| --- | --- |
| 157 | 关闭静默回环，启用 GPIO4/5 的 CAN 收发器接口；改为 100 kbit/s、扩展帧接收 |
| 传感器解析 | 检查 ID、帧类型、DLC、地址、温湿度范围；支持负温度和小端数据 |
| 采样状态 | 有效 CAN 帧超过 1 秒未更新则失效；持续 UART 上报不会让旧采样重新有效 |
| 串口 | 保持 v1 的 32 字节帧和 0.01 单位，每 200ms 上报；新增真实源与 Bus-off 状态位 |
| ESP | 目录改为 firmware/esp32s2；目标 esp32s2，40MHz 晶振、4MB Flash，使用原生 USB 日志 |
| ESP-IDF 6 | 明确依赖 esp_driver_uart / esp_driver_gpio，修正网页嵌入文件链接符号 |
| 网页 | 区分真实采样和模拟源，区分传感器失效与 UART 离线；温度轴可显示负温度 |
| 构建 | 新入口 tools/build_esp32s2.ps1，保留旧命令兼容入口 |

## 传感器参数

| 项目 | 默认值 |
| --- | --- |
| 波特率 | 100 kbit/s |
| 帧类型 / ID | 29 位扩展数据帧 / 0x18FF0000 |
| DLC / 地址 | 8 / Byte0=1 |
| 上报周期 | 约 100ms，传感器主动上报 |
| 温度 | Byte1 低字节、Byte2 高字节，有符号数，除以 10 得到 °C |
| 湿度 | Byte3 低字节、Byte4 高字节，无符号数，除以 10 得到 %RH |

协议页样例 `01 D9 00 1C 02 00 00 00` 应显示 **21.7°C、54.0%RH**。可配置常量位于 `shared/demo_protocol.h`：SENSOR_CAN_BITRATE、SENSOR_CAN_ID、SENSOR_ADDRESS、SENSOR_TIMEOUT_MS。

## 157 构建与 CCS 调试

在工程根目录运行：

```powershell
.\tools\build_c2000.ps1 -Configuration RAM
.\tools\build_c2000.ps1 -Configuration FLASH
```

输出位于 `build/c2000/RAM` 和 `build/c2000/FLASH`，程序名均为 `can_uart_demo.out`。

你已经导入的 `can_uart_demo` CCS 工程通过链接引用公共源码，**不用重新导入**。选择 `LAUNCHXL_RAM`，执行 **Build Project(s)**，然后 **Debug Project** 加载新固件。旧回环程序的断点位置已经改变，建议清理旧断点。

新电脑可运行 `.\tools\generate_ccs_project.ps1`，导入 `build/ccs/can_uart_demo.projectspec`。TI 构建脚本支持 `-C2000Ware`、`-Compiler` 参数，也支持 `C2000WARE_ROOT`、`C2000_CGT_ROOT` 环境变量。

Watch 建议：

| 变量 | 含义与预期 |
| --- | --- |
| g_uptime_ms | 毫秒运行时间 |
| g_can_rx | 校验通过的接收帧，接好传感器后约每秒增加 10 |
| g_can_tx | 应用未发送 CAN 请求，因此为 0；硬件 ACK 不统计 |
| g_can_frames / g_can_rejected | 邮箱读取帧数 / 被软件拒绝的帧数 |
| g_last_can_id / g_last_dlc | 正常为 0x18FF0000 / 8 |
| g_sensor_online | 有效采样为 1，未接入、超时或 Bus-off 为 0 |
| g_temperature_centi / g_humidity_centi | 除以 100 后为摄氏度与相对湿度，温度变量为有符号数 |
| g_can_errors | 被拒报文、邮箱覆盖事件、Bus-off 事件之和 |
| g_can_overruns / g_bus_off_count | 邮箱覆盖事件数 / 总线离线事件数 |
| g_can_rx_error_count / g_can_tx_error_count | 控制器 REC / TEC 硬件错误计数 |
| g_sensor_age_ms | 最近有效 CAN 帧的年龄；从未收到时为 0xFFFFFFFF |

无连接线时，g_can_rx=0、g_sensor_online=0 是预期结果。不能再使用“发送和接收计数一起增长”的旧回环判断方法。暂停 CPU 较久会造成邮箱覆盖，单步调试不适合验证连续接收速率。

## S2 Mini 构建和烧录

已在本机找到 ESP-IDF v6.0.3：

- SDK：`D:\work_study\.espressif\v6.0.3\esp-idf`
- 工具：`C:\Espressif\tools`
- 构建入口会按安装器的配置加载当前进程环境，不修改系统环境，也不自动安装组件。

```powershell
# 正常模式：从 157 的 UART 接收状态
.\tools\build_esp32s2.ps1

# 本地模拟：只验证 S2 Mini 热点和网页，不用接 157
.\tools\build_esp32s2.ps1 -LocalSimulation

# 准备好下载连接后再烧录，把 COM7 换为实际串口
.\tools\build_esp32s2.ps1 -Flash -Port COM7
```

两种模式分别使用 `build/esp32s2/uart` 和 `build/esp32s2/local-sim`，配置彼此独立。旧命令 `build_esp8684.ps1` 会转到 S2 Mini 构建并提醒，不再生成 ESP8684 固件。

普通串口模式没有数据时不会自动造数。本地模拟模式显式标记模拟来源，CAN 计数保持 0。S2 Mini 使用原生 USB，若首次下载没有出现端口，可按住 BOOT（GPIO0），按一次 RESET，再松开 BOOT，重新查看端口；下载过程中不要断电。

手机连接热点 **CANopen-Demo**，密码 **can-demo-123**，打开 **http://192.168.4.1**。网页资源均内嵌，不依赖互联网。手机提示无互联网时保持连接；未实现自动弹出门户。

## 在没有 ESP-IDF 的电脑上烧录

首次上传工程及另一台电脑获取文件，见 [GitHub 上传与同步步骤](docs/github-guide.md)。

可以使用项目内的预编译包，无需复制 build 目录或安装完整开发环境。在当前电脑执行 `.\tools\package_esp32s2.ps1` 后，把 `releases/esp32s2` 和烧录脚本一起提交到 GitHub。

另一台电脑安装 Python 3.10+ 和 `esptool==5.6.0`，拉取整个项目，再执行 `.\tools\flash_prebuilt_esp32s2.ps1 -Port COM7`。该脚本只校验、烧录，不调用 ESP-IDF。详细步骤见 [预编译包说明](releases/esp32s2/README.md)。

## 最小接线

先断电；两板各自用 USB 供电，不互连两板电源脚。

| 157 | S2 Mini |
| --- | --- |
| J1 的 IO.29 / SCIA_TX，第 4 脚 | GPIO5 / UART1_RX |
| GND | GND |
| J1 的 IO.28 / SCIA_RX，第 3 脚 | 不连接；S2 Mini GPIO4 预留且保持输入，当前只接收遥测 |

S2 Mini 的空孔需要焊排针或焊线，不能靠松插接触。UART 为 3.3V TTL、115200、8N1。157 的 S2 开关设为 SEL1=1、SEL2=0；S2 Mini 的 GPIO19/20 留给 USB，不用于数据 UART。

诊断固件 `s2-diag-1` 在 `/api/status` 中增加 `esp_uptime_ms`（ESP 自身时间）、`uart_rx_bytes`（收到的原始字节数）、UART 帧错误/溢出/BREAK 计数、启动阶段和热点运行状态。旧字段 `uptime_ms` 仍是 157 上报的时间，未收到遥测时为 0。原针位图里的 GPIO4 TX 标注为历史预留接口，当前固件不驱动 GPIO4。

USB 日志可用 `.\.venv\Scripts\python.exe tools/monitor_esp32s2.py --port COM6 --follow-single-esp` 采集，只连接一块 S2 Mini 时可跟随下载/运行模式的身份变化。原生 USB 的控制线可能影响复位；核对正常热点启动时应关闭串口工具后再断电启动。`boot_phase=6` 表示 HTTP 已就绪，`ap_running=true` 表示驱动已报告热点启动。

传感器：白线 CANH 接 J14 HI，黄线 CANL 接 J14 LO，黑线接共地，红线接符合规格的 5V 电源。按用户提供的说明书图，保持 USB 端朝上时，板底部 J14 三针从左到右为 **GND、LO、HI**。

157 的 S4 / CAN ROUTE 选择 XCVR，JP9 接入板端 120Ω，传感器端需确认是否自带终端电阻。传感器电流尚未提供，若从 157 的 5V 排针取电，需先核对供电能力。针位图见 [docs/wiring-pin-locations.png](docs/wiring-pin-locations.png)。

## 电脑测试

```powershell
.\tools\build_host.ps1
node --test .\tests\test_simulator.mjs
node .\tools\simulator.mjs
```

电脑预览地址为 **http://127.0.0.1:8080**，仅监听本机。电脑模拟器不代表真实硬件通信；页面会明确标识电脑模拟。暂停、数据故障、恢复控制仅在电脑模拟模式显示。

统一验证：`.\tools\verify.ps1`；加入 S2 Mini 双模式构建：`.\tools\verify.ps1 -IncludeEsp32S2`；无 TI 工具链时使用 `-SkipC2000`。

## 验证范围与限制

原生 C 测试覆盖协议页样例、负温度、湿度范围、倍率溢出、错误帧类型/ID/DLC/地址、数据过期、Bus-off、毫秒回绕、CRC、分段接收及恢复。Node.js 测试覆盖 C 夹具 → UART 帧 → 网页状态、传感器失效与串口离线的区分。

已构建 157 RAM/Flash、现有 CCS RAM 工程，以及 ESP-IDF v6.0.3 下的 S2 Mini 正常/本地模拟固件。

**2026-10-28，用户完成真实硬件联调：网页已显示传感器温湿度，改变传感器温湿度后，网页数值也相应变化。**这验证了当前固件与接线组合下的传感器 CAN → 157 → UART → S2 Mini → 手机网页链路。预编译应用与当前构建输出已核对 SHA256 一致。详细记录见 [硬件联调记录](docs/hardware-validation.md)。

长期运行、传感器断线恢复、整机断电重启等专项稳定性测试尚未单独记录；热点异常的原始原因未完成单变量复现，不能仅依据最终成功归因于某一条修改。

协议布局见 [docs/uart_protocol.md](docs/uart_protocol.md)。
