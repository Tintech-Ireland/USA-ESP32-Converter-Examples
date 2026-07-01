//
// wifi_tcp_tester - TEST HARNESS (not a shipped converter).
//
// Exercises the WiFi converters from a third ESP32-C3. It is a WiFi *station*
// that joins the converter's soft-AP, opens a TCP connection to the converter's
// server, periodically sends "hello #N", and logs everything it receives back.
//
// Used with wifi_to_rs232 + an RS232 loopback jumper (TX_RS232->RX_RS232) on the
// bridge board: each "hello #N" travels WiFi -> TCP -> UART TX -> jumper ->
// UART RX -> TCP -> WiFi and should come straight back here, proving the whole
// path end to end.
//

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

namespace {

constexpr char TAG[] = "wifi_tester";

// Must match the converter's soft-AP and TCP server.
constexpr char     AP_SSID[]    = "USA2-RS232";
constexpr char     AP_PASS[]    = "usa2rs232";
constexpr char     SRV_IP[]     = "192.168.4.1";
constexpr uint16_t SRV_PORT     = 3333;

constexpr EventBits_t GOT_IP_BIT = BIT0;
EventGroupHandle_t s_wifi_events = nullptr;

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void*)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected from AP, retrying");
        xEventGroupClearBits(s_wifi_events, GOT_IP_BIT);
        esp_wifi_connect();
    }
}

void on_ip_event(void*, esp_event_base_t, int32_t, void* data)
{
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_wifi_events, GOT_IP_BIT);
}

void wifi_init_sta()
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, nullptr, nullptr));

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), AP_SSID, sizeof(wc.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), AP_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (std::strlen(AP_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Cap TX power to ~8.5 dBm to avoid the WiFi current spike browning out a
    // USB-powered board (boards are bench-range, so this is plenty).
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));

    ESP_LOGI(TAG, "joining AP '%s' ...", AP_SSID);
}

void client_task(void*)
{
    xEventGroupWaitBits(s_wifi_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    for (;;) {  // (re)connect loop
        const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) { ESP_LOGE(TAG, "socket: errno %d", errno); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        sockaddr_in dst = {};
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(SRV_PORT);
        inet_pton(AF_INET, SRV_IP, &dst.sin_addr);

        ESP_LOGI(TAG, "connecting to %s:%u ...", SRV_IP, SRV_PORT);
        if (connect(sock, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
            ESP_LOGW(TAG, "connect failed: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "connected - sending counters, echoing replies");

        const int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        uint32_t counter = 0;
        TickType_t lastTx = 0;
        bool alive = true;
        while (alive) {
            // Send "hello #N" once per second.
            const TickType_t now = xTaskGetTickCount();
            if (now - lastTx >= pdMS_TO_TICKS(1000)) {
                lastTx = now;
                char msg[32];
                const int n = std::snprintf(msg, sizeof(msg), "hello #%u\n", static_cast<unsigned>(counter));
                if (send(sock, msg, n, 0) < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                    ESP_LOGW(TAG, "send failed: errno %d", errno);
                    alive = false;
                    break;
                }
                ESP_LOGI(TAG, "TX 'hello #%u'", static_cast<unsigned>(counter));
                ++counter;
            }

            // Drain any echoed bytes.
            uint8_t rx[128];
            const int r = recv(sock, rx, sizeof(rx) - 1, 0);
            if (r > 0) {
                rx[r] = '\0';
                // Trim trailing newline for a tidy log line.
                if (rx[r - 1] == '\n') rx[r - 1] = '\0';
                ESP_LOGI(TAG, "RX (echo): '%s'", reinterpret_cast<char*>(rx));
            } else if (r == 0) {
                ESP_LOGW(TAG, "server closed connection");
                alive = false;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                ESP_LOGW(TAG, "recv error: errno %d", errno);
                alive = false;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();
    xTaskCreate(client_task, "tcp_client", 4096, nullptr, 5, nullptr);
}
