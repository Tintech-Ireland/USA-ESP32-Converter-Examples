//
// bt_to_can - Bluetooth LE <-> CAN (TWAI) bridge for the USA2 board.
//
// BLE-only transport via the Nordic UART Service (NUS). CAN is message-oriented and
// the NUS characteristics carry an unframed byte stream (writes / notifications that
// can fragment), so - exactly as in wifi_to_can - each message is delimited with a
// 2-byte big-endian length prefix. Every transfer is  [u16 len][payload]  and the
// payload is one CAN frame:
//
//     length = 6 + dlc
//       [0..3] identifier  uint32 big-endian  (11-bit std or 29-bit extended)
//       [4]    flags        bit0 = extended id, bit1 = remote (RTR)
//       [5]    dlc          0..8
//       [6..]  data         dlc bytes
//
// A BLE central writes framed CAN frames to the RX characteristic (-> transmitted on
// the bus) and subscribes to the TX characteristic to receive framed bus frames.
// NUS RX bytes are handed to a stream buffer so the BLE host callback stays lean; a
// task reassembles frames and transmits them. A second task forwards received bus
// frames back as notifications. Outbound notifications are chunked to the negotiated
// ATT MTU and serialized with a mutex so frames never interleave on the wire.
//
// CAN: TJA1042, TX=GPIO8, RX=GPIO6, 500 kbit/s (120 ohm termination at each bus end).
// NUS UUIDs: Service 6E400001-.., RX(Wr) ..0002.., TX(Nfy) ..0003-B5A3-F393-E0A9-E50E24DCCA9E.
// Uses the NimBLE host (CONFIG_BT_NIMBLE_ENABLED).
//

#include <cstring>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
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

constexpr char TAG[]      = "bt_can";
constexpr char DEV_NAME[] = "USA2-BT-CAN";

constexpr int kMaxPayload = 6 + 8;     // largest CAN frame payload
constexpr int kAccCap     = 64;        // reassembly accumulator

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

StreamBufferHandle_t g_rx_stream = nullptr;   // BLE RX bytes -> process task
SemaphoreHandle_t    g_tx_mutex  = nullptr;   // serializes outbound notifications

void start_advertising();

// ---- CAN <-> payload codec (identical to wifi_to_can) ----------------------
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
        if (!om) { vTaskDelay(1); continue; }              // out of mbufs; retry
        if (ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om) != 0) break;
        off += c;
    }
    xSemaphoreGive(g_tx_mutex);
}

void send_frame(const uint8_t* payload, uint16_t plen)
{
    uint8_t buf[2 + kMaxPayload];
    buf[0] = static_cast<uint8_t>(plen >> 8);
    buf[1] = static_cast<uint8_t>(plen & 0xFF);
    std::memcpy(buf + 2, payload, plen);
    ble_send(buf, 2u + plen);
}

// Central wrote framed data to RX -> hand raw bytes to the process task.
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

void twai_init()
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(Board::CAN_TX, Board::CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "TWAI up @500kbit/s (TX=GPIO%d RX=GPIO%d)", Board::CAN_TX, Board::CAN_RX);
}

// BLE -> CAN: reassemble length-prefixed frames and transmit on the bus.
void ble_to_can_task(void*)
{
    uint8_t acc[kAccCap];
    size_t  accLen = 0;
    uint8_t tmp[128];
    for (;;) {
        const size_t got = xStreamBufferReceive(g_rx_stream, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (got > 0) {
            if (accLen + got > sizeof(acc)) { accLen = 0; }          // overflow -> resync
            else { std::memcpy(acc + accLen, tmp, got); accLen += got; }
        }
        while (accLen >= 2) {
            const uint16_t len = static_cast<uint16_t>((acc[0] << 8) | acc[1]);
            if (len > kMaxPayload) { ESP_LOGW(TAG, "framing error (len=%u), resync", len); accLen = 0; break; }
            if (accLen < 2u + len) break;
            twai_message_t m;
            if (decode_can(acc + 2, len, m)) {
                if (twai_transmit(&m, pdMS_TO_TICKS(100)) == ESP_OK)
                    ESP_LOGI(TAG, "BLE->CAN id=0x%03" PRIx32 " dlc=%d", m.identifier, m.data_length_code);
                else
                    ESP_LOGW(TAG, "CAN TX failed (bus/termination?)");
            } else {
                ESP_LOGW(TAG, "bad CAN payload (len=%u)", len);
            }
            const size_t consumed = 2u + len;
            std::memmove(acc, acc + consumed, accLen - consumed);
            accLen -= consumed;
        }
    }
}

// CAN -> BLE: forward received bus frames as length-prefixed notifications.
void can_to_ble_task(void*)
{
    for (;;) {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(100)) == ESP_OK) {
            uint8_t payload[kMaxPayload];
            const uint16_t plen = encode_can(rx, payload);
            send_frame(payload, plen);
            ESP_LOGI(TAG, "CAN->BLE id=0x%03" PRIx32 " dlc=%d", rx.identifier, rx.data_length_code);
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

    twai_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEV_NAME));
    ble_att_set_preferred_mtu(247);

    xTaskCreate(ble_to_can_task, "ble2can", 4096, nullptr, 5, nullptr);
    xTaskCreate(can_to_ble_task, "can2ble", 4096, nullptr, 5, nullptr);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "bt_to_can up: NUS <-> CAN (TX=GPIO%d RX=GPIO%d) @500kbit/s",
             Board::CAN_TX, Board::CAN_RX);
}
