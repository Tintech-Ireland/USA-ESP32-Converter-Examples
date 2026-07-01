//
// bt_to_i2c - Bluetooth LE <-> I2C (master) bridge for the USA2 board.
//
// BLE-only transport via the Nordic UART Service (NUS). I2C is master-driven and
// transactional, so - exactly as in wifi_to_i2c - this bridge is request/response
// over the framed NUS byte stream:
//
//   central writes  [u16 len][ addr | wlen | rlen | write bytes(wlen) ]
//   bridge runs one I2C transaction (write / read / write-then-read)
//   bridge notifies [u16 len][ status | read bytes(rlen, only if status==0) ]
//
//     addr    7-bit device address
//     wlen    bytes to write (0..kMaxData),  rlen bytes to read (0..kMaxData)
//     status  0 = ESP_OK, non-zero = failed; 0xFE = malformed request
//
// The 2-byte big-endian length prefix delimits each message (NUS writes /
// notifications can fragment). I2C master on the spare header (SDA=GPIO0, SCL=GPIO1),
// 100 kHz, internal pull-ups; ESP-IDF v5.x bus-based driver (driver/i2c_master.h).
// A device handle is created per address on demand and cached.
//
// NUS UUIDs: Service 6E400001-.., RX ..0002.., TX ..0003.
// Uses the NimBLE host (CONFIG_BT_NIMBLE_ENABLED).
//

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
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

constexpr char TAG[]      = "bt_i2c";
constexpr char DEV_NAME[] = "USA2-BT-I2C";

constexpr uint32_t kSclHz   = 100000;    // 100 kHz standard mode
constexpr int      kMaxData = 32;        // cap on wlen / rlen per transaction
constexpr int      kXferMs  = 1000;

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

i2c_master_bus_handle_t g_bus      = nullptr;
i2c_master_dev_handle_t g_dev      = nullptr;
uint16_t                g_dev_addr = 0xFFFF;

StreamBufferHandle_t g_rx_stream = nullptr;
SemaphoreHandle_t    g_tx_mutex  = nullptr;

void start_advertising();

i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    if (g_dev && g_dev_addr == addr) return g_dev;
    if (g_dev) { i2c_master_bus_rm_device(g_dev); g_dev = nullptr; g_dev_addr = 0xFFFF; }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = addr;
    dc.scl_speed_hz    = kSclHz;
    if (i2c_master_bus_add_device(g_bus, &dc, &g_dev) != ESP_OK) { g_dev = nullptr; return nullptr; }
    g_dev_addr = addr;
    return g_dev;
}

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
    uint8_t buf[2 + 1 + kMaxData];
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

// BLE -> I2C -> BLE: reassemble [len][addr|wlen|rlen|wbytes], run the transaction,
// reply [len][status|rbytes].
void bridge_task(void*)
{
    uint8_t acc[3 + kMaxData];
    size_t  accLen = 0;
    uint8_t tmp[128];
    uint8_t rbuf[kMaxData];
    uint8_t resp[1 + kMaxData];
    for (;;) {
        const size_t got = xStreamBufferReceive(g_rx_stream, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (got > 0) {
            if (accLen + got > sizeof(acc)) { accLen = 0; }         // overflow -> resync
            else { std::memcpy(acc + accLen, tmp, got); accLen += got; }
        }
        while (accLen >= 2) {
            const uint16_t len = static_cast<uint16_t>((acc[0] << 8) | acc[1]);
            if (len < 3 || len > sizeof(acc)) { ESP_LOGW(TAG, "framing error (len=%u), resync", len); accLen = 0; break; }
            if (accLen < 2u + len) break;

            const uint8_t* p    = acc + 2;
            const uint8_t  addr = p[0];
            const uint8_t  wlen = p[1];
            const uint8_t  rlen = p[2];

            uint16_t rlen_out = 1;
            if (wlen > kMaxData || rlen > kMaxData || 3 + wlen != len) {
                ESP_LOGW(TAG, "malformed txn (addr=0x%02X w=%u r=%u len=%u)", addr, wlen, rlen, len);
                resp[0] = 0xFE;
            } else {
                esp_err_t err = ESP_ERR_INVALID_ARG;
                i2c_master_dev_handle_t dev = dev_for(addr);
                if (dev) {
                    const uint8_t* w = p + 3;
                    if (wlen > 0 && rlen > 0)      err = i2c_master_transmit_receive(dev, w, wlen, rbuf, rlen, kXferMs);
                    else if (wlen > 0)             err = i2c_master_transmit(dev, w, wlen, kXferMs);
                    else if (rlen > 0)             err = i2c_master_receive(dev, rbuf, rlen, kXferMs);
                }
                resp[0] = (err == ESP_OK) ? 0x00 : 0x01;
                if (err == ESP_OK) {
                    if (rlen > 0) { std::memcpy(resp + 1, rbuf, rlen); rlen_out = static_cast<uint16_t>(1 + rlen); }
                    ESP_LOGI(TAG, "txn addr=0x%02X w=%u r=%u OK", addr, wlen, rlen);
                } else {
                    ESP_LOGW(TAG, "txn addr=0x%02X w=%u r=%u failed: %s", addr, wlen, rlen, esp_err_to_name(err));
                }
            }
            send_frame(resp, rlen_out);

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

    i2c_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEV_NAME));
    ble_att_set_preferred_mtu(247);

    xTaskCreate(bridge_task, "ble2i2c", 4096, nullptr, 5, nullptr);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "bt_to_i2c up: NUS <-> I2C master (SDA=GPIO%d SCL=GPIO%d)",
             Board::I2C_SDA, Board::I2C_SCL);
}
