# 文件作用：注册 ESP32-S2 主组件、公共协议和网页资源。
# 包含模块：源文件列表、公共头文件目录、ESP-IDF 组件依赖、内嵌 HTML 页面。
# 调用方式：由 ESP-IDF 组件构建流程自动加载，不单独运行。
idf_component_register(
    SRCS "app_main.c" "../../../shared/demo_protocol.c"
    INCLUDE_DIRS "../../../shared"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_driver_uart esp_driver_gpio esp_timer
    EMBED_FILES "web/index.html")
