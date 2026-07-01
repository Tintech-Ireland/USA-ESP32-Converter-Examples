//
// wifi_to_i2c - WiFi (TCP) <-> I2C (master) bridge for the USA2 board.
//
// I2C is a master-driven transaction protocol, so - like wifi_to_spi - this bridge
// is request/response. Each length-prefixed TCP frame is one I2C transaction the
// board performs as bus master; the reply carries the status and any bytes read.
//
//   client -> [u16 len][ addr | wlen | rlen | write bytes(wlen) ]
//   bridge -> perform one of: write / read / write-then-read on the I2C bus
//   bridge -> [u16 len][ status | read bytes(rlen, only if status==0) ]
//
//     addr    7-bit device address
//     wlen    number of bytes to write (0..kMaxData)
//     rlen    number of bytes to read  (0..kMaxData)
//     status  0 = ESP_OK, non-zero = transfer failed (e.g. address NACKed)
//
// The three transaction shapes fall out of wlen/rlen:
//     wlen>0, rlen>0 -> write-then-read (repeated START), e.g. register read
//     wlen>0, rlen=0 -> write only
//     wlen=0, rlen>0 -> read only
//
// Topology: soft-AP with a TCP server on port 3337; I2C master on the spare
// GPIO0/GPIO1 header (SDA=GPIO0, SCL=GPIO1), 100 kHz, internal pull-ups. Uses the
// ESP-IDF v5.x bus-based master driver (driver/i2c_master.h). A device handle is
// created per address on demand and cached (re-created when the address changes).
//

#include <cstring>
#include <cerrno>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "wifi_i2c";

constexpr char     AP_SSID[]   = "USA2-I2C";
constexpr char     AP_PASS[]   = "usa2i2c1";   // >= 8 chars for WPA2
constexpr uint16_t TCP_PORT    = 3337;

constexpr uint32_t kSclHz      = 100000;       // 100 kHz standard mode
constexpr int      kMaxData    = 32;           // cap on wlen / rlen per transaction
constexpr int      kXferMs     = 1000;         // per-transaction timeout

i2c_master_bus_handle_t g_bus = nullptr;

// Cache one device handle; re-create it when the requested address changes. The
// common case is a single target, so this avoids add/remove churn per transaction.
i2c_master_dev_handle_t g_dev      = nullptr;
uint16_t                g_dev_addr = 0xFFFF;

i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    if (g_dev && g_dev_addr == addr) return g_dev;
    if (g_dev) { i2c_master_bus_rm_device(g_dev); g_dev = nullptr; g_dev_addr = 0xFFFF; }

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = addr;
    dc.scl_speed_hz    = kSclHz;
    if (i2c_master_bus_add_device(g_bus, &dc, &g_dev) != ESP_OK) {
        g_dev = nullptr;
        return nullptr;
    }
    g_dev_addr = addr;
    return g_dev;
}

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

void i2c_init()
{
    i2c_master_bus_config_t bc = {};
    bc.i2c_port                     = I2C_NUM_0;
    bc.sda_io_num                   = Board::I2C_SDA;
    bc.scl_io_num                   = Board::I2C_SCL;
    bc.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt            = 7;
    bc.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &g_bus));

    ESP_LOGI(TAG, "I2C master up: SDA=GPIO%d SCL=GPIO%d @%luHz",
             Board::I2C_SDA, Board::I2C_SCL, static_cast<unsigned long>(kSclHz));
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

bool send_reply(int sock, const uint8_t* payload, uint16_t len)
{
    const uint8_t hdr[2] = { static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF) };
    return send_all(sock, hdr, 2) && send_all(sock, payload, len);
}

// Pump one connected client: [len][addr|wlen|rlen|wbytes] -> I2C -> [len][status|rbytes]
void bridge_client(int sock)
{
    uint8_t req[3 + kMaxData];
    uint8_t rbuf[kMaxData];
    uint8_t resp[1 + kMaxData];

    for (;;) {
        uint8_t hdr[2];
        if (!recv_exact(sock, hdr, 2)) break;
        const uint16_t len = static_cast<uint16_t>((hdr[0] << 8) | hdr[1]);

        if (len < 3 || len > sizeof(req)) {
            ESP_LOGW(TAG, "bad request length %u, dropping client", len);
            break;
        }
        if (!recv_exact(sock, req, len)) break;

        const uint8_t addr = req[0];
        const uint8_t wlen = req[1];
        const uint8_t rlen = req[2];

        // Validate against the declared frame length and our caps.
        if (wlen > kMaxData || rlen > kMaxData || 3 + wlen != len) {
            ESP_LOGW(TAG, "malformed txn (addr=0x%02X wlen=%u rlen=%u len=%u)", addr, wlen, rlen, len);
            const uint8_t bad[1] = { 0xFE };
            if (!send_reply(sock, bad, 1)) break;
            continue;
        }

        esp_err_t err = ESP_ERR_INVALID_ARG;
        i2c_master_dev_handle_t dev = dev_for(addr);
        if (dev) {
            const uint8_t* w = req + 3;
            if (wlen > 0 && rlen > 0) {
                err = i2c_master_transmit_receive(dev, w, wlen, rbuf, rlen, kXferMs);
            } else if (wlen > 0) {
                err = i2c_master_transmit(dev, w, wlen, kXferMs);
            } else if (rlen > 0) {
                err = i2c_master_receive(dev, rbuf, rlen, kXferMs);
            }
        }

        resp[0] = (err == ESP_OK) ? 0x00 : 0x01;
        uint16_t rlen_out = 1;
        if (err == ESP_OK) {
            if (rlen > 0) { std::memcpy(resp + 1, rbuf, rlen); rlen_out = static_cast<uint16_t>(1 + rlen); }
            ESP_LOGI(TAG, "txn addr=0x%02X w=%u r=%u OK", addr, wlen, rlen);
        } else {
            ESP_LOGW(TAG, "txn addr=0x%02X w=%u r=%u failed: %s", addr, wlen, rlen, esp_err_to_name(err));
        }
        if (!send_reply(sock, resp, rlen_out)) break;
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

    i2c_init();
    wifi_init_softap();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, nullptr, 5, nullptr);
}
