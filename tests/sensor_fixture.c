/*
 * 文件作用：生成供集成测试使用的真实传感器格式遥测帧，不用于模拟服务的默认数据源。
 * 包含模块：协议页固定样例、过期和离线状态选择、二进制标准输出。
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include "demo_protocol.h"

/*
 * 作用：把协议页样例经生产解码器和 UART 编码器输出为二进制夹具。
 * 参数：argc 为参数数量；argv 可选 --stale 或 --bus-off，分别生成过期或离线状态帧。
 * 返回：成功返回 0；无法解码样例或输出失败返回 1；参数错误返回 2。
 * 调用方式：仅由 Node.js 测试执行 build/host/sensor_fixture.exe，不代表已接真实硬件。
 */
int main(int argc, char **argv)
{
    demo_sensor_t sensor;
    demo_sample_t sample;
    uint16_t data[8] = {1U, 0xD9U, 0U, 0x1CU, 2U, 0U, 0U, 0U};
    uint16_t wire[DEMO_FRAME_MAX], length, i;
    uint32_t now_ms = 200U;
    bool off = false;
    if (argc > 2) return 2;
    if (argc == 2) {
        if (strcmp(argv[1], "--stale") == 0) now_ms = 1100U;
        else if (strcmp(argv[1], "--bus-off") == 0) off = true;
        else return 2;
    }
    demo_sensor_init(&sensor);
    if (!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 100U)) return 1;
    demo_sensor_snapshot(&sensor, now_ms, off, &sample);
    length = demo_encode(&sample, 42U, wire);
    _setmode(_fileno(stdout), _O_BINARY);
    for (i = 0; i < length; ++i) if (fputc(wire[i], stdout) == EOF) return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}
