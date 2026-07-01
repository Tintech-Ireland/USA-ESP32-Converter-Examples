//
// bt_to_spi - Bluetooth LE <-> SPI (master) bridge for the USA2 board.
//
// BLE-only transport via the Nordic UART Service (NUS). SPI is a master-driven,
// full-duplex transaction protocol, so - exactly as in wifi_to_spi - this bridge is
// request/response over the framed NUS byte stream:
//
//   central writes  [u16 len][MOSI bytes]   (bytes to clock out, len 1..64)
//   bridge runs one SPI transaction of len bytes
//   bridge notifies [u16 len][MISO bytes]   (bytes clocked in, same length)
//
// The 2-byte big-endian length prefix delimits each message (NUS writes /
// notifications can fragment). A full-duplex SPI transfer reads as many bytes as it
// writes, so the reply length always equals the request length.
//
// SPI: master on bus SPI2 (GPSPI2), 1 MHz, mode 0. Borrowed pins (see board_pins.h):
// MOSI=GPIO2, SCLK=GPIO5, MISO=GPIO7, CS=GPIO10; EN_5V held LOW so the transceivers
// stay off the bus. NUS UUIDs: Service 6E400001-.., RX ..0002.., TX ..0003.
// Uses the NimBLE host (CONFIG_BT_NIMBLE_ENABLED).
//

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board_pins.h"

namespace {

constexpr char TAG[]      = "bt_spi";
constexpr char DEV_NAME[] = "USA2-BT-SPI";

constexpr spi_host_device_t kHost    = SPI2_HOST;
constexpr int               kClockHz = 1000000;    // 1 MHz, mode 0
constexpr int               kMaxXfer = 64;         // largest single SPI transaction

// --- NUS UUIDs (128-bit, little-endian byte order for BLE_UUID128_INIT) ---
const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

uint8_t  g_own_addr_type = 0;
uint16_t g_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_tx_val_handle = 0;
bool     g_notify_on     = false;

spi_device_handle_t  g_spi       = nullptr;
StreamBufferHandle_t g_rx_stream = nullptr;
SemaphoreHandle_t    g_tx_mutex  = nullptr;

void start_advertising();

// ---- BLE notify: send a byte run, chunked to MTU, serialized by mutex ------
void ble_send(const uint8_t* data, size_t len)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_on) return;
    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    const int mtu   = ble_att_mtu(g_conn_handle);
    const int chunk = (mtu > 3) ? (mtu - 3) : 20;
    for (size_t off = 0; off < len; ) {
        const size_t c = (len - off < static_cast<size_t>(chunk)) ? (len - off) : chunk;
        struct os_mbuf* om = ble_hs_mbuf_from_flat(data + off, c);
        if (!om) { vTaskDelay(1); continue; }
        if (ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om) != 0) break;
        off += c;
    }
    xSemaphoreGive(g_tx_mutex);
}

void send_frame(const uint8_t* payload, uint16_t plen)
{
    uint8_t buf[2 + kMaxXfer];
    buf[0] = static_cast<uint8_t>(plen >> 8);
    buf[1] = static_cast<uint8_t>(plen & 0xFF);
    std::memcpy(buf + 2, payload, plen);
    ble_send(buf, 2u + plen);
}

int rx_access_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t  buf[512];
    uint16_t out = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (out > 0) xStreamBufferSend(g_rx_stream, buf, out, 0);
    return 0;
}

int tx_access_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt*, void*) { return 0; }

const struct ble_gatt_chr_def nus_chrs[] = {
    { .uuid = &nus_rx_uuid.u, .access_cb = rx_access_cb,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &nus_tx_uuid.u, .access_cb = tx_access_cb,
      .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_tx_val_handle },
    { 0 },
};
const struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &nus_svc_uuid.u, .characteristics = nus_chrs },
    { 0 },
};

int gap_event(struct ble_gap_event* event, void*)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) { g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected (conn=%u)", g_conn_handle); }
        else start_advertising();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason=%d)", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE; g_notify_on = false; start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_tx_val_handle) {
            g_notify_on = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "TX notifications %s", g_notify_on ? "enabled" : "disabled");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated to %d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    }
    return 0;
}

void start_advertising()
{
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name  = reinterpret_cast<const uint8_t*>(ble_svc_gap_device_name());
    fields.name_len         = std::strlen(reinterpret_cast<const char*>(fields.name));
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields rsp = {};
    rsp.uuids128             = const_cast<ble_uuid128_t*>(&nus_svc_uuid);
    rsp.num_uuids128         = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params advp = {};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER,
                                     &advp, gap_event, nullptr);
    if (rc != 0) ESP_LOGE(TAG, "adv start failed: rc=%d", rc);
    else         ESP_LOGI(TAG, "advertising as '%s'", DEV_NAME);
}

void on_sync()  { ble_hs_id_infer_auto(0, &g_own_addr_type); start_advertising(); }
void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason=%d", reason); }
void ble_host_task(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }

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

// BLE -> SPI -> BLE: reassemble [len][MOSI], run one transaction, reply [len][MISO].
void bridge_task(void*)
{
    uint8_t acc[2 + kMaxXfer];
    size_t  accLen = 0;
    uint8_t tmp[128];
    uint8_t miso[kMaxXfer];
    for (;;) {
        const size_t got = xStreamBufferReceive(g_rx_stream, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (got > 0) {
            if (accLen + got > sizeof(acc)) { accLen = 0; }         // overflow -> resync
            else { std::memcpy(acc + accLen, tmp, got); accLen += got; }
        }
        while (accLen >= 2) {
            const uint16_t len = static_cast<uint16_t>((acc[0] << 8) | acc[1]);
            if (len == 0 || len > kMaxXfer) { ESP_LOGW(TAG, "framing error (len=%u), resync", len); accLen = 0; break; }
            if (accLen < 2u + len) break;

            std::memset(miso, 0, len);
            spi_transaction_t t = {};
            t.length    = static_cast<size_t>(len) * 8;
            t.tx_buffer = acc + 2;
            t.rx_buffer = miso;
            if (spi_device_transmit(g_spi, &t) == ESP_OK) {
                ESP_LOGI(TAG, "BLE->SPI %u byte transaction", len);
                send_frame(miso, len);
            } else {
                ESP_LOGW(TAG, "SPI transfer failed");
            }

            const size_t consumed = 2u + len;
            std::memmove(acc, acc + consumed, accLen - consumed);
            accLen -= consumed;
        }
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

    g_rx_stream = xStreamBufferCreate(512, 1);
    g_tx_mutex  = xSemaphoreCreateMutex();

    spi_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEV_NAME));
    ble_att_set_preferred_mtu(247);

    xTaskCreate(bridge_task, "ble2spi", 4096, nullptr, 5, nullptr);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "bt_to_spi up: NUS <-> SPI master (1 MHz mode0)");
}
