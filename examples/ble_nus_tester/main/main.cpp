//
// ble_nus_tester - TEST HARNESS (not a shipped converter).
//
// Exercises the Bluetooth (BLE) converters from the spare ESP32-C3. It is a BLE
// *central* that scans for a peripheral advertising the name prefix "USA2-BT",
// connects, discovers the Nordic UART Service (NUS), subscribes to the TX
// characteristic (notifications), and periodically writes "hello #N" to the RX
// characteristic - logging everything it is notified.
//
// Used like wifi_tcp_tester was for the WiFi bridges:
//   * bt_to_rs232 / bt_to_rs485 with a wired loopback (TX->RX on the bridge board):
//     each "hello #N" travels BLE -> NUS RX -> UART TX -> loopback -> UART RX ->
//     NUS TX notify -> here, and should come straight back.
//   * For the framed converters (bt_to_can/spi/i2c) edit send_test_payload() to emit
//     the same [u16 len][payload] framing the bridge expects (see their READMEs),
//     exactly as wifi_tcp_tester was retargeted per converter.
//
// NUS UUIDs: Service 6E400001-.., RX(Wr) ..0002.., TX(Nfy) ..0003.
// Uses the NimBLE host in the central role.
//

#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

namespace {

constexpr char TAG[]         = "ble_tester";
constexpr char NAME_PREFIX[] = "USA2-BT";     // connect to the first match

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
uint16_t g_conn          = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_svc_end       = 0;      // NUS service end handle (for descriptor discovery)
uint16_t g_rx_handle     = 0;      // NUS RX value handle (we write here)
uint16_t g_tx_handle     = 0;      // NUS TX value handle (notifications)
uint16_t g_tx_cccd       = 0;      // NUS TX CCCD handle
bool     g_ready         = false;  // subscribed & ready to send

int gap_event(struct ble_gap_event* event, void* arg);

void start_scan()
{
    struct ble_gap_disc_params dp = {};
    dp.filter_duplicates = 1;
    dp.passive           = 0;      // active scan -> receive scan response (has the name)
    ble_gap_disc(g_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, nullptr);
    ESP_LOGI(TAG, "scanning for '%s*' ...", NAME_PREFIX);
}

// Does this advertisement carry a name starting with NAME_PREFIX?
bool name_matches(const uint8_t* data, uint8_t len)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, data, len) != 0 || fields.name == nullptr) return false;
    const size_t plen = std::strlen(NAME_PREFIX);
    return fields.name_len >= plen && std::memcmp(fields.name, NAME_PREFIX, plen) == 0;
}

// ---- GATT discovery chain: service -> chars -> TX CCCD -> subscribe ---------
int on_subscribe(uint16_t, const struct ble_gatt_error* error, struct ble_gatt_attr*, void*)
{
    if (error->status == 0) { g_ready = true; ESP_LOGI(TAG, "subscribed - ready to send"); }
    else                    { ESP_LOGE(TAG, "CCCD write failed: status=%d", error->status); }
    return 0;
}

int on_dsc(uint16_t conn, const struct ble_gatt_error* error, uint16_t,
           const struct ble_gatt_dsc* dsc, void*)
{
    if (error->status == 0 && dsc) {
        // 0x2902 = Client Characteristic Configuration Descriptor (CCCD)
        static const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0) g_tx_cccd = dsc->handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (g_tx_cccd == 0) { ESP_LOGE(TAG, "TX CCCD not found"); return 0; }
        const uint8_t enable[2] = { 0x01, 0x00 };   // enable notifications
        ble_gattc_write_flat(conn, g_tx_cccd, enable, sizeof(enable), on_subscribe, nullptr);
    }
    return 0;
}

int on_chr(uint16_t conn, const struct ble_gatt_error* error,
           const struct ble_gatt_chr* chr, void*)
{
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &nus_rx_uuid.u) == 0) g_rx_handle = chr->val_handle;
        if (ble_uuid_cmp(&chr->uuid.u, &nus_tx_uuid.u) == 0) g_tx_handle = chr->val_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (g_rx_handle == 0 || g_tx_handle == 0) { ESP_LOGE(TAG, "NUS chars not found"); return 0; }
        ESP_LOGI(TAG, "found NUS: rx=%u tx=%u", g_rx_handle, g_tx_handle);
        ble_gattc_disc_all_dscs(conn, g_tx_handle, g_svc_end, on_dsc, nullptr);
    }
    return 0;
}

int on_svc(uint16_t conn, const struct ble_gatt_error* error,
           const struct ble_gatt_svc* svc, void*)
{
    if (error->status == 0 && svc) {
        g_svc_end = svc->end_handle;
        ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle, on_chr, nullptr);
    } else if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "NUS service not found: status=%d", error->status);
    }
    return 0;
}

int on_mtu(uint16_t, const struct ble_gatt_error*, uint16_t mtu, void*)
{
    ESP_LOGI(TAG, "MTU negotiated: %u", mtu);
    return 0;
}

int gap_event(struct ble_gap_event* event, void*)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (name_matches(event->disc.data, event->disc.length_data)) {
            ESP_LOGI(TAG, "found peripheral, connecting");
            ble_gap_disc_cancel();
            ble_gap_connect(g_own_addr_type, &event->disc.addr, 30000, nullptr, gap_event, nullptr);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (conn=%u), discovering NUS", g_conn);
            ble_gattc_exchange_mtu(g_conn, on_mtu, nullptr);
            ble_gattc_disc_svc_by_uuid(g_conn, &nus_svc_uuid.u, on_svc, nullptr);
        } else {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            start_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason=%d), rescanning", event->disconnect.reason);
        g_conn = BLE_HS_CONN_HANDLE_NONE;
        g_ready = false;
        g_rx_handle = g_tx_handle = g_tx_cccd = g_svc_end = 0;
        start_scan();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == g_tx_handle) {
            uint8_t  buf[256];
            uint16_t out = 0;
            ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf) - 1, &out);
            buf[out] = '\0';
            ESP_LOGI(TAG, "RX notify (%u bytes): '%s'", out, reinterpret_cast<char*>(buf));
        }
        break;

    default:
        break;
    }
    return 0;
}

void on_sync() { ble_hs_id_infer_auto(0, &g_own_addr_type); start_scan(); }
void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason=%d", reason); }
void ble_host_task(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }

// Edit this to change what the tester sends. Default: raw "hello #N" (works with
// bt_to_rs232 / bt_to_rs485 + a wired loopback). For the framed converters, emit
// the [u16 len][payload] framing they expect (see each converter's README).
void send_test_payload(uint32_t counter)
{
    char msg[32];
    const int n = std::snprintf(msg, sizeof(msg), "hello #%u\n", static_cast<unsigned>(counter));
    ble_gattc_write_no_rsp_flat(g_conn, g_rx_handle, msg, n);
    ESP_LOGI(TAG, "TX 'hello #%u'", static_cast<unsigned>(counter));
}

void writer_task(void*)
{
    uint32_t counter = 0;
    for (;;) {
        if (g_ready && g_conn != BLE_HS_CONN_HANDLE_NONE) {
            send_test_payload(counter);
            ++counter;
        }
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

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_att_set_preferred_mtu(247);

    xTaskCreate(writer_task, "writer", 4096, nullptr, 5, nullptr);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "ble_nus_tester up (central) - will scan for '%s*'", NAME_PREFIX);
}
