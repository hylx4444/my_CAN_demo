/*
 * 文件作用：依据传感器协议样例验证真实 CAN 解码、倍率、异常数据和失效状态。
 * 包含模块：正负温度、协议元数据、数值边界、最后有效帧时间、总线离线、时间回绕、UART 联通。
 */
#include <assert.h>
#include <stdio.h>
#include "demo_protocol.h"

/*
 * 作用：运行协议样例和状态转换测试，保证真实采样不会被误标为模拟或长期保鲜。
 * 参数：无。
 * 返回：全部断言通过返回 0，失败时终止进程。
 * 调用方式：由 tools/build_host.ps1 编译并运行 test_sensor.exe。
 */
int main(void)
{
    demo_sensor_t sensor;
    demo_sample_t sample, decoded;
    demo_frame_t frame;
    demo_parser_t parser;
    uint16_t data[8] = {1U, 0xD9U, 0U, 0x1CU, 2U, 0xA5U, 0x5AU, 0xFFU};
    uint16_t wire[DEMO_FRAME_MAX], length, i;
    demo_sensor_init(&sensor);
    demo_sensor_snapshot(&sensor, 0U, false, &sample);
    assert(sample.flags == DEMO_FLAG_SENSOR && sample.can_rx == 0U);
    assert(demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 100U));
    demo_sensor_snapshot(&sensor, 200U, false, &sample);
    assert(sample.temperature_centi == 2170 && sample.humidity_centi == 5400U);
    assert(sample.flags == (DEMO_FLAG_SENSOR | DEMO_FLAG_VALID));
    assert(sample.can_rx == 1U && sample.can_tx == 0U);
    /* 负温度 -12.3 摄氏度：原始补码 FF85，小端 85 FF。 */
    data[1] = 0x85U; data[2] = 0xFFU;
    assert(demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 200U));
    assert(sensor.last.temperature_centi == -1230);
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID + 1U, true, 8U, data, 900U));
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, false, 8U, data, 900U));
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 7U, data, 900U));
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 9U, data, 900U));
    data[0] = 2U;
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 900U));
    data[0] = 1U; data[3] = 0xE9U; data[4] = 3U;
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 900U));
    data[3] = 0xE8U;
    data[1] = 0xCDU; data[2] = 0x0CU;
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 900U));
    data[1] = 0U; data[2] = 0x80U;
    assert(!demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 900U));
    assert(sensor.last.can_errors == 8U && sensor.last.can_rx == 2U && sensor.last_rx_ms == 200U);
    demo_sensor_snapshot(&sensor, 1199U, false, &sample);
    assert(sample.flags & DEMO_FLAG_VALID);
    demo_sensor_snapshot(&sensor, 1200U, false, &sample);
    assert(!(sample.flags & DEMO_FLAG_VALID));
    assert(sample.temperature_centi == -1230);
    demo_sensor_snapshot(&sensor, 1201U, false, &sample);
    assert(!(sample.flags & DEMO_FLAG_VALID));
    demo_sensor_snapshot(&sensor, 300U, true, &sample);
    assert((sample.flags & DEMO_FLAG_BUS_OFF) && !(sample.flags & DEMO_FLAG_VALID));
    demo_sensor_snapshot(&sensor, 400U, false, &sample);
    assert((sample.flags & DEMO_FLAG_VALID) && !(sample.flags & DEMO_FLAG_BUS_OFF));
    /* 输入元素的高 8 位不是线路数据，必须被屏蔽；NA 字节允许非零。 */
    data[1] = 0xD9U; data[2] = 0U;
    for (i = 0; i < 8U; ++i) data[i] |= 0xAB00U;
    assert(demo_sensor_receive(&sensor, SENSOR_CAN_ID, true, 8U, data, 0xFFFFFFF0UL));
    demo_sensor_snapshot(&sensor, 500U, false, &sample);
    assert(sample.flags & DEMO_FLAG_VALID);
    assert(sample.humidity_centi == 10000U);
    length = demo_encode(&sample, 42U, wire);
    demo_parser_init(&parser);
    for (i = 0; i < length; ++i) {
        if (demo_parser_feed(&parser, wire[i], &frame)) {
            assert(demo_decode(&frame, &decoded));
            assert(decoded.flags == sample.flags && decoded.temperature_centi == 2170);
        }
    }
    demo_sensor_snapshot(&sensor, 1000U, false, &sample);
    assert(!(sample.flags & DEMO_FLAG_VALID));
    puts("PASS: sensor document vector, signed values, metadata, bounds, freshness, bus-off, wraparound, UART");
    return 0;
}
