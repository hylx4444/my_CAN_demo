/*
 * 文件作用：在 WEMOS S2 Mini 上接收 157 遥测，并提供手机可访问的热点网页。
 * 包含模块：采样缓存、只接收 UART、硬件错误与启动诊断、本地模拟、热点与 DHCP、HTTP 接口。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "demo_protocol.h"

#define LINK_TIMEOUT_MS 3500U
#define UART_PARTIAL_TIMEOUT_MS 200
#define LINK_UART UART_NUM_1
#define FIRMWARE_ID "s2-diag-1"
static const char *TAG = "can_demo";
static SemaphoreHandle_t lock;
static demo_sample_t latest;
static bool have_sample;
static uint16_t last_sequence;
static int64_t last_sample_us;
static uint32_t received_frames, crc_errors, format_errors;
/* 阶段：1=入口，2=NVS，3=网络初始化，4=热点启动，5=数据源就绪，6=HTTP 就绪。 */
static volatile uint32_t boot_phase;
static volatile bool ap_running;
typedef struct {
    uint32_t bytes, frame_errors, parity_errors, overflows, breaks, read_errors;
    int64_t last_rx_us;
} uart_diagnostics_t;
static uart_diagnostics_t uart_diagnostics;
#ifndef CONFIG_DEMO_LOCAL_SIMULATION
static QueueHandle_t uart_events;
#endif
/* ESP-IDF 6 按嵌入文件名生成符号，web/index.html 对应 index_html。 */
extern const unsigned char index_start[] asm("_binary_index_html_start");
extern const unsigned char index_end[] asm("_binary_index_html_end");

/*
 * 作用：通过互斥锁更新最新采样、帧序号、接收时间和接收计数。
 * 参数：sample 为待发布的采样快照；sequence 为收到的帧序号。
 * 返回：无。
 * 调用方式：接收任务成功解码后调用 publish(&sample, frame.sequence)；需先创建 lock。
 */
static void publish(const demo_sample_t *sample, uint16_t sequence)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    latest = *sample; last_sequence = sequence;
    have_sample = true; last_sample_us = esp_timer_get_time(); ++received_frames;
    xSemaphoreGive(lock);
}

/*
 * 作用：记录热点真正启动和停止的事件，便于区分 Wi-Fi 状态与手机显示。
 * 参数：arg 为未使用的上下文；base 为事件域；id 为事件编号；data 为未使用的事件数据。
 * 返回：无。
 * 调用方式：由 app_main() 注册到默认事件循环，Wi-Fi 驱动异步调用。
 */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_START) { ap_running = true; ESP_LOGI(TAG, "AP started"); }
    else if (id == WIFI_EVENT_AP_STOP) { ap_running = false; ESP_LOGW(TAG, "AP stopped"); }
}

/*
 * 作用：每两秒输出启动阶段、热点状态和串口计数，收到完整帧前也能定位问题。
 * 参数：arg 为未使用的任务参数，创建时传 NULL。
 * 返回：无，任务常驻；不控制 Wi-Fi、不修改 UART、不生成测量数据。
 * 调用方式：由 app_main() 在互斥锁创建后启动；通过原生 USB 控制台查看日志。
 */
static void diagnostics_task(void *arg)
{
    (void)arg;
    for (;;) {
        uart_diagnostics_t diag;
        uint32_t frames, crc;
        xSemaphoreTake(lock, portMAX_DELAY);
        diag = uart_diagnostics; frames = received_frames; crc = crc_errors;
        xSemaphoreGive(lock);
        ESP_LOGI(TAG, "%s phase=%" PRIu32 " ap=%u esp_ms=%" PRIu32
                 " bytes=%" PRIu32 " frames=%" PRIu32 " crc=%" PRIu32
                 " frame_err=%" PRIu32 " overflows=%" PRIu32,
                 FIRMWARE_ID, boot_phase, (unsigned)ap_running,
                 (uint32_t)(esp_timer_get_time() / 1000), diag.bytes, frames, crc,
                 diag.frame_errors, diag.overflows);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/*
 * 作用：返回固件内嵌的网页，并关闭页面缓存。
 * 参数：req 为 HTTP 服务器传入的请求上下文。
 * 返回：返回 HTTP 响应发送结果，成功为 ESP_OK。
 * 调用方式：注册为 GET / 的处理函数，由 HTTP 服务器收到首页请求后自动调用。
 */
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_start, index_end - index_start);
}

/*
 * 作用：读取采样快照，分别报告 UART 链路、传感器有效状态和 CAN 离线状态。
 * 参数：req 为 HTTP 服务器传入的请求上下文。
 * 返回：返回 HTTP 响应发送结果；无效或过期温湿度以 JSON null 表示。
 * 调用方式：注册为 GET /api/status 的处理函数，网页每秒请求时由服务器自动调用。
 */
static esp_err_t status_get(httpd_req_t *req)
{
    char json[1280];
    demo_sample_t sample;
    bool present, online, valid;
    uint16_t sequence;
    uint32_t age, frames, crc, format;
    uart_diagnostics_t diag;
    xSemaphoreTake(lock, portMAX_DELAY);
    sample = latest; present = have_sample; sequence = last_sequence;
    age = present ? (uint32_t)((esp_timer_get_time() - last_sample_us) / 1000) : 0U;
    frames = received_frames; crc = crc_errors; format = format_errors;
    diag = uart_diagnostics;
    xSemaphoreGive(lock);
    online = present && age < LINK_TIMEOUT_MS;
    valid = online && (sample.flags & DEMO_FLAG_VALID);
    bool bus_off = online && (sample.flags & DEMO_FLAG_BUS_OFF);
    bool sensor_online = valid && (sample.flags & DEMO_FLAG_SENSOR);
    const char *source = (sample.flags & DEMO_FLAG_SENSOR) ? "c2000-sensor" :
                         (sample.flags & DEMO_FLAG_ESP_LOCAL) ? "esp-local-simulation" :
                         (sample.flags & DEMO_FLAG_HOST) ? "host-simulation" :
                         (sample.flags & DEMO_FLAG_HW_LOOPBACK) ? "c2000-loopback" : "unknown";
    char temperature[24], humidity[24];
    if (valid) {
        snprintf(temperature, sizeof(temperature), "%.2f", sample.temperature_centi / 100.0);
        snprintf(humidity, sizeof(humidity), "%.2f", sample.humidity_centi / 100.0);
    } else { strcpy(temperature, "null"); strcpy(humidity, "null"); }
    int n = snprintf(json, sizeof(json),
        "{\"source\":\"%s\",\"simulated\":%s,\"online\":%s,\"valid\":%s,"
        "\"sensor_online\":%s,\"bus_off\":%s,"
        "\"temperature\":%s,\"humidity\":%s,\"age_ms\":%" PRIu32 ",\"sequence\":%u,"
        "\"uptime_ms\":%" PRIu32 ",\"can_tx\":%" PRIu32 ",\"can_rx\":%" PRIu32 ","
        "\"can_errors\":%" PRIu32 ",\"received_frames\":%" PRIu32 ","
        "\"crc_errors\":%" PRIu32 ",\"format_errors\":%" PRIu32 ","
        "\"firmware\":\"%s\",\"esp_uptime_ms\":%" PRIu32 ",\"boot_phase\":%" PRIu32 ","
        "\"ap_running\":%s,\"reset_reason\":%u,\"uart_rx_gpio\":%u,\"uart_tx_enabled\":false,"
        "\"uart_rx_level\":%d,\"uart_rx_bytes\":%" PRIu32 ",\"uart_rx_age_ms\":%" PRIu32 ","
        "\"uart_frame_errors\":%" PRIu32 ",\"uart_parity_errors\":%" PRIu32 ","
        "\"uart_overflows\":%" PRIu32 ",\"uart_breaks\":%" PRIu32 ",\"uart_read_errors\":%" PRIu32 "}",
        source, (sample.flags & DEMO_FLAG_SIMULATED) ? "true" : "false",
        online ? "true" : "false", valid ? "true" : "false",
        sensor_online ? "true" : "false", bus_off ? "true" : "false", temperature, humidity,
        age, sequence, sample.uptime_ms, sample.can_tx, sample.can_rx, sample.can_errors,
        frames, crc, format, FIRMWARE_ID, (uint32_t)(esp_timer_get_time() / 1000), boot_phase,
        ap_running ? "true" : "false", (unsigned)esp_reset_reason(), (unsigned)CONFIG_DEMO_UART_RX_GPIO,
        gpio_get_level(CONFIG_DEMO_UART_RX_GPIO), diag.bytes,
        diag.bytes ? (uint32_t)((esp_timer_get_time() - diag.last_rx_us) / 1000) : UINT32_MAX,
        diag.frame_errors, diag.parity_errors, diag.overflows, diag.breaks, diag.read_errors);
    if (n < 0 || n >= (int)sizeof(json)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

#ifndef CONFIG_DEMO_LOCAL_SIMULATION
/*
 * 作用：读取 UART1，统计字节与硬件错误，解析遥测并发布采样，每轮让出 CPU。
 * 参数：arg 为 FreeRTOS 任务参数，当前未使用，创建时传 NULL。
 * 返回：无。
 * 调用方式：普通串口模式下由 app_main() 使用 xTaskCreate() 创建，任务启动后持续运行。
 */
static void uart_task(void *arg)
{
    (void)arg;
    demo_parser_t parser;
    demo_frame_t frame;
    demo_sample_t sample;
    uint8_t bytes[128];
    int64_t last_byte = 0;
    uart_diagnostics_t diag = {0};
    demo_parser_init(&parser);
    for (;;) {
        int n = uart_read_bytes(LINK_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        if (last_byte && now - last_byte > UART_PARTIAL_TIMEOUT_MS * 1000LL) demo_parser_reset(&parser);
        if (n > 0) {
            last_byte = now;
            diag.bytes += (uint32_t)n; diag.last_rx_us = now;
            for (int i = 0; i < n; ++i) {
                if (demo_parser_feed(&parser, bytes[i], &frame)) {
                    if (demo_decode(&frame, &sample)) publish(&sample, frame.sequence);
                    else ++parser.format_errors;
                }
            }
        } else if (n < 0) ++diag.read_errors;
        /* 事件队列只用于诊断；每轮处理有界数量，避免噪声输入使任务长期占用 CPU。 */
        uart_event_t event;
        for (unsigned i = 0; i < 20 && xQueueReceive(uart_events, &event, 0) == pdTRUE; ++i) {
            if (event.type == UART_FRAME_ERR) ++diag.frame_errors;
            else if (event.type == UART_PARITY_ERR) ++diag.parity_errors;
            else if (event.type == UART_BREAK) ++diag.breaks;
            else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                ++diag.overflows;
                /* 覆盖后丢弃不完整线路帧，恢复接收；不会把残帧当作有效采样。 */
                uart_flush_input(LINK_UART); demo_parser_reset(&parser); last_byte = 0;
            }
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        crc_errors = parser.crc_errors; format_errors = parser.format_errors;
        uart_diagnostics = diag;
        xSemaphoreGive(lock);
        vTaskDelay(1);
    }
}
#else
/*
 * 作用：在 ESP 本地生成模拟数据，并经公共编解码模块后发布到网页缓存。
 * 参数：arg 为 FreeRTOS 任务参数，当前未使用，创建时传 NULL。
 * 返回：无。
 * 调用方式：启用 CONFIG_DEMO_LOCAL_SIMULATION 后由 app_main() 创建，不访问 F2800157 或 UART。
 */
static void local_sim_task(void *arg)
{
    (void)arg;
    demo_sample_t sample = {0};
    demo_parser_t parser;
    demo_frame_t frame;
    uint16_t data[8], wire[DEMO_FRAME_MAX], sequence = 0;
    uint32_t step = 0;
    demo_parser_init(&parser);
    for (;;) {
        demo_model_payload(step++, data); demo_sample_from_can(data, &sample);
        sample.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        sample.flags = DEMO_FLAG_VALID | DEMO_FLAG_SIMULATED | DEMO_FLAG_ESP_LOCAL;
        uint16_t n = demo_encode(&sample, sequence++, wire);
        for (uint16_t i = 0; i < n; ++i) {
            if (demo_parser_feed(&parser, wire[i], &frame) && demo_decode(&frame, &sample)) publish(&sample, frame.sequence);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/*
 * 作用：初始化存储、热点网络、采样任务和 HTTP 路由。
 * 参数：无；热点名称、密码和 GPIO 从工程配置读取。
 * 返回：无。
 * 调用方式：由 ESP-IDF 启动任务自动调用一次 app_main()，其创建的后台任务继续运行。
 */
void app_main(void)
{
    boot_phase = 1;
    ESP_LOGI(TAG, "%s reset_reason=%u", FIRMWARE_ID, (unsigned)esp_reset_reason());
    /* GPIO5 接收遥测，GPIO4 作为预留引脚保持输入；GPIO19/20 留给 USB。 */
    if (CONFIG_DEMO_UART_TX_GPIO == CONFIG_DEMO_UART_RX_GPIO) {
        ESP_LOGE(TAG, "UART TX and RX must use different GPIOs");
        abort();
    }
    size_t password_len = strlen(CONFIG_DEMO_AP_PASSWORD);
    size_t ssid_len = strlen(CONFIG_DEMO_AP_SSID);
    if (password_len < 8 || password_len > 63 || ssid_len == 0 || ssid_len > 32) abort();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    boot_phase = 2;
    lock = xSemaphoreCreateMutex(); configASSERT(lock);
    configASSERT(xTaskCreate(diagnostics_task, "diagnostics", 3072, NULL, 3, NULL) == pdPASS);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    boot_phase = 3;
    esp_netif_t *ap = esp_netif_create_default_wifi_ap(); configASSERT(ap);
    /* 固定热点地址，使手机访问地址与运行日志、工程说明保持一致。 */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi = {0};
    memcpy(wifi.ap.ssid, CONFIG_DEMO_AP_SSID, ssid_len);
    memcpy(wifi.ap.password, CONFIG_DEMO_AP_PASSWORD, password_len);
    wifi.ap.ssid_len = ssid_len; wifi.ap.channel = 1; wifi.ap.max_connection = 4;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    boot_phase = 4;

#ifndef CONFIG_DEMO_LOCAL_SIMULATION
    uart_config_t uart = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT
    };
    ESP_LOGI(TAG, "UART init: RX=%u, TX disabled, 115200 8N1", (unsigned)CONFIG_DEMO_UART_RX_GPIO);
    ESP_ERROR_CHECK(gpio_reset_pin(CONFIG_DEMO_UART_TX_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(CONFIG_DEMO_UART_TX_GPIO, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(CONFIG_DEMO_UART_TX_GPIO, GPIO_FLOATING));
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 0, 20, &uart_events, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, UART_PIN_NO_CHANGE, CONFIG_DEMO_UART_RX_GPIO,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* 未接 157 时让 RX 保持 UART 空闲高电平，避免悬空输入持续接收噪声。 */
    ESP_ERROR_CHECK(gpio_pullup_en(CONFIG_DEMO_UART_RX_GPIO));
    configASSERT(xTaskCreate(uart_task, "uart_link", 4096, NULL, 5, NULL) == pdPASS);
#else
    configASSERT(xTaskCreate(local_sim_task, "local_sim", 4096, NULL, 5, NULL) == pdPASS);
#endif
    boot_phase = 5;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.lru_purge_enable = true; http.max_open_sockets = 5; http.stack_size = 6144;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    boot_phase = 6;
    ESP_LOGI(TAG, "Hotspot %s ready; open http://192.168.4.1", CONFIG_DEMO_AP_SSID);
}
