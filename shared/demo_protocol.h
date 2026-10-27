/*
 * 文件作用：声明两种固件与电脑模拟器共用的遥测协议接口。
 * 包含模块：协议常量、状态位、采样与解析器结构、UART 编解码、真实传感器缓存及模拟接口。
 */
#ifndef DEMO_PROTOCOL_H
#define DEMO_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* 每个 uint16_t 元素的低 8 位表示一个线路字节，兼容 char 为 16 位的 C28x。
 * 不直接发送结构体，也不使用 sizeof() 计算线路帧长度。 */
#define DEMO_VERSION 1U
#define DEMO_TYPE_TELEMETRY 1U
#define DEMO_PAYLOAD_MAX 48U
#define DEMO_FRAME_MAX (DEMO_PAYLOAD_MAX + 10U)
#define DEMO_TELEMETRY_LEN 22U
#define DEMO_FLAG_VALID 0x0001U
#define DEMO_FLAG_SIMULATED 0x0002U
#define DEMO_FLAG_HW_LOOPBACK 0x0004U
#define DEMO_FLAG_HOST 0x0008U
#define DEMO_FLAG_ESP_LOCAL 0x0010U
#define DEMO_FLAG_SENSOR 0x0020U
#define DEMO_FLAG_BUS_OFF 0x0040U

/* 传感器协议：100 kbit/s、扩展数据帧；设备地址位于第 0 字节。 */
#define SENSOR_CAN_BITRATE 100000UL
#define SENSOR_CAN_ID 0x18FF0000UL
#define SENSOR_ADDRESS 1U
#define SENSOR_TIMEOUT_MS 1000UL

/* 采样快照：温湿度以百分之一单位保存，计数和状态随遥测帧上报。 */
typedef struct {
    uint32_t uptime_ms; /* 源端运行时间，单位为毫秒。 */
    int16_t temperature_centi; /* 温度，单位为 0.01 摄氏度。 */
    uint16_t humidity_centi; /* 相对湿度，单位为 0.01 %RH。 */
    uint32_t can_tx;
    uint32_t can_rx;
    uint32_t can_errors;
    uint16_t flags;
} demo_sample_t;

/* 完成帧头和 CRC 校验后的协议帧，载荷长度按线路字节计。 */
typedef struct {
    uint16_t type, sequence, length;
    uint16_t payload[DEMO_PAYLOAD_MAX];
} demo_frame_t;

/* 流式接收状态：缓存残帧，并累计 CRC 与格式错误。 */
typedef struct {
    uint16_t buffer[DEMO_FRAME_MAX];
    uint16_t used;
    uint32_t crc_errors, format_errors;
} demo_parser_t;

/* 保存最近一次有效 CAN 采样；UART 周期上报不能刷新此接收时间。 */
typedef struct {
    demo_sample_t last;
    uint32_t last_rx_ms;
    bool have_sample;
} demo_sensor_t;

/*
 * 作用：初始化传感器缓存和计数，不生成模拟值。
 * 参数：sensor 为待初始化的状态实例。
 * 返回：无。
 * 调用方式：接收前调用 demo_sensor_init(&sensor)。
 */
void demo_sensor_init(demo_sensor_t *sensor);

/*
 * 作用：校验自定义 CAN 报文，将原始 0.1 单位换算为遥测的 0.01 单位。
 * 参数：sensor 为状态；id 为帧 ID；extended 为扩展帧标志；dlc 为长度；
 *       data 为至少 8 个元素的接收数组；now_ms 为接收毫秒数。
 * 返回：合法时为 true；非法时为 false，保留旧值和旧接收时间。
 * 调用方式：demo_sensor_receive(&sensor, id, extended, dlc, data, now_ms)。
 */
bool demo_sensor_receive(demo_sensor_t *sensor, uint32_t id, bool extended,
                         uint16_t dlc, const uint16_t *data, uint32_t now_ms);

/*
 * 作用：生成遥测快照，未接收、超时或总线离线时清除有效位。
 * 参数：sensor 为缓存；now_ms 为当前毫秒数；bus_off 为总线离线状态；sample 为输出。
 * 返回：无；旧值仍保留，显示前必须检查 DEMO_FLAG_VALID。
 * 调用方式：每次上报前调用 demo_sensor_snapshot(&sensor, now_ms, bus_off, &sample)。
 */
void demo_sensor_snapshot(const demo_sensor_t *sensor, uint32_t now_ms,
                          bool bus_off, demo_sample_t *sample);

/*
 * 作用：计算 CRC-16/CCITT-FALSE 校验值。
 * 参数：data 为输入数组，每个元素仅取低 8 位；count 为线路字节数。
 * 返回：16 位校验值。
 * 调用方式：crc = demo_crc16(data, count)，输入缓冲区至少包含 count 个元素。
 */
uint16_t demo_crc16(const uint16_t *data, uint16_t count);
/*
 * 作用：将采样快照编码为固定 32 字节遥测帧。
 * 参数：sample 为只读采样快照；sequence 为帧序号；wire 为输出数组。
 * 返回：线路帧长度 32，每个数组元素保存一个线路字节。
 * 调用方式：length = demo_encode(&sample, sequence, wire)，wire 至少有 DEMO_FRAME_MAX 个元素。
 */
uint16_t demo_encode(const demo_sample_t *sample, uint16_t sequence, uint16_t *wire);
/*
 * 作用：从已校验的遥测帧提取采样值并检查湿度范围。
 * 参数：frame 为解析器输出帧；sample 为接收解码结果的结构体。
 * 返回：帧类型、长度及湿度范围合法时返回 true；返回 false 时不可使用输出值。
 * 调用方式：先用 demo_parser_feed() 得到完整帧，再调用 demo_decode(&frame, &sample)。
 */
bool demo_decode(const demo_frame_t *frame, demo_sample_t *sample);
/*
 * 作用：初始化流式解析器并清零错误计数。
 * 参数：parser 为待初始化的解析器实例。
 * 返回：无。
 * 调用方式：首次接收前调用 demo_parser_init(&parser)。
 */
void demo_parser_init(demo_parser_t *parser);
/*
 * 作用：丢弃未完成的接收帧，保留已有错误计数。
 * 参数：parser 为已初始化的解析器实例。
 * 返回：无。
 * 调用方式：字节间隔超时后调用 demo_parser_reset(&parser)，再继续送入新字节。
 */
void demo_parser_reset(demo_parser_t *parser);
/*
 * 作用：送入一个线路字节，寻找帧头并完成长度与 CRC 校验。
 * 参数：parser 为解析器状态；octet 为新接收字节；frame 为完整帧输出地址。
 * 返回：输出完整有效帧时返回 true；数据不足或校验失败时返回 false。
 * 调用方式：逐字节调用 demo_parser_feed(&parser, octet, &frame)，返回 true 后再解码 frame。
 */
bool demo_parser_feed(demo_parser_t *parser, uint16_t octet, demo_frame_t *frame);
/*
 * 作用：生成演示用的 8 字节 CAN 模拟载荷。
 * 参数：step 为递增采样步数；can_data 为至少包含 8 个 uint16_t 元素的输出数组。
 * 返回：无。
 * 调用方式：demo_model_payload(step, can_data)，每次采样将 step 加一。
 */
void demo_model_payload(uint32_t step, uint16_t *can_data);
/*
 * 作用：从演示 CAN 载荷提取温湿度，只更新采样结构中的这两个字段。
 * 参数：can_data 为至少包含 8 个元素的演示 CAN 载荷；sample 为待更新的采样结构体。
 * 返回：无。
 * 调用方式：确认回环帧正确后调用 demo_sample_from_can(can_data, &sample)。
 */
void demo_sample_from_can(const uint16_t *can_data, demo_sample_t *sample);

#endif
