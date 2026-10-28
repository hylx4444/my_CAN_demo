# S2 Mini 预编译烧录包

本目录应随项目提交到 GitHub，供没有 ESP-IDF 的电脑直接烧录。仅适用于 **WEMOS / LOLIN S2 Mini，ESP32-S2，4MB Flash**，不能烧给 ESP8684、ESP32-C3 等其他芯片。

## 文件与模式

| 子目录 | 用途 |
| --- | --- |
| uart | 正常采集：从 F2800157 接收真实传感器遥测，默认选择 |
| local-sim | S2 Mini 自己产生模拟值，先验证热点和网页；不验证 CAN 或 UART |

每个目录都包含完整启动所需的三段固件，不是只有应用程序：

| 文件 | Flash 地址 |
| --- | --- |
| bootloader.bin | 0x1000 |
| partition-table.bin | 0x8000 |
| application.bin | 0x10000 |

manifest.json 记录目标芯片、模式、编译框架和各文件 SHA256；SHA256SUMS.txt 可用于人工校验。烧录脚本先校验全部文件，再按上述地址写入，不执行整片擦除。

## 另一台 Windows 电脑怎么用

拉取整个项目后，不用安装 ESP-IDF、编译器、CMake、Ninja、VS Code 插件或本机的 SDK 路径。需要能正常运行的 **Python 3.10 或更新版本**与轻量烧录工具 esptool 5.x，以及 USB 数据线和可识别的设备端口。

首次安装烧录工具：

```powershell
python -m pip install "esptool==5.6.0"
```

在项目根目录执行，把 COM7 换成实际端口：

```powershell
# 不连接板卡也能先检查下载的固件是否完整
.\tools\flash_prebuilt_esp32s2.ps1 -CheckOnly

# 正常传感器采集模式
.\tools\flash_prebuilt_esp32s2.ps1 -Port COM7

# 仅热点和网页模拟模式
.\tools\flash_prebuilt_esp32s2.ps1 -Port COM7 -Mode local-sim
```

如 Python 命令是 py，可增加 `-Python py`；如使用独立 esptool.exe，可增加 `-EsptoolExe '工具完整路径'`，此时无需 Python，也无需 ESP-IDF。独立工具从 [乐鑫 esptool 官方发布页](https://github.com/espressif/esptool/releases) 获取，选择 Windows 对应版本。

如果 S2 Mini 首次连接没有端口，按住 BOOT，按一次 RESET，再松开 BOOT，重新检查设备端口。自动连接失败时可手动进入这个下载模式后重试；有端口但传输不稳定可增加 `-Baud 115200`。

Windows 下原生 USB 自动复位可能使 COM 端口失效。此时关闭所有串口监听窗口，手动进入下载模式，再查询新端口；例如下载端口为 COM5 时执行 `.\tools\flash_prebuilt_esp32s2.ps1 -Python .\.venv\Scripts\python.exe -Port COM5 -DownloadMode`。DownloadMode 跳过烧录前自动复位，只用于已经进入 ROM 下载模式的板卡，不改变烧录文件和地址。

当前包为 `s2-diag-1`：正常模式保留 GPIO5 接收遥测，GPIO4 不再作为发送输出，API 新增原始接收字节、硬件错误、热点事件和启动阶段。判断具体板卡是否写入新版，应以烧录成功提示及 API 中的 firmware 字段确认。

烧录完成后，连接热点 CANopen-Demo，密码 can-demo-123，浏览器访问 http://192.168.4.1。正常模式需要 157 和传感器连接后才显示有效测量；模拟模式会明确标识为模拟数据。

## 当前电脑怎么更新发布包

在已配置 ESP-IDF 的电脑上运行：

```powershell
.\tools\package_esp32s2.ps1
```

脚本重新编译两种模式，再更新本目录中的固件与校验清单。源码发生变化后，必须重新运行打包脚本并把新的发布包一起提交。另一台电脑只更新源码不会自动改变已编译固件。

本仓库无需携带整个 build 目录、Python 虚拟环境或 ESP-IDF 工具链。若另一台电脑还要修改源码并自行编译，则仍需要安装开发环境。本包没有包含任何 USB 驱动或烧录工具可执行文件。

## 验证范围

发布包已完成编译与 SHA256 校验，应用二进制与当前构建输出一致。2026-10-28，用户通过 ESP-IDF 成功烧录当前真实采集版本，并确认手机网页显示温湿度且随传感器变化。本地模拟版本通过编译与文件校验，其最新版本未另行记录硬件实测。

完整联调范围与烧录注意事项见 [硬件联调记录](../../docs/hardware-validation.md)。

参考：[esptool 安装说明](https://docs.espressif.com/projects/esptool/en/latest/esp32s2/installation.html)、[固件写入说明](https://docs.espressif.com/projects/esptool/en/latest/esp32s2/esptool/basic-commands.html)。
