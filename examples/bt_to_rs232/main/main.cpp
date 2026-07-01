//
// bt_to_rs232 - transparent Bluetooth LE <-> RS232 bridge for the USA2 board.
//
// The ESP32-C3 is BLE-only (no Classic Bluetooth / SPP), so the transport is a
// GATT peripheral exposing the Nordic UART Service (NUS) - the de-facto "BLE
// serial" profile. A BLE central (nRF Connect, Serial Bluetooth Terminal in BLE
// mode, or your own app) connects and:
//   * writes bytes to the RX characteristic  -> forwarded to RS232 TX
//   * subscribes to the TX characteristic     -> receives RS232 RX as notifications
//
// NUS UUIDs (128-bit):
//   Service : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX (Wr) : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E   (central -> device)
//   TX (Nfy): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E   (device  -> central)
//
// Raw bytes both ways, no framing (the byte-stream twin of wifi_to_rs232). BLE
// notifications are capped at (ATT_MTU - 3) bytes, so the UART->BLE side chunks to
// the negotiated MTU; we request a larger MTU (247) up front for better throughput.
//
// RS232 link: UART1 via the on-board MAX3232 (TX=GPIO5, RX=GPIO4), 115200 8N1.
// Uses the NimBLE host (CONFIG_BT_NIMBLE_ENABLED).
//

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
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

constexpr char TAG[]      = "bt_rs232";
constexpr char DEV_NAME[] = "USA2-BT-RS232";

// --- RS232 UART ---
constexpr uart_port_t kPort    = UART_NUM_1;
constexpr int         kBaud    = 115200;
constexpr int         kUartBuf = 2048;

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

void start_advertising();

// Central wrote to the RX characteristic -> push the bytes out RS232.
int rx_access_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t  buf[512];
    uint16_t out = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (out > 0) uart_write_bytes(kPort, reinterpret_cast<const char*>(buf), out);
    return 0;
}

// TX characteristic is notify-only; nothing to serve on a direct read.
int tx_access_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt*, void*)
{
    return 0;
}

const struct ble_gatt_chr_def nus_chrs[] = {
    {
        .uuid       = &nus_rx_uuid.u,
        .access_cb  = rx_access_cb,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = &nus_tx_uuid.u,
        .access_cb  = tx_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_tx_val_handle,
    },
    { 0 },
};

const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &nus_svc_uuid.u,
        .characteristics = nus_chrs,
    },
    { 0 },
};

int gap_event(struct ble_gap_event* event, void*)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected (conn=%u)", g_conn_handle);
        } else {
            start_advertising();                       // connection failed - re-advertise
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason=%d)", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_notify_on   = false;
        start_advertising();
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

    // The 128-bit service UUID doesn't fit alongside the name in the 31-byte adv
    // packet, so advertise it in the scan response.
    struct ble_hs_adv_fields rsp = {};
    rsp.uuids128            = const_cast<ble_uuid128_t*>(&nus_svc_uuid);
    rsp.num_uuids128        = 1;
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

void on_sync()
{
    ble_hs_id_infer_auto(0, &g_own_addr_type);
    start_advertising();
}

void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

void ble_host_task(void*)
{
    nimble_port_run();                 // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
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

// RS232 RX -> BLE notifications, chunked to the negotiated ATT MTU.
void uart_to_ble_task(void*)
{
    uint8_t buf[256];
    for (;;) {
        const int n = uart_read_bytes(kPort, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0 || g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_on) continue;

        const int mtu   = ble_att_mtu(g_conn_handle);
        const int chunk = (mtu > 3) ? (mtu - 3) : 20;
        for (int off = 0; off < n; ) {
            const int c = (n - off < chunk) ? (n - off) : chunk;
            struct os_mbuf* om = ble_hs_mbuf_from_flat(buf + off, c);
            if (!om) { vTaskDelay(1); continue; }          // out of mbufs; retry
            // ble_gatts_notify_custom consumes (frees) om regardless of result.
            ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
            off += c;
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

    uart_init();

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEV_NAME));

    ble_att_set_preferred_mtu(247);

    xTaskCreate(uart_to_ble_task, "uart2ble", 4096, nullptr, 5, nullptr);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "bt_to_rs232 up: NUS <-> RS232 (TX=GPIO%d RX=GPIO%d) @%d 8N1",
             Board::RS232_TX, Board::RS232_RX, kBaud);
}
