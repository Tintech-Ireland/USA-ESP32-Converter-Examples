//
// wifi_to_rs232 - transparent WiFi (TCP) <-> RS232 bridge for the USA2 board.
//
// The board comes up as a WiFi soft-AP and listens on a TCP port. Bytes received
// on the TCP connection are written verbatim to the RS232 UART, and bytes read
// from the UART are sent verbatim back over TCP. No framing/protocol is imposed
// (raw <-> raw), so any plain TCP client works:
//
//     1. Join WiFi SSID "USA2-RS232" (password below).
//     2. Connect a TCP client to 192.168.4.1:3333, e.g.  nc 192.168.4.1 3333
//     3. Whatever you type goes out RS232 TX; whatever arrives on RS232 RX
//        comes back to your TCP client.
//
// RS232 link: UART1 via the on-board MAX3232 (TX=GPIO5, RX=GPIO4), 115200 8N1.
//
// To make this a WiFi *station* (join an existing network) instead of an AP,
// swap wifi_init_softap() for a STA init and use the DHCP-assigned IP; the TCP
// server and bridge code are unchanged.
//

#include <cstring>
#include <cerrno>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"     // MACSTR / MAC2STR
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "wifi_rs232";

// --- WiFi soft-AP settings ---
constexpr char     AP_SSID[]    = "USA2-RS232";
constexpr char     AP_PASS[]    = "usa2rs232";   // >= 8 chars for WPA2; "" = open
constexpr uint8_t  AP_CHANNEL   = 1;
constexpr uint8_t  AP_MAX_CONN  = 4;

// --- TCP server ---
constexpr uint16_t TCP_PORT     = 3333;

// --- RS232 UART ---
constexpr uart_port_t kPort     = UART_NUM_1;
constexpr int         kBaud     = 115200;
constexpr int         kUartBuf  = 2048;
constexpr int         kIoBuf    = 1024;

void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
        ESP_LOGI(TAG, "station " MACSTR " joined (aid=%d)", MAC2STR(e->mac), e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto* e = static_cast<wifi_event_ap_stadisconnected_t*>(data);
        ESP_LOGI(TAG, "station " MACSTR " left (aid=%d)", MAC2STR(e->mac), e->aid);
    }
}

void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.ap.ssid), AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = std::strlen(AP_SSID);
    wc.ap.channel        = AP_CHANNEL;
    std::strncpy(reinterpret_cast<char*>(wc.ap.password), AP_PASS, sizeof(wc.ap.password));
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode       = (std::strlen(AP_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Cap TX power to ~8.5 dBm (units of 0.25 dBm). The full 20 dBm current spike
    // browns out a USB-powered board; this is ample for bench-range links and
    // keeps the supply out of brownout.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));

    ESP_LOGI(TAG, "soft-AP up: SSID='%s' pass='%s' -> connect TCP to 192.168.4.1:%u",
             AP_SSID, AP_PASS, TCP_PORT);
}

void uart_init()
{
    const uart_config_t cfg = {
        .baud_rate  = kBaud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(kPort, kUartBuf, kUartBuf, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, Board::RS232_TX, Board::RS232_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// Pump one connected client until it disconnects: socket<->UART, both ways.
void bridge_client(int sock)
{
    // Non-blocking socket so we can interleave with UART polling.
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    uint8_t buf[kIoBuf];
    for (;;) {
        // TCP -> UART
        const int r = recv(sock, buf, sizeof(buf), 0);
        if (r > 0) {
            uart_write_bytes(kPort, reinterpret_cast<const char*>(buf), r);
        } else if (r == 0) {
            break;                                   // client closed
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            break;                                   // socket error
        }

        // UART -> TCP. The 10 ms read timeout also paces this loop.
        const int n = uart_read_bytes(kPort, buf, sizeof(buf), pdMS_TO_TICKS(10));
        for (int sent = 0; sent < n; ) {
            const int s = send(sock, buf + sent, n - sent, 0);
            if (s > 0) {
                sent += s;
            } else if (s < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                vTaskDelay(pdMS_TO_TICKS(1));         // TX buffer full, retry
            } else {
                return;                               // socket error -> drop client
            }
        }
    }
}

void tcp_server_task(void*)
{
    const int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(TCP_PORT);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "TCP server listening on port %u", TCP_PORT);

    for (;;) {
        sockaddr_in src = {};
        socklen_t   slen = sizeof(src);
        const int   sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&src), &slen);
        if (sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno %d", errno);
            continue;
        }

        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "client %s connected - bridging to RS232", ip);

        // Disable Nagle so single keystrokes go out promptly.
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        bridge_client(sock);

        close(sock);
        uart_flush_input(kPort);
        ESP_LOGI(TAG, "client %s disconnected", ip);
    }
}

} // namespace

extern "C" void app_main(void)
{
    // NVS is required by the WiFi driver.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    uart_init();
    wifi_init_softap();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, nullptr, 5, nullptr);
}
