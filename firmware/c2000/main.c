/*
 * 文件作用：接收自定义 CAN 温湿度传感器，并向 ESP32-S2 发送遥测。
 * 包含模块：板级初始化、毫秒中断、DCAN 接收恢复、传感器解析、串口上报、调试变量。
 */
#include "driverlib.h"
#include "device.h"
#include "demo_protocol.h"

#define CAN_RX_OBJECT 2U
#define UART_BAUD 115200UL
#define TELEMETRY_PERIOD_MS 200UL

/* 应用不发送 CAN 数据帧，硬件 ACK 不计入 g_can_tx；可在 CCS Watch 观察这些变量。 */
volatile uint32_t g_can_tx = 0, g_can_rx = 0, g_can_errors = 0;
volatile uint32_t g_can_frames = 0, g_can_rejected = 0, g_can_overruns = 0;
volatile uint32_t g_bus_off_count = 0, g_uptime_ms = 0, g_last_rx_ms = 0;
volatile uint32_t g_can_rx_error_count = 0, g_can_tx_error_count = 0;
volatile uint32_t g_last_can_id = 0, g_sensor_age_ms = 0;
volatile uint16_t g_last_can_status = 0, g_last_dlc = 0, g_sensor_online = 0;
volatile int16_t g_temperature_centi = 0;
volatile uint16_t g_humidity_centi = 0;
static demo_sensor_t sensor;
static bool bus_off = false;

/*
 * 作用：推进毫秒时间，并应答定时器中断。
 * 参数：无。
 * 返回：无。
 * 调用方式：由 board_init() 注册，每毫秒由硬件自动触发。
 */
__interrupt void tick_isr(void)
{
    ++g_uptime_ms;
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

/*
 * 作用：建立严格匹配传感器扩展数据帧的接收邮箱，并清除覆盖标志。
 * 参数：无；ID 使用 SENSOR_CAN_ID。
 * 返回：无。
 * 调用方式：初始化 CAN 后调用；收到覆盖帧并处理后再次调用以恢复邮箱。
 */
static void can_arm_receiver(void)
{
    CAN_setupMessageObject(CANA_BASE, CAN_RX_OBJECT, SENSOR_CAN_ID, CAN_MSG_FRAME_EXT,
                           CAN_MSG_OBJ_TYPE_RX, 0x1FFFFFFFUL,
                           CAN_MSG_OBJ_USE_ID_FILTER | CAN_MSG_OBJ_USE_EXT_FILTER |
                           CAN_MSG_OBJ_USE_DIR_FILTER, 8U);
}

/*
 * 作用：初始化真实 CAN、UART、LED 和定时中断，不启用回环或静默模式。
 * 参数：无。
 * 返回：无。
 * 调用方式：main() 启动时调用；板上 S2 设为 1/0，S4 接到 XCVR 一侧。
 */
static void board_init(void)
{
    Device_init(); Device_initGPIO();
    Interrupt_initModule(); Interrupt_initVectorTable();
    GPIO_setPinConfig(GPIO_29_SCIA_TX);
    GPIO_setPinConfig(GPIO_28_SCIA_RX);
    GPIO_setPadConfig(28U, GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(28U, GPIO_QUAL_ASYNC);
    SCI_performSoftwareReset(SCIA_BASE);
    SCI_setConfig(SCIA_BASE, DEVICE_LSPCLK_FREQ, UART_BAUD,
                  SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE);
    SCI_resetChannels(SCIA_BASE); SCI_resetTxFIFO(SCIA_BASE); SCI_resetRxFIFO(SCIA_BASE);
    SCI_enableFIFO(SCIA_BASE); SCI_enableModule(SCIA_BASE);
    SCI_performSoftwareReset(SCIA_BASE);
    GPIO_setPinConfig(DEVICE_GPIO_CFG_LED1);
    GPIO_setDirectionMode(DEVICE_GPIO_PIN_LED1, GPIO_DIR_MODE_OUT);
    GPIO_writePin(DEVICE_GPIO_PIN_LED1, 1U);
    /* GPIO4/5 接板载收发器；传感器 CANH/CANL 必须接 J14，而不是 GPIO。 */
    GPIO_setPinConfig(DEVICE_GPIO_CFG_CANTXA);
    GPIO_setPinConfig(DEVICE_GPIO_CFG_CANRXA);
    GPIO_setPadConfig(DEVICE_GPIO_PIN_CANRXA, GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(DEVICE_GPIO_PIN_CANRXA, GPIO_QUAL_ASYNC);
    CAN_initModule(CANA_BASE);
    CAN_setBitRate(CANA_BASE, DEVICE_SYSCLK_FREQ, SENSOR_CAN_BITRATE, 20U);
    CAN_disableTestMode(CANA_BASE);
    CAN_setAutoBusOnTime(CANA_BASE, DEVICE_SYSCLK_FREQ / 10U);
    CAN_enableAutoBusOn(CANA_BASE);
    can_arm_receiver();
    CAN_startModule(CANA_BASE);
    CPUTimer_stopTimer(CPUTIMER0_BASE);
    CPUTimer_setPreScaler(CPUTIMER0_BASE, 0U);
    CPUTimer_setPeriod(CPUTIMER0_BASE, DEVICE_SYSCLK_FREQ / 1000U - 1U);
    CPUTimer_reloadTimerCounter(CPUTIMER0_BASE);
    CPUTimer_setEmulationMode(CPUTIMER0_BASE, CPUTIMER_EMULATIONMODE_STOPAFTERNEXTDECREMENT);
    Interrupt_register(INT_TIMER0, &tick_isr);
    CPUTimer_enableInterrupt(CPUTIMER0_BASE); Interrupt_enable(INT_TIMER0);
    CPUTimer_startTimer(CPUTIMER0_BASE);
    EINT; ERTM;
}

/*
 * 作用：读取实际帧元数据及载荷，校验后更新真实温湿度和调试计数。
 * 参数：now_ms 为接收时的毫秒数。
 * 返回：无。
 * 调用方式：主循环持续调用 can_poll(now_ms)，不等待传感器，不发送读取命令。
 */
static void can_poll(uint32_t now_ms)
{
    uint16_t data[8] = {0}, control;
    uint32_t arbitration;
    bool extended, lost;
    if ((CAN_getNewDataFlags(CANA_BASE) & (1UL << (CAN_RX_OBJECT - 1U))) == 0U) return;
    /* 先读取 DLC，再固定读取 8 字节，避免非法长度造成接收数组越界。 */
    CAN_transferMessage(CANA_BASE, 2U, CAN_RX_OBJECT, false);
    control = HWREGH(CANA_BASE + CAN_O_IF2MCTL);
    arbitration = HWREG_BP(CANA_BASE + CAN_O_IF2ARB);
    g_last_dlc = control & CAN_IF2MCTL_DLC_M;
    extended = (arbitration & CAN_IF2ARB_XTD) != 0U;
    g_last_can_id = extended ? arbitration & CAN_IF2ARB_ID_M :
                    (arbitration & CAN_IF2ARB_STD_ID_M) >> CAN_IF2ARB_STD_ID_S;
    lost = (control & CAN_IF2MCTL_MSGLST) != 0U;
    ++g_can_frames;
    if (g_last_dlc == 8U) CAN_readDataReg(data, CANA_BASE + CAN_O_IF2DATA, 8U);
    if ((arbitration & CAN_IF2ARB_DIR) != 0U) {
        ++sensor.last.can_errors; ++g_can_rejected;
    } else if (demo_sensor_receive(&sensor, g_last_can_id, extended, g_last_dlc, data, now_ms)) {
        g_can_rx = sensor.last.can_rx;
        g_last_rx_ms = now_ms;
        g_temperature_centi = sensor.last.temperature_centi;
        g_humidity_centi = sensor.last.humidity_centi;
        GPIO_togglePin(DEVICE_GPIO_PIN_LED1);
    } else ++g_can_rejected;
    if (lost) { ++g_can_overruns; can_arm_receiver(); }
}

/*
 * 作用：记录总线离线事件和硬件错误计数，自动恢复交由 DCAN 的 Auto-Bus-On 执行。
 * 参数：无。
 * 返回：无。
 * 调用方式：主循环每 10 毫秒调用；同一次连续离线只累计一个事件。
 */
static void can_update_status(void)
{
    uint32_t rx_errors, tx_errors;
    bool next_bus_off;
    g_last_can_status = CAN_getStatus(CANA_BASE);
    next_bus_off = (g_last_can_status & CAN_STATUS_BUS_OFF) != 0U;
    if (next_bus_off && !bus_off) ++g_bus_off_count;
    bus_off = next_bus_off;
    (void)CAN_getErrorCount(CANA_BASE, &rx_errors, &tx_errors);
    g_can_rx_error_count = rx_errors; g_can_tx_error_count = tx_errors;
}

/*
 * 作用：处理真实传感器帧，每 200 毫秒发送状态，超时后继续发送无效状态帧。
 * 参数：无。
 * 返回：无，常驻运行。
 * 调用方式：由芯片启动代码进入；未接线时接收计数为 0、传感器离线是正常现象。
 */
void main(void)
{
    demo_sample_t sample;
    uint16_t wire[DEMO_FRAME_MAX], sequence = 0U, i, length;
    uint32_t now_ms, last_report = 0U, last_status = 0U;
    demo_sensor_init(&sensor);
    board_init();
    for (;;) {
        now_ms = g_uptime_ms;
        can_poll(now_ms);
        if ((uint32_t)(now_ms - last_status) >= 10U) {
            last_status = now_ms; can_update_status();
        }
        if ((uint32_t)(now_ms - last_report) < TELEMETRY_PERIOD_MS) continue;
        last_report = now_ms;
        demo_sensor_snapshot(&sensor, now_ms, bus_off, &sample);
        sample.can_errors += g_can_overruns + g_bus_off_count;
        g_can_errors = sample.can_errors;
        g_sensor_online = (sample.flags & DEMO_FLAG_VALID) != 0U;
        g_sensor_age_ms = sensor.have_sample ? (uint32_t)(now_ms - sensor.last_rx_ms) : 0xFFFFFFFFUL;
        length = demo_encode(&sample, sequence++, wire);
        for (i = 0U; i < length; ++i) SCI_writeCharBlockingFIFO(SCIA_BASE, wire[i]);
    }
}
