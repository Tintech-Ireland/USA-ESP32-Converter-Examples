//
// wifi_modbus_gateway - Modbus TCP  <->  Modbus RTU (RS485) gateway for the USA2 board.
//
// The board comes up as a WiFi soft-AP with a Modbus TCP server on port 502. Each
// Modbus TCP request is translated to a Modbus RTU transaction on the RS485 bus (via
// the `modbus` component), and the RTU response is wrapped back into Modbus TCP and
// returned to the client. This is a standard TCP<->RTU gateway: any Modbus TCP client
// (pymodbus, a SCADA/HMI, mbpoll, ...) can reach the RTU slaves on the wired bus.
//
// Modbus TCP frame  = MBAP header (7) + PDU:
//     [0..1] transaction id   (echoed back unchanged)
//     [2..3] protocol id      (0 for Modbus)
//     [4..5] length           (bytes following = unit id + PDU)
//     [6]    unit id          (the RTU slave address)
//     [7..]  PDU              (function code + data)
//
// The gateway forwards the PDU to RTU unit `unit id`, then replies with the RTU
// response PDU under the same MBAP. If the slave does not answer (timeout / CRC),
// it returns a Modbus exception `function|0x80` with code 0x0B "gateway target device
// failed to respond", per the Modbus messaging spec.
//
// RS485: UART1 via the on-board SP3485EN (TX=GPIO9, RX=GPIO7, DE=GPIO10). Line format
// defaults to 9600 8N1 below - change kBaud / parity to match your bus (the Modbus
// spec default is 19200 8E1).
//

#include <cstring>
#include <cerrno>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "board_pins.h"
#include "modbus_rtu.h"

namespace {

constexpr char TAG[] = "wifi_modbus";

constexpr char     AP_SSID[]  = "USA2-MODBUS";
constexpr char     AP_PASS[]  = "usa2mbus1";     // >= 8 chars for WPA2
constexpr uint16_t TCP_PORT   = 502;             // standard Modbus TCP port

// --- RS485 / Modbus RTU line settings (adjust to your bus) ---
constexpr int      kBaud          = 9600;        // spec default is 19200; many devices use 9600
constexpr uart_parity_t kParity   = UART_PARITY_DISABLE;   // 8N1 here; spec default is 8E1
constexpr uint32_t kRtuTimeoutMs  = 1000;        // per-transaction response timeout

Modbus::RtuMaster g_mb;

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

    ESP_LOGI(TAG, "soft-AP '%s' up -> Modbus TCP 192.168.4.1:%u", AP_SSID, TCP_PORT);
}

// ---- blocking socket helpers ----------------------------------------------
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

bool send_all(int sock, const uint8_t* p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        const int s = send(sock, p + sent, n - sent, 0);
        if (s > 0)                        { sent += static_cast<size_t>(s); }
        else if (s < 0 && errno == EINTR) { continue; }
        else                              { return false; }
    }
    return true;
}

// Pump one Modbus TCP client: MBAP+PDU -> RTU transaction -> MBAP+response.
void gateway_client(int sock)
{
    for (;;) {
        uint8_t mbap[7];
        if (!recv_exact(sock, mbap, sizeof(mbap))) break;

        const uint16_t proto = static_cast<uint16_t>((mbap[2] << 8) | mbap[3]);
        const uint16_t len   = static_cast<uint16_t>((mbap[4] << 8) | mbap[5]);
        const uint8_t  unit  = mbap[6];

        // length counts unit id (1) + PDU (>=1); PDU capped at 253.
        if (proto != 0 || len < 2 || len > 1 + Modbus::RtuMaster::kMaxPdu) {
            ESP_LOGW(TAG, "bad MBAP (proto=%u len=%u), dropping client", proto, len);
            break;
        }
        const size_t pduLen = len - 1;
        uint8_t pdu[Modbus::RtuMaster::kMaxPdu];
        if (!recv_exact(sock, pdu, pduLen)) break;

        // Forward to the RTU bus.
        uint8_t rpdu[Modbus::RtuMaster::kMaxPdu];
        size_t  rlen = sizeof(rpdu);
        const Modbus::Status st = g_mb.transceive(unit, pdu, pduLen, rpdu, &rlen, kRtuTimeoutMs);

        uint8_t out[7 + Modbus::RtuMaster::kMaxPdu];
        out[0] = mbap[0];  out[1] = mbap[1];        // echo transaction id
        out[2] = 0;        out[3] = 0;              // protocol id
        out[6] = unit;

        size_t plen;
        if (st == Modbus::Status::Ok) {
            plen = rlen;
            std::memcpy(out + 7, rpdu, plen);
            ESP_LOGI(TAG, "unit=%u fc=0x%02X -> %u byte reply", unit, pdu[0], static_cast<unsigned>(plen));
        } else {
            // Modbus exception: function|0x80, code 0x0B (gateway target failed to respond).
            plen = 2;
            out[7] = static_cast<uint8_t>(pdu[0] | 0x80);
            out[8] = 0x0B;
            ESP_LOGW(TAG, "unit=%u fc=0x%02X RTU %s -> exception 0x0B",
                     unit, pdu[0], Modbus::to_string(st));
        }
        const uint16_t rlenField = static_cast<uint16_t>(1 + plen);   // unit + PDU
        out[4] = static_cast<uint8_t>(rlenField >> 8);
        out[5] = static_cast<uint8_t>(rlenField & 0xFF);

        if (!send_all(sock, out, 7 + plen)) break;
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
    ESP_LOGI(TAG, "Modbus TCP server listening on port %u", TCP_PORT);

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

        gateway_client(sock);

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

    ESP_ERROR_CHECK(g_mb.begin(UART_NUM_1, kBaud, Board::RS485_TX, Board::RS485_RX,
                               Board::RS485_DE, UART_DATA_8_BITS, kParity, UART_STOP_BITS_1));
    ESP_LOGI(TAG, "Modbus RTU master up on RS485 (TX=GPIO%d RX=GPIO%d DE=GPIO%d) @%d",
             Board::RS485_TX, Board::RS485_RX, Board::RS485_DE, kBaud);

    wifi_init_softap();
    xTaskCreate(tcp_server_task, "modbus_tcp", 4096, nullptr, 5, nullptr);
}
