/*
 * 文件作用：在 WEMOS S2 Mini 上接收 157 遥测，并提供手机可访问的热点网页。
 * 包含模块：共享采样缓存、UART 接收任务、本地模拟任务、热点与 DHCP、HTTP 页面和状态接口。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "demo_protocol.h"

#define LINK_TIMEOUT_MS 3500U
#define UART_PARTIAL_TIMEOUT_MS 200
#define LINK_UART UART_NUM_1
static const char *TAG = "can_demo";
static SemaphoreHandle_t lock;
static demo_sample_t latest;
static bool have_sample;
static uint16_t last_sequence;
static int64_t last_sample_us;
static uint32_t received_frames, crc_errors, format_errors;
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
    char json[768];
    demo_sample_t sample;
    bool present, online, valid;
    uint16_t sequence;
    uint32_t age, frames, crc, format;
    xSemaphoreTake(lock, portMAX_DELAY);
    sample = latest; present = have_sample; sequence = last_sequence;
    age = present ? (uint32_t)((esp_timer_get_time() - last_sample_us) / 1000) : 0U;
    frames = received_frames; crc = crc_errors; format = format_errors;
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
        "\"crc_errors\":%" PRIu32 ",\"format_errors\":%" PRIu32 "}",
        source, (sample.flags & DEMO_FLAG_SIMULATED) ? "true" : "false",
        online ? "true" : "false", valid ? "true" : "false",
        sensor_online ? "true" : "false", bus_off ? "true" : "false", temperature, humidity,
        age, sequence, sample.uptime_ms, sample.can_tx, sample.can_rx, sample.can_errors,
        frames, crc, format);
    if (n < 0 || n >= (int)sizeof(json)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

#ifndef CONFIG_DEMO_LOCAL_SIMULATION
/*
 * 作用：循环读取 UART1，解析遥测帧，处理残帧超时并发布采样。
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
    demo_parser_init(&parser);
    for (;;) {
        int n = uart_read_bytes(LINK_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        if (last_byte && now - last_byte > UART_PARTIAL_TIMEOUT_MS * 1000LL) demo_parser_reset(&parser);
        if (n > 0) {
            last_byte = now;
            for (int i = 0; i < n; ++i) {
                if (demo_parser_feed(&parser, bytes[i], &frame)) {
                    if (demo_decode(&frame, &sample)) publish(&sample, frame.sequence);
                    else ++parser.format_errors;
                }
            }
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        crc_errors = parser.crc_errors; format_errors = parser.format_errors;
        xSemaphoreGive(lock);
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
    /* GPIO4/5 用作 UART1；配置范围限制在 1..18，保留原生 USB GPIO19/20。 */
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
    lock = xSemaphoreCreateMutex(); configASSERT(lock);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
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

#ifndef CONFIG_DEMO_LOCAL_SIMULATION
    uart_config_t uart = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_DEMO_UART_TX_GPIO, CONFIG_DEMO_UART_RX_GPIO,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* 未接 157 时让 RX 保持 UART 空闲高电平，避免悬空输入持续接收噪声。 */
    ESP_ERROR_CHECK(gpio_pullup_en(CONFIG_DEMO_UART_RX_GPIO));
    configASSERT(xTaskCreate(uart_task, "uart_link", 4096, NULL, 8, NULL) == pdPASS);
#else
    configASSERT(xTaskCreate(local_sim_task, "local_sim", 4096, NULL, 5, NULL) == pdPASS);
#endif
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.lru_purge_enable = true; http.max_open_sockets = 5;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_LOGI(TAG, "Hotspot %s ready; open http://192.168.4.1", CONFIG_DEMO_AP_SSID);
}
