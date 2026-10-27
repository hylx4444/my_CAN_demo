/*
 * 文件作用：在电脑上生成与固件格式相同的模拟遥测二进制数据流。
 * 包含模块：命令行参数解析、温湿度模拟模型、协议编码、标准输出、周期控制。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include "demo_protocol.h"

/*
 * 作用：解析启动参数，并持续或单次输出 32 字节遥测帧。
 * 参数：argc 为参数数量；argv 为参数数组，支持 --once、--step N 和 --period MS。
 * 返回：正常完成返回 0，写入失败返回 1，未知参数返回 2。
 * 调用方式：编译后执行 build/host/host_source.exe --once；通常由 simulator.mjs 创建子进程调用。
 */
int main(int argc, char **argv)
{
    uint32_t step = 0U, count = 0U, limit = 0U, period = 1000U;
    demo_sample_t sample = {0};
    uint16_t can_data[8], wire[DEMO_FRAME_MAX], i, length;
    for (int arg = 1; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--once") == 0) limit = 1U;
        else if (arg + 1 < argc && strcmp(argv[arg], "--step") == 0) step = (uint32_t)strtoul(argv[++arg], NULL, 10);
        else if (arg + 1 < argc && strcmp(argv[arg], "--period") == 0) period = (uint32_t)strtoul(argv[++arg], NULL, 10);
        else { fprintf(stderr, "Usage: host_source [--once] [--step N] [--period MS]\n"); return 2; }
    }
    _setmode(_fileno(stdout), _O_BINARY);
    for (;;) {
        demo_model_payload(step, can_data);
        demo_sample_from_can(can_data, &sample);
        sample.uptime_ms = step * 1000U;
        sample.can_tx = sample.can_rx = step + 1U;
        sample.flags = DEMO_FLAG_VALID | DEMO_FLAG_SIMULATED | DEMO_FLAG_HOST;
        length = demo_encode(&sample, (uint16_t)step, wire);
        for (i = 0U; i < length; ++i) {
            if (fputc((int)wire[i], stdout) == EOF) return 1;
        }
        if (fflush(stdout) != 0) return 1;
        ++count; ++step;
        if (limit && count >= limit) return 0;
        Sleep(period);
    }
}
