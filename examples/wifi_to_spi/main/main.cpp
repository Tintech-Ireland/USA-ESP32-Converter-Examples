//
// wifi_to_spi - WiFi (TCP) <-> SPI (master) bridge for the USA2 board.
//
// SPI is a master-driven, full-duplex transaction protocol: the master clocks N
// bytes out on MOSI while simultaneously shifting N bytes in on MISO. There is no
// unsolicited slave->master traffic, so this bridge is request/response:
//
//     TCP client sends  [uint16 len][MOSI bytes]   (len bytes to clock out)
//     bridge runs one SPI transaction of len bytes
//     bridge replies    [uint16 len][MISO bytes]   (the bytes clocked in)
//
// The 2-byte big-endian (network-order) length prefix delimits each message on the
// TCP side, exactly as in wifi_to_can. The reply always echoes the request length
// (a full-duplex SPI transfer reads as many bytes as it writes).
//
// Topology: the board is a WiFi soft-AP with a TCP server on port 3335, and acts as
// SPI master on bus SPI2 (GPSPI2), 1 MHz, mode 0. The board has no dedicated SPI
// pins; the four signals are borrowed from the spare header + transceiver-input
// nets (see board_pins.h): MOSI=GPIO2, SCLK=GPIO5, MISO=GPIO7, CS=GPIO10. EN_5V is
// held LOW so the transceivers stay off and clear of the SPI bus.
//

#include <cstring>
#include <cerrno>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "wifi_spi";

constexpr char     AP_SSID[]   = "USA2-SPI";
constexpr char     AP_PASS[]   = "usa2spi1";   // >= 8 chars for WPA2
constexpr uint16_t TCP_PORT    = 3335;

constexpr spi_host_device_t kHost     = SPI2_HOST;
constexpr int               kClockHz  = 1000000;   // 1 MHz, mode 0
constexpr int               kMaxXfer  = 64;        // largest single SPI transaction

spi_device_handle_t g_spi = nullptr;

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

void spi_init()
{
    // Keep the transceiver 5 V rail OFF so the borrowed RS232/CAN/RS485 pins stay
    // idle and clear of the SPI bus.
    gpio_reset_pin(Board::EN_5V);
    gpio_set_direction(Board::EN_5V, GPIO_MODE_OUTPUT);
    gpio_set_level(Board::EN_5V, 0);

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = Board::SPI_MOSI;
    bus.miso_io_num     = Board::SPI_MISO;
    bus.sclk_io_num     = Board::SPI_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = kMaxXfer;
    ESP_ERROR_CHECK(spi_bus_initialize(kHost, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = kClockHz;
    dev.mode           = 0;
    dev.spics_io_num   = Board::SPI_CS;
    dev.queue_size     = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(kHost, &dev, &g_spi));

    ESP_LOGI(TAG, "SPI master up: SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d @%dHz mode0",
             Board::SPI_SCLK, Board::SPI_MOSI, Board::SPI_MISO, Board::SPI_CS, kClockHz);
}

// ---- blocking socket helpers ----------------------------------------------
// Read exactly n bytes into buf. Returns false on EOF or error.
bool recv_exact(int sock, uint8_t* buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int r = recv(sock, buf + got, n - got, 0);
        if (r > 0)                          { got += static_cast<size_t>(r); }
        else if (r < 0 && errno == EINTR)   { continue; }
        else                                { return false; }   // EOF or error
    }
    return true;
}

bool send_all(int sock, const uint8_t* p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        const int s = send(sock, p + sent, n - sent, 0);
        if (s > 0)                          { sent += static_cast<size_t>(s); }
        else if (s < 0 && errno == EINTR)   { continue; }
        else                                { return false; }
    }
    return true;
}

// Pump one connected client: [len][MOSI] -> SPI -> [len][MISO], until it closes.
void bridge_client(int sock)
{
    uint8_t mosi[kMaxXfer];
    uint8_t miso[kMaxXfer];

    for (;;) {
        uint8_t hdr[2];
        if (!recv_exact(sock, hdr, 2)) break;                 // client closed / error
        const uint16_t len = static_cast<uint16_t>((hdr[0] << 8) | hdr[1]);

        if (len == 0 || len > kMaxXfer) {
            ESP_LOGW(TAG, "bad transaction length %u (max %d), dropping client", len, kMaxXfer);
            break;
        }
        if (!recv_exact(sock, mosi, len)) break;

        std::memset(miso, 0, len);
        spi_transaction_t t = {};
        t.length    = static_cast<size_t>(len) * 8;           // in bits
        t.tx_buffer = mosi;
        t.rx_buffer = miso;

        const esp_err_t err = spi_device_transmit(g_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPI transfer failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "TCP->SPI %u byte transaction", len);

        const uint8_t reply_hdr[2] = { hdr[0], hdr[1] };      // reply length == request length
        if (!send_all(sock, reply_hdr, 2) || !send_all(sock, miso, len)) break;
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

    spi_init();
    wifi_init_softap();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, nullptr, 5, nullptr);
}
