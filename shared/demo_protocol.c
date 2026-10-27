/*
 * 文件作用：实现与处理器字节宽度无关的遥测协议及模拟数据源。
 * 包含模块：小端读写、CRC16、遥测编解码、流式解析、模拟模型、自定义 CAN 传感器解析。
 */
#include "demo_protocol.h"

/*
 * 作用：将 16 位数值按小端顺序拆成两个线路字节。
 * 参数：p 为至少有两个元素的输出数组；v 为待拆分数值。
 * 返回：无。
 * 调用方式：模块内部调用 put16(p, v)。
 */
static void put16(uint16_t *p, uint16_t v)
{
    p[0] = v & 0xFFU;
    p[1] = (v >> 8U) & 0xFFU;
}

/*
 * 作用：按小端顺序组合两个线路字节。
 * 参数：p 指向至少包含两个元素的输入数组。
 * 返回：组合后的无符号 16 位数值。
 * 调用方式：模块内部使用 value = get16(p)。
 */
static uint16_t get16(const uint16_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8U));
}

/*
 * 作用：将 32 位数值按小端顺序写入四个线路字节。
 * 参数：p 为至少有四个元素的输出数组；v 为待写入数值。
 * 返回：无。
 * 调用方式：模块内部调用 put32(p, v)。
 */
static void put32(uint16_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16U));
}

/*
 * 作用：按小端顺序组合四个线路字节。
 * 参数：p 指向至少包含四个元素的输入数组。
 * 返回：组合后的无符号 32 位数值。
 * 调用方式：模块内部使用 value = get32(p)。
 */
static uint32_t get32(const uint16_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16U);
}

/*
 * 作用：计算 CRC-16/CCITT-FALSE 校验值。
 * 参数：data 为输入数组，每个元素仅取低 8 位；count 为线路字节数。
 * 返回：16 位校验值。
 * 调用方式：crc = demo_crc16(data, count)，输入缓冲区至少包含 count 个元素。
 */
uint16_t demo_crc16(const uint16_t *data, uint16_t count)
{
    uint16_t crc = 0xFFFFU, i, bit;
    for (i = 0; i < count; ++i) {
        crc ^= (uint16_t)((data[i] & 0xFFU) << 8U);
        for (bit = 0; bit < 8U; ++bit) {
            if (crc & 0x8000U) crc = (uint16_t)(((uint32_t)crc << 1U) ^ 0x1021UL);
            else crc = (uint16_t)((uint32_t)crc << 1U);
        }
    }
    return crc;
}

/*
 * 作用：将采样快照编码为固定 32 字节遥测帧。
 * 参数：s 为只读采样快照；sequence 为帧序号；wire 为输出数组。
 * 返回：线路帧长度 32，每个数组元素保存一个线路字节。
 * 调用方式：length = demo_encode(&sample, sequence, wire)，wire 至少有 DEMO_FRAME_MAX 个元素。
 */
uint16_t demo_encode(const demo_sample_t *s, uint16_t sequence, uint16_t *wire)
{
    uint16_t *p = wire + 8;
    wire[0] = 0xA5U; wire[1] = 0x5AU;
    wire[2] = DEMO_VERSION; wire[3] = DEMO_TYPE_TELEMETRY;
    put16(wire + 4, sequence); put16(wire + 6, DEMO_TELEMETRY_LEN);
    put32(p, s->uptime_ms);
    put16(p + 4, (uint16_t)s->temperature_centi);
    put16(p + 6, s->humidity_centi);
    put32(p + 8, s->can_tx); put32(p + 12, s->can_rx);
    put32(p + 16, s->can_errors); put16(p + 20, s->flags);
    put16(wire + 30, demo_crc16(wire + 2, 28U));
    return 32U;
}

/*
 * 作用：从已校验的遥测帧提取采样值并检查湿度范围。
 * 参数：f 为解析器输出帧；s 为接收解码结果的结构体。
 * 返回：帧类型、长度及湿度范围合法时返回 true；返回 false 时不可使用输出值。
 * 调用方式：先用 demo_parser_feed() 得到完整帧，再调用 demo_decode(&frame, &sample)。
 */
bool demo_decode(const demo_frame_t *f, demo_sample_t *s)
{
    uint16_t raw;
    if (f->type != DEMO_TYPE_TELEMETRY || f->length != DEMO_TELEMETRY_LEN) return false;
    s->uptime_ms = get32(f->payload);
    raw = get16(f->payload + 4);
    /* 显式还原有符号温度，避免依赖编译器对超范围强制转换的实现。 */
    s->temperature_centi = (int16_t)((raw & 0x8000U) ? (int32_t)raw - 65536L : raw);
    s->humidity_centi = get16(f->payload + 6);
    s->can_tx = get32(f->payload + 8); s->can_rx = get32(f->payload + 12);
    s->can_errors = get32(f->payload + 16); s->flags = get16(f->payload + 20);
    return s->humidity_centi <= 10000U;
}

/*
 * 作用：丢弃未完成的接收帧，保留已有错误计数。
 * 参数：p 为已初始化的解析器实例。
 * 返回：无。
 * 调用方式：字节间隔超时后调用 demo_parser_reset(&parser)，再继续送入新字节。
 */
void demo_parser_reset(demo_parser_t *p) { p->used = 0U; }
/*
 * 作用：初始化流式解析器并清零错误计数。
 * 参数：p 为待初始化的解析器实例。
 * 返回：无。
 * 调用方式：首次接收前调用 demo_parser_init(&parser)。
 */
void demo_parser_init(demo_parser_t *p)
{
    p->used = 0U; p->crc_errors = 0U; p->format_errors = 0U;
}

/*
 * 作用：移除接收缓存开头的指定元素，并前移剩余数据。
 * 参数：p 为解析器状态；count 为待移除的线路字节数，不能大于 p->used。
 * 返回：无。
 * 调用方式：解析器内部调用 discard(p, count)，用于重新同步或消费完整帧。
 */
static void discard(demo_parser_t *p, uint16_t count)
{
    uint16_t i;
    for (i = count; i < p->used; ++i) p->buffer[i - count] = p->buffer[i];
    p->used -= count;
}

/*
 * 作用：送入一个线路字节，寻找帧头并完成长度与 CRC 校验。
 * 参数：p 为解析器状态；octet 为新接收字节；f 为完整帧输出地址。
 * 返回：输出完整有效帧时返回 true；数据不足或校验失败时返回 false。
 * 调用方式：逐字节调用 demo_parser_feed(&parser, octet, &frame)，返回 true 后再解码 frame。
 */
bool demo_parser_feed(demo_parser_t *p, uint16_t octet, demo_frame_t *f)
{
    uint16_t length, total, i;
    if (p->used == DEMO_FRAME_MAX) discard(p, 1U);
    p->buffer[p->used++] = octet & 0xFFU;
    for (;;) {
        if (p->used < 2U) return false;
        if (p->buffer[0] != 0xA5U || p->buffer[1] != 0x5AU) { discard(p, 1U); continue; }
        if (p->used < 8U) return false;
        length = get16(p->buffer + 6);
        if (p->buffer[2] != DEMO_VERSION || length > DEMO_PAYLOAD_MAX) {
            ++p->format_errors; discard(p, 1U); continue;
        }
        total = length + 10U;
        if (p->used < total) return false;
        if (demo_crc16(p->buffer + 2, total - 4U) != get16(p->buffer + total - 2U)) {
            ++p->crc_errors; discard(p, 1U); continue;
        }
        f->type = p->buffer[3]; f->sequence = get16(p->buffer + 4); f->length = length;
        for (i = 0; i < length; ++i) f->payload[i] = p->buffer[8U + i];
        discard(p, total);
        return true;
    }
}

/*
 * 作用：生成演示用的 8 字节 CAN 模拟载荷。
 * 参数：step 为递增采样步数；data 为至少包含 8 个 uint16_t 元素的输出数组。
 * 返回：无。
 * 调用方式：demo_model_payload(step, can_data)，每次采样将 step 加一。
 */
void demo_model_payload(uint32_t step, uint16_t *data)
{
    uint16_t phase = (uint16_t)(step % 120U);
    uint16_t triangle = phase <= 60U ? phase : 120U - phase;
    put16(data, (uint16_t)(2200U + triangle * 10U));
    put16(data + 2, (uint16_t)(4500U + triangle * 25U));
    put16(data + 4, (uint16_t)step);
    data[6] = 0xC0U; data[7] = 0xDEU;
}

/*
 * 作用：从演示 CAN 载荷提取温湿度，只更新采样结构中的这两个字段。
 * 参数：data 为至少包含 8 个元素的演示 CAN 载荷；s 为待更新的采样结构体。
 * 返回：无。
 * 调用方式：确认回环帧正确后调用 demo_sample_from_can(can_data, &sample)。
 */
void demo_sample_from_can(const uint16_t *data, demo_sample_t *s)
{
    uint16_t raw = get16(data);
    s->temperature_centi = (int16_t)((raw & 0x8000U) ? (int32_t)raw - 65536L : raw);
    s->humidity_centi = get16(data + 2);
}

/*
 * 作用：初始化真实传感器缓存、接收时间及计数，不生成模拟测量值。
 * 参数：sensor 为待初始化的状态对象。
 * 返回：无。
 * 调用方式：系统启动时调用 demo_sensor_init(&sensor)。
 */
void demo_sensor_init(demo_sensor_t *sensor)
{
    demo_sample_t empty = {0};
    sensor->last = empty;
    sensor->last.flags = DEMO_FLAG_SENSOR;
    sensor->last_rx_ms = 0U;
    sensor->have_sample = false;
}

/*
 * 作用：校验厂家报文并换算温湿度，拒绝错误元数据和超出遥测表示范围的数据。
 * 参数：sensor 为状态；id 为帧 ID；extended 为扩展帧标志；dlc 为字节数；
 *       data 为 8 元素接收数组；now_ms 为本帧接收时的毫秒数。
 * 返回：接受时返回 true；拒绝时返回 false，并累计解析错误而不刷新旧数据。
 * 调用方式：DCAN 收到数据帧后调用 demo_sensor_receive(&sensor, id, extended, dlc, data, now_ms)。
 */
bool demo_sensor_receive(demo_sensor_t *sensor, uint32_t id, bool extended,
                         uint16_t dlc, const uint16_t *data, uint32_t now_ms)
{
    uint16_t raw_temp, raw_humidity;
    int32_t signed_temp, centi_temp;
    if (id != SENSOR_CAN_ID || !extended || dlc != 8U ||
        (data[0] & 0xFFU) != SENSOR_ADDRESS) {
        ++sensor->last.can_errors;
        return false;
    }
    raw_temp = (uint16_t)((data[1] & 0xFFU) | ((data[2] & 0xFFU) << 8U));
    raw_humidity = (uint16_t)((data[3] & 0xFFU) | ((data[4] & 0xFFU) << 8U));
    signed_temp = (raw_temp & 0x8000U) ? (int32_t)raw_temp - 65536L : (int32_t)raw_temp;
    centi_temp = signed_temp * 10L;
    /* 先在 32 位中换算并验证，避免负温度或倍率转换发生 16 位溢出。 */
    if (centi_temp < -32768L || centi_temp > 32767L || raw_humidity > 1000U) {
        ++sensor->last.can_errors;
        return false;
    }
    sensor->last.temperature_centi = (int16_t)centi_temp;
    sensor->last.humidity_centi = raw_humidity * 10U;
    ++sensor->last.can_rx;
    sensor->last_rx_ms = now_ms;
    sensor->have_sample = true;
    return true;
}

/*
 * 作用：依据最后有效 CAN 帧生成快照，传感器超时不能被持续发送的 UART 状态帧掩盖。
 * 参数：sensor 为缓存；now_ms 为当前毫秒数；bus_off 为 DCAN 离线状态；sample 为输出。
 * 返回：无；时间差使用无符号减法，支持毫秒计数器正常回绕。
 * 调用方式：周期上报前调用 demo_sensor_snapshot(&sensor, now_ms, bus_off, &sample)。
 */
void demo_sensor_snapshot(const demo_sensor_t *sensor, uint32_t now_ms,
                          bool bus_off, demo_sample_t *sample)
{
    *sample = sensor->last;
    sample->uptime_ms = now_ms;
    sample->flags = DEMO_FLAG_SENSOR;
    if (bus_off) sample->flags |= DEMO_FLAG_BUS_OFF;
    if (sensor->have_sample && !bus_off &&
        (uint32_t)(now_ms - sensor->last_rx_ms) < SENSOR_TIMEOUT_MS) {
        sample->flags |= DEMO_FLAG_VALID;
    }
}
