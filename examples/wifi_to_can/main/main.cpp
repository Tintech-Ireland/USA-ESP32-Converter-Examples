//
// wifi_to_can - WiFi (TCP) <-> CAN (TWAI) bridge for the USA2 board.
//
// CAN is message-oriented, and TCP is a boundary-less byte stream, so frames are
// delimited on the TCP side with a 2-byte big-endian (network-order) length
// prefix. Every TCP transmission is  [uint16 len][payload]  and every received
// payload is reassembled the same way. The payload is one CAN frame:
//
//     length = 6 + dlc
//       [0..3] identifier  uint32 big-endian  (11-bit std or 29-bit extended)
//       [4]    flags        bit0 = extended id, bit1 = remote (RTR)
//       [5]    dlc          0..8
//       [6..]  data         dlc bytes
//
// Topology: the board is a WiFi soft-AP with a TCP server on port 3334. Frames
// arriving over TCP are transmitted on the CAN bus; frames received from the bus
// are sent to the TCP client. CAN: TJA1042 transceiver, TX=GPIO8, RX=GPIO6,
// 500 kbit/s. The bus still needs 120 ohm termination at each end.
//

#include <cstring>
#include <cerrno>
#include <cinttypes>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "wifi_can";

constexpr char     AP_SSID[]   = "USA2-CAN";
constexpr char     AP_PASS[]   = "usa2can1";   // >= 8 chars for WPA2
constexpr uint16_t TCP_PORT    = 3334;

constexpr int      kMaxPayload = 6 + 8;        // largest CAN frame payload
constexpr int      kAccCap     = 64;           // TCP receive accumulator

// ---- WiFi soft-AP (TX power capped to avoid USB brownout) ------------------
void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.ap.ssid), AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = std::strlen(AP_SSID);
    wc.ap.channel        = 1;
    std::strncpy(reinterpret_cast<char*>(wc.ap.password), AP_PASS, sizeof(wc.ap.password));
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));   // ~8.5 dBm; avoids brownout on USB

    ESP_LOGI(TAG, "soft-AP '%s' up -> TCP 192.168.4.1:%u", AP_SSID, TCP_PORT);
}

void twai_init()
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(Board::CAN_TX, Board::CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "TWAI up @500kbit/s (TX=GPIO%d RX=GPIO%d)", Board::CAN_TX, Board::CAN_RX);
}

// ---- length-prefixed TCP send ---------------------------------------------
bool send_all(int sock, const uint8_t* p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        const int s = send(sock, p + sent, n - sent, 0);
        if (s > 0)                                   { sent += static_cast<size_t>(s); }
        else if (s < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) { vTaskDelay(1); }
        else                                         { return false; }
    }
    return true;
}

bool send_frame(int sock, const uint8_t* payload, uint16_t len)
{
    const uint8_t hdr[2] = { static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF) };
    return send_all(sock, hdr, 2) && send_all(sock, payload, len);
}

// ---- CAN <-> payload codec -------------------------------------------------
uint16_t encode_can(const twai_message_t& m, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(m.identifier >> 24);
    out[1] = static_cast<uint8_t>(m.identifier >> 16);
    out[2] = static_cast<uint8_t>(m.identifier >>  8);
    out[3] = static_cast<uint8_t>(m.identifier      );
    out[4] = static_cast<uint8_t>((m.extd ? 0x01 : 0) | (m.rtr ? 0x02 : 0));
    out[5] = m.data_length_code;
    for (int i = 0; i < m.data_length_code; ++i) out[6 + i] = m.data[i];
    return static_cast<uint16_t>(6 + m.data_length_code);
}

// Returns true if the payload was a well-formed CAN frame.
bool decode_can(const uint8_t* p, int len, twai_message_t& m)
{
    if (len < 6) return false;
    const uint8_t dlc = p[5];
    if (dlc > 8 || len < 6 + dlc) return false;

    m = {};
    m.identifier       = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
                       | (static_cast<uint32_t>(p[2]) <<  8) |  static_cast<uint32_t>(p[3]);
    m.extd             = (p[4] & 0x01) ? 1 : 0;
    m.rtr              = (p[4] & 0x02) ? 1 : 0;
    m.data_length_code = dlc;
    for (int i = 0; i < dlc; ++i) m.data[i] = p[6 + i];
    return true;
}

// Pump one connected client until it disconnects.
void bridge_client(int sock)
{
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    uint8_t acc[kAccCap];
    size_t  accLen = 0;

    for (;;) {
        // ---- TCP -> CAN: accumulate bytes, emit complete length-prefixed frames
        if (accLen < sizeof(acc)) {
            const int r = recv(sock, acc + accLen, sizeof(acc) - accLen, 0);
            if (r > 0) {
                accLen += static_cast<size_t>(r);
            } else if (r == 0) {
                break;                                       // client closed
            } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                break;                                       // socket error
            }
        }

        while (accLen >= 2) {
            const uint16_t len = static_cast<uint16_t>((acc[0] << 8) | acc[1]);
            if (len > kMaxPayload) {
                ESP_LOGW(TAG, "framing error: payload len %u > %d, dropping client", len, kMaxPayload);
                return;
            }
            if (accLen < 2u + len) break;                    // wait for the rest

            twai_message_t m;
            if (decode_can(acc + 2, len, m)) {
                if (twai_transmit(&m, pdMS_TO_TICKS(100)) == ESP_OK) {
                    ESP_LOGI(TAG, "TCP->CAN id=0x%03" PRIx32 " dlc=%d", m.identifier, m.data_length_code);
                } else {
                    ESP_LOGW(TAG, "CAN TX failed (bus/termination?)");
                }
            } else {
                ESP_LOGW(TAG, "bad CAN payload (len=%u)", len);
            }

            const size_t consumed = 2u + len;
            memmove(acc, acc + consumed, accLen - consumed);
            accLen -= consumed;
        }

        // ---- CAN -> TCP: forward any received bus frame (10 ms paces the loop)
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) {
            uint8_t payload[kMaxPayload];
            const uint16_t len = encode_can(rx, payload);
            if (!send_frame(sock, payload, len)) break;
            ESP_LOGI(TAG, "CAN->TCP id=0x%03" PRIx32 " dlc=%d", rx.identifier, rx.data_length_code);
        }
    }
}

void tcp_server_task(void*)
{
    const int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
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
        if (sock < 0) { ESP_LOGW(TAG, "accept: errno %d", errno); continue; }

        char ip[16];
        inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        ESP_LOGI(TAG, "client %s connected", ip);

        bridge_client(sock);

        close(sock);
        ESP_LOGI(TAG, "client %s disconnected", ip);
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

    twai_init();
    wifi_init_softap();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, nullptr, 5, nullptr);
}
