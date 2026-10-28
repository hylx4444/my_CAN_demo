"""
文件作用：读取 S2 Mini 的原生 USB 日志，在断电或重启导致串口消失后自动重连。
包含模块：串口与设备身份匹配、只读日志采集、重连等待、UTF-8 日志文件保存。
调用方式：python tools/monitor_esp32s2.py --port COM6 --follow-single-esp；按 Ctrl+C 结束。
依赖说明：使用已有 pyserial，不依赖 ESP-IDF，不发送应用数据或烧录命令。
注意事项：USB 控制线可能影响复位；检查正常热点启动时应先关闭监听，再断电重启。
"""

import argparse
import codecs
from datetime import datetime
from pathlib import Path
import sys
import time

import serial
from serial.tools import list_ports


def find_port(port_name, device_serial, follow_single_esp=False):
    """
    作用：寻找指定串口，或按首次识别的 USB 序列号追踪同一块 S2 Mini。
    参数：port_name 为首次端口；device_serial 为已记录序列号，首次为 None；
          follow_single_esp 仅在只连接一块 S2 Mini 时允许跟随下载与运行模式的身份变化。
    返回：匹配的端口信息；设备尚未连接时返回 None。
    调用方式：采集循环调用 info = find_port(port_name, device_serial)。
    """
    ports = list(list_ports.comports())
    for info in ports:
        if device_serial is not None:
            if info.vid == 0x303A and info.serial_number == device_serial:
                return info
        elif info.device.casefold() == port_name.casefold():
            return info
    if follow_single_esp:
        candidates = [info for info in ports if info.vid == 0x303A and info.pid == 0x0002]
        if len(candidates) == 1:
            return candidates[0]
    return None


def monitor(port_name, log_path, follow_single_esp=False):
    """
    作用：持续读取 USB 控制台，记录输出，并在设备断开后等待重新枚举。
    参数：port_name 为起始端口；log_path 为日志路径；follow_single_esp 允许跟随唯一 S2 Mini。
    返回：正常退出返回 0；首次端口不是乐鑫 USB 设备时返回 1。
    调用方式：main() 调用 monitor(args.port, args.log)，用户按 Ctrl+C 结束。
    """
    log_path.parent.mkdir(parents=True, exist_ok=True)
    device_serial = None
    last_notice = None
    with log_path.open("a", encoding="utf-8") as log:
        try:
            while True:
                connection = None
                try:
                    info = find_port(port_name, device_serial, follow_single_esp)
                    if info is None:
                        available = ", ".join(
                            f"{item.device} 序列号={item.serial_number}"
                            for item in list_ports.comports() if item.vid == 0x303A
                        ) or "无乐鑫串口"
                        notice = f"等待原设备；当前枚举：{available}"
                        if notice != last_notice:
                            print(notice, flush=True)
                            last_notice = notice
                        time.sleep(0.5)
                        continue
                    if info.vid != 0x303A:
                        print("所选端口不是乐鑫原生 USB 设备，请重新确认 S2 Mini 端口。", flush=True)
                        return 1
                    if not info.serial_number:
                        raise serial.SerialException("USB 序列号暂不可用，等待设备完成枚举")
                    device_serial = info.serial_number
                    # SDK 将 RTS 的下降沿且 DTR=1 解释为进入下载模式，因此监听时保持 RTS=1。
                    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
                    connection.dtr = True
                    connection.rts = True
                    connection.port = info.device
                    connection.open()
                    marker = (f"\n[{datetime.now().isoformat(timespec='seconds')}] "
                              f"已连接 {info.device}，USB 序列号={device_serial}\n")
                    print(marker, end="", flush=True)
                    log.write(marker)
                    log.flush()
                    last_notice = None
                    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
                    while True:
                        data = connection.read(1024)
                        if data:
                            output = decoder.decode(data)
                            print(output, end="", flush=True)
                            log.write(output)
                            log.flush()
                except (serial.SerialException, OSError) as error:
                    notice = f"USB 暂不可用，正在自动重连：{error}"
                    if notice != last_notice:
                        print(notice, flush=True)
                        log.write(f"\n{notice}\n")
                        log.flush()
                        last_notice = notice
                    time.sleep(0.5)
                finally:
                    if connection is not None:
                        # 关闭前先降低 DTR，避免系统释放 RTS 时请求进入下载模式。
                        # 某些驱动仍可能触发普通复位，因此不能用退出监听后的热点状态判故障。
                        try:
                            if connection.is_open:
                                connection.dtr = False
                        except OSError:
                            pass
                        connection.close()
        except KeyboardInterrupt:
            print(f"\n已停止。日志保存在：{log_path.resolve()}", flush=True)
            return 0


def main():
    """
    作用：读取命令行参数并启动日志采集。
    参数：无；--port 指定起始端口，--log 修改日志路径，--follow-single-esp 跟随模式切换。
    返回：采集程序退出码。
    调用方式：通过 Python 直接执行本文件，由文件入口调用。
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="S2 Mini USB 日志采集与自动重连，不烧录固件")
    parser.add_argument("--port", required=True, help="S2 Mini 的起始端口，例如 COM6")
    parser.add_argument(
        "--follow-single-esp", action="store_true",
        help="只连接一块 S2 Mini 时，允许跟随端口号及下载/运行模式序列号变化",
    )
    parser.add_argument(
        "--log", type=Path,
        default=Path(__file__).resolve().parent.parent / "build/logs/esp32s2-usb.log",
        help="UTF-8 日志文件路径",
    )
    args = parser.parse_args()
    print(f"日志保存位置：{args.log.resolve()}；按 Ctrl+C 结束。", flush=True)
    return monitor(args.port, args.log, args.follow_single_esp)


if __name__ == "__main__":
    raise SystemExit(main())
