//
// modbus_tcp_client - Modbus TCP client (WiFi station) for the USA2 board.
//
// A self-contained on-board client for wifi_modbus_gateway: it joins the gateway's
// soft-AP, opens a Modbus TCP connection to 192.168.4.1:502, and once per second
// reads holding registers 0..3 from unit 1 (function 0x03), logging the values.
// Every few seconds it also writes a register (function 0x06) so the write path is
// exercised too - the written value then shows up in the next read.
//
// This lets the whole gateway<->RTU-slave demo run on three boards with no PC:
//     [modbus_tcp_client] --WiFi/TCP:502--> [wifi_modbus_gateway] --RS485--> [modbus_rtu_slave]
//
// (Any standard Modbus TCP client - pymodbus, mbpoll, a SCADA/HMI - works just as
// well against the gateway; this example is the ESP32-only equivalent.)
//
// Modbus TCP frame = MBAP header (7) + PDU:
//     [0..1] transaction id  [2..3] protocol id (0)  [4..5] length (unit+PDU)
//     [6] unit id            [7..] PDU (function + data)
//

#include <cstring>
#include <cstdio>
#include <cerrno>

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

constexpr char TAG[] = "mb_client";

// Must match wifi_modbus_gateway's soft-AP and Modbus TCP server.
constexpr char     AP_SSID[] = "USA2-MODBUS";
constexpr char     AP_PASS[] = "usa2mbus1";
constexpr char     SRV_IP[]  = "192.168.4.1";
constexpr uint16_t SRV_PORT  = 502;
constexpr uint8_t  UNIT_ID   = 1;         // RTU slave address behind the gateway

constexpr EventBits_t GOT_IP_BIT = BIT0;
EventGroupHandle_t s_wifi_events = nullptr;
uint16_t           s_txid       = 0;

// --- WiFi station ----------------------------------------------------------
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
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));   // ~8.5 dBm; avoids brownout on USB

    ESP_LOGI(TAG, "joining AP '%s' ...", AP_SSID);
}

// --- Modbus TCP helpers ----------------------------------------------------
bool recv_exact(int sock, uint8_t* buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int r = recv(sock, buf + got, n - got, 0);
        if (r > 0)                        { got += static_cast<size_t>(r); }
        else if (r < 0 && errno == EINTR) { continue; }
        else                              { return false; }
    }
    return true;
}

// One Modbus TCP transaction: wrap the request PDU in an MBAP, read the response
// PDU. Returns false on socket error. A Modbus exception is a normal response here
// (caller inspects respPdu[0] for the 0x80 bit).
bool transact(int sock, uint8_t unit, const uint8_t* reqPdu, size_t reqLen,
              uint8_t* respPdu, size_t* respLen)
{
    uint8_t frame[7 + 253];
    const uint16_t txid = s_txid++;
    frame[0] = static_cast<uint8_t>(txid >> 8);
    frame[1] = static_cast<uint8_t>(txid & 0xFF);
    frame[2] = 0; frame[3] = 0;                          // protocol id
    const uint16_t len = static_cast<uint16_t>(1 + reqLen);
    frame[4] = static_cast<uint8_t>(len >> 8);
    frame[5] = static_cast<uint8_t>(len & 0xFF);
    frame[6] = unit;
    std::memcpy(frame + 7, reqPdu, reqLen);
    if (send(sock, frame, 7 + reqLen, 0) < 0) return false;

    uint8_t mbap[7];
    if (!recv_exact(sock, mbap, 7)) return false;
    const uint16_t rlen = static_cast<uint16_t>((mbap[4] << 8) | mbap[5]);
    if (rlen < 1) return false;
    const size_t plen = rlen - 1;
    if (plen > *respLen) return false;
    if (!recv_exact(sock, respPdu, plen)) return false;
    *respLen = plen;
    return true;
}

// FC 0x03: read `count` holding registers from `start` into regs[]. Returns count read, -1 on error.
int read_holding(int sock, uint8_t unit, uint16_t start, uint16_t count, uint16_t* regs)
{
    const uint8_t pdu[5] = { 0x03, static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start & 0xFF),
                             static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count & 0xFF) };
    uint8_t resp[260];
    size_t  rl = sizeof(resp);
    if (!transact(sock, unit, pdu, sizeof(pdu), resp, &rl) || rl < 2) return -1;
    if (resp[0] != 0x03) { ESP_LOGW(TAG, "read exception code=0x%02X", resp[1]); return -1; }
    const int n = resp[1] / 2;
    for (int i = 0; i < n && i < count; ++i)
        regs[i] = static_cast<uint16_t>((resp[2 + 2 * i] << 8) | resp[3 + 2 * i]);
    return n;
}

// FC 0x06: write one holding register. Returns true on the normal echo response.
bool write_single(int sock, uint8_t unit, uint16_t addr, uint16_t val)
{
    const uint8_t pdu[5] = { 0x06, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
                             static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val & 0xFF) };
    uint8_t resp[16];
    size_t  rl = sizeof(resp);
    if (!transact(sock, unit, pdu, sizeof(pdu), resp, &rl) || rl < 1) return false;
    if (resp[0] != 0x06) { ESP_LOGW(TAG, "write exception code=0x%02X", rl >= 2 ? resp[1] : 0); return false; }
    return true;
}

void client_task(void*)
{
    xEventGroupWaitBits(s_wifi_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    for (;;) {  // (re)connect loop
        const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(SRV_PORT);
        inet_pton(AF_INET, SRV_IP, &dst.sin_addr);

        ESP_LOGI(TAG, "connecting to %s:%u ...", SRV_IP, SRV_PORT);
        if (connect(sock, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
            ESP_LOGW(TAG, "connect failed: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "connected - polling unit %u", UNIT_ID);
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        uint16_t writeVal = 0;
        for (int iter = 0;; ++iter) {
            // Read holding registers 0..3.
            uint16_t regs[4] = {0};
            if (read_holding(sock, UNIT_ID, 0, 4, regs) < 0) break;
            ESP_LOGI(TAG, "read holding: [0]=%u [1]=%u [2]=%u [3]=%u",
                     regs[0], regs[1], regs[2], regs[3]);

            // Every 5th poll, write reg 2 - it should appear in the next read.
            if (iter % 5 == 4) {
                if (!write_single(sock, UNIT_ID, 2, writeVal)) break;
                ESP_LOGI(TAG, "wrote reg[2] = %u", writeVal);
                ++writeVal;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGW(TAG, "connection lost, reconnecting");
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
    xTaskCreate(client_task, "mb_client", 4096, nullptr, 5, nullptr);
}
