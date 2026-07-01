//
// modbus_rtu_slave - minimal Modbus RTU slave (server) on RS485 for the USA2 board.
//
// A test peer for wifi_modbus_gateway: put this board on the RS485 bus at slave
// address 1, and poll it over Modbus TCP through the gateway (or with any RTU master).
// It serves a small holding-register bank via the `modbus` component's RtuSlave
// transport (which handles RTU framing + CRC); this file supplies the register model
// and function-code handling.
//
// Registers (16 x uint16, addresses 0..15):
//     reg 0  = uptime in seconds        (live, updated continuously)
//     reg 1  = number of requests served (increments each request)
//     reg 2..15 = general read/write registers, pre-set to 0x1002..0x100F
//
// Supported function codes:
//     0x03 Read Holding Registers      0x04 Read Input Registers (same bank)
//     0x06 Write Single Register       0x10 Write Multiple Registers
// Anything else / out-of-range returns the appropriate Modbus exception.
//
// RS485: UART1 via the SP3485EN (TX=GPIO9, RX=GPIO7, DE=GPIO10), 9600 8N1 by default.
// The line settings MUST match the master/gateway (see kBaud / kParity).
//

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board_pins.h"
#include "modbus_rtu.h"

namespace {

constexpr char    TAG[]        = "mb_slave";
constexpr uint8_t kSlaveAddr   = 1;
constexpr int     kBaud        = 9600;
constexpr uart_parity_t kParity = UART_PARITY_DISABLE;    // 8N1; match the master
constexpr int     kNReg        = 16;

Modbus::RtuSlave g_slave;
uint16_t         g_hold[kNReg];

// Build a Modbus exception PDU: [function|0x80][exception code]. Returns its length.
size_t make_exception(uint8_t fc, uint8_t code, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(fc | 0x80);
    out[1] = code;
    return 2;
}

// Handle a request PDU, writing the response PDU to `out`. Returns response length.
size_t handle_request(const uint8_t* pdu, size_t len, uint8_t* out)
{
    if (len < 1) return 0;
    const uint8_t fc = pdu[0];

    switch (fc) {
    case 0x03:   // Read Holding Registers
    case 0x04: { // Read Input Registers (aliased to the same bank for the demo)
        if (len < 5) return make_exception(fc, 0x03, out);
        const uint16_t start = static_cast<uint16_t>((pdu[1] << 8) | pdu[2]);
        const uint16_t count = static_cast<uint16_t>((pdu[3] << 8) | pdu[4]);
        if (count < 1 || count > 125)       return make_exception(fc, 0x03, out);  // illegal data value
        if (start + count > kNReg)          return make_exception(fc, 0x02, out);  // illegal data address
        out[0] = fc;
        out[1] = static_cast<uint8_t>(count * 2);
        for (uint16_t i = 0; i < count; ++i) {
            out[2 + 2 * i] = static_cast<uint8_t>(g_hold[start + i] >> 8);
            out[3 + 2 * i] = static_cast<uint8_t>(g_hold[start + i] & 0xFF);
        }
        return static_cast<size_t>(2 + count * 2);
    }
    case 0x06: { // Write Single Register
        if (len < 5) return make_exception(fc, 0x03, out);
        const uint16_t addr = static_cast<uint16_t>((pdu[1] << 8) | pdu[2]);
        const uint16_t val  = static_cast<uint16_t>((pdu[3] << 8) | pdu[4]);
        if (addr >= kNReg)                  return make_exception(fc, 0x02, out);
        g_hold[addr] = val;
        std::memcpy(out, pdu, 5);           // echo the request back
        return 5;
    }
    case 0x10: { // Write Multiple Registers
        if (len < 6) return make_exception(fc, 0x03, out);
        const uint16_t start = static_cast<uint16_t>((pdu[1] << 8) | pdu[2]);
        const uint16_t count = static_cast<uint16_t>((pdu[3] << 8) | pdu[4]);
        const uint8_t  bc    = pdu[5];
        if (count < 1 || count > 123 || bc != count * 2 || len < 6u + bc)
            return make_exception(fc, 0x03, out);
        if (start + count > kNReg)          return make_exception(fc, 0x02, out);
        for (uint16_t i = 0; i < count; ++i)
            g_hold[start + i] = static_cast<uint16_t>((pdu[6 + 2 * i] << 8) | pdu[7 + 2 * i]);
        out[0] = fc; out[1] = pdu[1]; out[2] = pdu[2]; out[3] = pdu[3]; out[4] = pdu[4];
        return 5;
    }
    default:
        return make_exception(fc, 0x01, out);   // illegal function
    }
}

void slave_task(void*)
{
    uint8_t pdu[Modbus::kMaxPdu];
    uint8_t resp[Modbus::kMaxPdu];
    for (;;) {
        g_hold[0] = static_cast<uint16_t>(esp_timer_get_time() / 1000000);   // uptime seconds

        uint8_t addr;
        size_t  len = sizeof(pdu);
        const Modbus::Status st = g_slave.receive(&addr, pdu, &len, 1000);
        if (st != Modbus::Status::Ok) continue;                 // timeout / CRC error -> ignore
        if (addr != kSlaveAddr && addr != 0) continue;          // not addressed to us

        const size_t rlen = handle_request(pdu, len, resp);
        ++g_hold[1];
        ESP_LOGI(TAG, "req addr=%u fc=0x%02X (%u B) -> %u B reply%s",
                 addr, pdu[0], static_cast<unsigned>(len), static_cast<unsigned>(rlen),
                 addr == 0 ? " [broadcast: suppressed]" : "");

        if (addr != 0 && rlen > 0) g_slave.reply(kSlaveAddr, resp, rlen);   // broadcast gets no reply
    }
}

} // namespace

extern "C" void app_main(void)
{
    for (int i = 0; i < kNReg; ++i) g_hold[i] = static_cast<uint16_t>(0x1000 + i);

    ESP_ERROR_CHECK(g_slave.begin(UART_NUM_1, kBaud, Board::RS485_TX, Board::RS485_RX,
                                  Board::RS485_DE, UART_DATA_8_BITS, kParity, UART_STOP_BITS_1));
    ESP_LOGI(TAG, "Modbus RTU slave addr=%u up on RS485 (TX=GPIO%d RX=GPIO%d DE=GPIO%d) @%d 8N1",
             kSlaveAddr, Board::RS485_TX, Board::RS485_RX, Board::RS485_DE, kBaud);

    xTaskCreate(slave_task, "mb_slave", 4096, nullptr, 8, nullptr);
}
