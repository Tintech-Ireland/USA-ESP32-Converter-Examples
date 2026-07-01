#pragma once

//
// modbus_rtu.h
//
// Lightweight Modbus RTU transport for the USA2 board, running over an ESP-IDF UART
// configured for RS485 half-duplex. Header-only. Provides both roles:
//   * Modbus::RtuMaster - initiates transactions (client).
//   * Modbus::RtuSlave  - answers transactions (server).
//
// Modbus RTU wraps a Protocol Data Unit (PDU) in an Application Data Unit (ADU):
//
//     [ address (1) ][ PDU (function + data) ][ CRC-16/MODBUS (2, LE) ]
//
// Frames are delimited on the wire by >= 3.5-character-time silence. These classes
// build/parse the ADU, and reassemble incoming frames by detecting that inter-frame
// gap. The IDF RS485 half-duplex mode auto-toggles the DE line and suppresses the
// local echo, so both roles share the same UART setup.
//

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace Modbus {

constexpr size_t kMaxAdu = 256;   // Modbus RTU max ADU (address + 253 PDU + 2 CRC)
constexpr size_t kMaxPdu = 253;

enum class Status { Ok, Timeout, CrcError, BadAddress, TooLong, InvalidArg, NotReady };

inline const char* to_string(Status s)
{
    switch (s) {
    case Status::Ok:         return "ok";
    case Status::Timeout:    return "timeout";
    case Status::CrcError:   return "crc-error";
    case Status::BadAddress: return "bad-address";
    case Status::TooLong:    return "response-too-long";
    case Status::InvalidArg: return "invalid-arg";
    case Status::NotReady:   return "not-ready";
    }
    return "?";
}

// CRC-16/MODBUS: poly 0xA001 (reflected 0x8005), init 0xFFFF. Appended low byte first.
inline uint16_t crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

// ----- shared helpers -------------------------------------------------------

// RTU inter-frame gap = 3.5 character times (11 bits/char worst case), min 2 ms.
inline uint32_t rtu_gap_ms(int baud)
{
    uint32_t g = static_cast<uint32_t>((3.5f * 11.0f * 1000.0f) / static_cast<float>(baud)) + 1;
    return (g < 2) ? 2 : g;
}

inline esp_err_t configure_rs485(uart_port_t port, int baud, gpio_num_t tx, gpio_num_t rx,
                                 gpio_num_t de, uart_word_length_t data, uart_parity_t parity,
                                 uart_stop_bits_t stop, int buf_bytes)
{
    uart_config_t cfg = {};
    cfg.baud_rate  = baud;
    cfg.data_bits  = data;
    cfg.parity     = parity;
    cfg.stop_bits  = stop;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t e;
    if ((e = uart_driver_install(port, buf_bytes, buf_bytes, 0, nullptr, 0)) != ESP_OK) return e;
    if ((e = uart_param_config(port, &cfg)) != ESP_OK) return e;
    if ((e = uart_set_pin(port, tx, rx, de, UART_PIN_NO_CHANGE)) != ESP_OK) return e;
    if ((e = uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX)) != ESP_OK) return e;
    return ESP_OK;
}

// Read one RTU frame: bytes until a >= gap_ms silence follows data, or timeout.
// Returns the number of bytes captured (0 = nothing arrived within timeout).
inline size_t read_rtu_frame(uart_port_t port, uint8_t* buf, size_t cap,
                             uint32_t gap_ms, uint32_t timeout_ms)
{
    size_t  got = 0;
    bool    started = false;
    const int64_t end_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < end_us && got < cap) {
        const int n = uart_read_bytes(port, buf + got, cap - got,
                                      pdMS_TO_TICKS(started ? gap_ms : 20));
        if (n > 0)        { got += static_cast<size_t>(n); started = true; }
        else if (started) { break; }         // gap after data -> frame complete
    }
    return got;
}

inline void send_adu(uart_port_t port, uint8_t addr, const uint8_t* pdu, size_t pduLen)
{
    uint8_t adu[kMaxAdu];
    adu[0] = addr;
    std::memcpy(adu + 1, pdu, pduLen);
    const uint16_t crc = crc16(adu, 1 + pduLen);
    adu[1 + pduLen] = static_cast<uint8_t>(crc & 0xFF);   // CRC low byte first
    adu[2 + pduLen] = static_cast<uint8_t>(crc >> 8);
    uart_write_bytes(port, reinterpret_cast<const char*>(adu), 3 + pduLen);
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));           // block until the bus is released
}

// ----- Modbus RTU master (client) -------------------------------------------
class RtuMaster {
public:
    esp_err_t begin(uart_port_t port, int baud, gpio_num_t tx, gpio_num_t rx, gpio_num_t de,
                    uart_word_length_t data   = UART_DATA_8_BITS,
                    uart_parity_t      parity = UART_PARITY_DISABLE,
                    uart_stop_bits_t   stop   = UART_STOP_BITS_1)
    {
        port_ = port;
        gap_ms_ = rtu_gap_ms(baud);
        const esp_err_t e = configure_rs485(port, baud, tx, rx, de, data, parity, stop, kBufBytes);
        ready_ = (e == ESP_OK);
        return e;
    }

    // One RTU transaction: send [addr|pdu|crc], read the response, strip address+CRC,
    // and copy the response PDU into outPdu (*outLen in/out = capacity/size). An
    // exception response (function|0x80 + code) comes back as Ok with a 2-byte PDU.
    Status transceive(uint8_t slave, const uint8_t* pdu, size_t pduLen,
                      uint8_t* outPdu, size_t* outLen, uint32_t timeoutMs)
    {
        if (!ready_)                          return Status::NotReady;
        if (!pdu || !outPdu || !outLen)       return Status::InvalidArg;
        if (pduLen == 0 || pduLen > kMaxPdu)  return Status::InvalidArg;

        uart_flush_input(port_);
        send_adu(port_, slave, pdu, pduLen);

        uint8_t resp[kMaxAdu];
        const size_t got = read_rtu_frame(port_, resp, sizeof(resp), gap_ms_, timeoutMs);
        if (got < 4)                          return Status::Timeout;   // addr+fc+crc min
        const uint16_t have = static_cast<uint16_t>(resp[got - 2] | (resp[got - 1] << 8));
        if (have != crc16(resp, got - 2))     return Status::CrcError;
        if (resp[0] != slave)                 return Status::BadAddress;

        const size_t plen = got - 3;          // strip address + 2 CRC bytes
        if (plen > *outLen)                   return Status::TooLong;
        std::memcpy(outPdu, resp + 1, plen);
        *outLen = plen;
        return Status::Ok;
    }

private:
    static constexpr int kBufBytes = 512;
    uart_port_t port_   = UART_NUM_1;
    uint32_t    gap_ms_ = 4;
    bool        ready_  = false;
};

// ----- Modbus RTU slave (server) --------------------------------------------
// Transport only: it receives request frames and sends response frames. The
// application supplies the register model and function-code handling.
class RtuSlave {
public:
    esp_err_t begin(uart_port_t port, int baud, gpio_num_t tx, gpio_num_t rx, gpio_num_t de,
                    uart_word_length_t data   = UART_DATA_8_BITS,
                    uart_parity_t      parity = UART_PARITY_DISABLE,
                    uart_stop_bits_t   stop   = UART_STOP_BITS_1)
    {
        port_ = port;
        gap_ms_ = rtu_gap_ms(baud);
        const esp_err_t e = configure_rs485(port, baud, tx, rx, de, data, parity, stop, kBufBytes);
        ready_ = (e == ESP_OK);
        return e;
    }

    // Wait up to timeoutMs for a valid request frame. On Ok, *outAddr is the target
    // address and outPdu/*outLen is the request PDU (function + data).
    Status receive(uint8_t* outAddr, uint8_t* outPdu, size_t* outLen, uint32_t timeoutMs)
    {
        if (!ready_)                       return Status::NotReady;
        if (!outAddr || !outPdu || !outLen) return Status::InvalidArg;

        uint8_t frame[kMaxAdu];
        const size_t got = read_rtu_frame(port_, frame, sizeof(frame), gap_ms_, timeoutMs);
        if (got == 0)                      return Status::Timeout;
        if (got < 4)                       return Status::Timeout;   // runt frame
        const uint16_t have = static_cast<uint16_t>(frame[got - 2] | (frame[got - 1] << 8));
        if (have != crc16(frame, got - 2)) return Status::CrcError;

        const size_t plen = got - 3;
        if (plen > *outLen)                return Status::TooLong;
        *outAddr = frame[0];
        std::memcpy(outPdu, frame + 1, plen);
        *outLen = plen;
        return Status::Ok;
    }

    // Send a response ADU (address + PDU + CRC) back to the master.
    void reply(uint8_t addr, const uint8_t* pdu, size_t pduLen)
    {
        if (!ready_ || pduLen == 0 || pduLen > kMaxPdu) return;
        send_adu(port_, addr, pdu, pduLen);
    }

private:
    static constexpr int kBufBytes = 512;
    uart_port_t port_   = UART_NUM_1;
    uint32_t    gap_ms_ = 4;
    bool        ready_  = false;
};

} // namespace Modbus
