/*
 * 文件作用：在电脑上验证公共 C 协议模块的编码、解析与异常恢复行为。
 * 包含模块：CRC 标准向量、数据格式、负温度、分段与连续帧、损坏帧恢复、超时清理、模拟源边界。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "demo_protocol.h"

/*
 * 作用：将一段线路数据逐字节送入解析器，统计完整帧数量。
 * 参数：p 为已初始化的解析器；wire 为输入数组；n 为元素数量；f 为最近解析帧的输出地址。
 * 返回：返回本次输入期间成功解析的完整帧数量。
 * 调用方式：测试内部调用 count = feed(&parser, wire, length, &frame)。
 */
static unsigned feed(demo_parser_t *p, const uint16_t *wire, unsigned n, demo_frame_t *f)
{
    unsigned found = 0;
    for (unsigned i = 0; i < n; ++i) if (demo_parser_feed(p, wire[i], f)) ++found;
    return found;
}

/*
 * 作用：依次执行公共协议断言，验证格式、校验、容错和模拟模型。
 * 参数：无。
 * 返回：全部断言通过返回 0；断言失败时测试进程终止。
 * 调用方式：通过 tools/build_host.ps1 编译后自动运行；也可直接执行 build/host/test_protocol.exe。
 */
int main(void)
{
    const uint16_t check[] = {'1','2','3','4','5','6','7','8','9'};
    demo_sample_t in = {0xFEDCBA98UL, -1234, 5678, 0x89ABCDEFUL, 0x12345678UL, 65537U, 7U}, out;
    demo_parser_t parser;
    demo_frame_t frame;
    uint16_t wire[DEMO_FRAME_MAX], damaged[DEMO_FRAME_MAX], can_data[8];
    assert(demo_crc16(check, 9) == 0x29B1U); /* CRC-16/CCITT-FALSE 的标准校验向量。 */
    assert(demo_encode(&in, 65535U, wire) == 32U);
    assert(wire[12] == 0x2EU && wire[13] == 0xFBU); /* -12.34 摄氏度的小端补码表示。 */
    demo_parser_init(&parser);
    assert(!demo_parser_feed(&parser, 0x00U, &frame));
    assert(!demo_parser_feed(&parser, 0xA5U, &frame)); /* 检查重叠帧头仍能正确同步。 */
    assert(feed(&parser, wire, 7, &frame) == 0U);
    assert(feed(&parser, wire + 7, 25, &frame) == 1U);
    assert(frame.sequence == 65535U && demo_decode(&frame, &out));
    assert(out.temperature_centi == in.temperature_centi && out.humidity_centi == in.humidity_centi);
    assert(out.uptime_ms == in.uptime_ms && out.can_tx == in.can_tx && out.can_rx == in.can_rx);
    assert(out.can_errors == in.can_errors && out.flags == in.flags);
    assert(feed(&parser, wire, 32, &frame) == 1U); /* 检查连续帧可直接解析。 */
    memcpy(damaged, wire, sizeof(wire)); damaged[18] ^= 0x80U;
    assert(feed(&parser, damaged, 32, &frame) == 0U);
    assert(feed(&parser, wire, 32, &frame) == 1U && parser.crc_errors == 1U);
    memcpy(damaged, wire, sizeof(wire)); damaged[6] = 0xFFU;
    assert(feed(&parser, damaged, 32, &frame) == 0U);
    assert(feed(&parser, wire, 32, &frame) == 1U && parser.format_errors >= 1U);
    /* 损坏长度仍在合法范围时会暂存后续帧头，CRC 失败后必须重新同步。 */
    memcpy(damaged, wire, sizeof(wire)); damaged[6] = 30U;
    assert(feed(&parser, damaged, 32, &frame) == 0U);
    assert(feed(&parser, wire, 32, &frame) == 1U);
    assert(feed(&parser, wire, 12, &frame) == 0U);
    demo_parser_reset(&parser); /* 模拟字节间隔超时，丢弃未完成帧。 */
    assert(feed(&parser, wire, 32, &frame) == 1U);
    memcpy(damaged, wire, sizeof(wire)); damaged[2] = 2U;
    assert(feed(&parser, damaged, 32, &frame) == 0U);
    assert(feed(&parser, wire, 32, &frame) == 1U);
    for (unsigned i = 0; i < 32; ++i) damaged[i] = wire[i] | 0xFF00U;
    assert(feed(&parser, damaged, 32, &frame) == 1U); /* 模拟 C28x 数据表示，确认只使用每个元素的低 8 位。 */
    frame.length = 21U; assert(!demo_decode(&frame, &out));
    frame.length = 22U; frame.type = 99U; assert(!demo_decode(&frame, &out));
    in.humidity_centi = 10001U; demo_encode(&in, 0U, wire);
    assert(feed(&parser, wire, 32, &frame) == 1U && !demo_decode(&frame, &out));
    in.humidity_centi = 10000U; in.temperature_centi = -32768;
    demo_encode(&in, 0U, wire); assert(feed(&parser, wire, 32, &frame) == 1U);
    assert(demo_decode(&frame, &out) && out.temperature_centi == -32768);
    /* 检查长噪声流下缓存有界，并遍历模拟模型数值验证完整帧编解码。 */
    for (unsigned i = 0; i < 10000; ++i) {
        (void)demo_parser_feed(&parser, (uint16_t)(i * 17U), &frame);
        assert(parser.used <= DEMO_FRAME_MAX);
    }
    demo_parser_reset(&parser);
    for (uint32_t step = 0; step < 300U; ++step) {
        demo_model_payload(step, can_data); demo_sample_from_can(can_data, &in);
        assert(in.temperature_centi >= 2200 && in.temperature_centi <= 2800);
        assert(in.humidity_centi >= 4500 && in.humidity_centi <= 6000);
        demo_encode(&in, (uint16_t)step, wire);
        assert(feed(&parser, wire, 32, &frame) == 1U && demo_decode(&frame, &out));
        assert(out.temperature_centi == in.temperature_centi);
    }
    puts("PASS: CRC vector, signed/32-bit wire format, split/continuous frames, noise, corruption, resync, timeout reset, model");
    return 0;
}
