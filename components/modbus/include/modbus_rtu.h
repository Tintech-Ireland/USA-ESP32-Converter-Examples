#pragma once

//
// modbus_rtu.h
//
// Lightweight Modbus RTU **master** transport for the USA2 board, running over an
// ESP-IDF UART configured for RS485 half-duplex. Header-only.
//
// Modbus RTU wraps a Protocol Data Unit (PDU) in an Application Data Unit (ADU):
//
//     [ slave address (1) ][ PDU (function + data) ][ CRC-16/MODBUS (2, LE) ]
//
// Frames are delimited on the wire by >= 3.5-character-time silence. This class
// builds the ADU, transmits it (the IDF RS485 half-duplex mode auto-toggles the
// DE line and suppresses the local echo), then reassembles the response frame by
// detecting that inter-frame gap, validates the CRC, and returns the response PDU.
//
// It is a master/client only (it initiates transactions). Pair it with a Modbus
// TCP server to build a TCP<->RTU gateway (see examples/wifi_modbus_gateway).
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

// Modbus RTU master bound to one UART in RS485 half-duplex mode.
class RtuMaster {
public:
    static constexpr size_t kMaxAdu = 256;   // Modbus RTU max ADU (address + 253 PDU + 2 CRC)
    static constexpr size_t kMaxPdu = 253;

    // Configure the UART for RS485 half-duplex (DE on the RTS pin). Call once.
    // Modbus spec default line format is 19200 8E1; many devices use 9600 8N1 —
    // pass what your bus uses.
    esp_err_t begin(uart_port_t port, int baud, gpio_num_t tx, gpio_num_t rx, gpio_num_t de,
                    uart_word_length_t data   = UART_DATA_8_BITS,
                    uart_parity_t      parity = UART_PARITY_DISABLE,
                    uart_stop_bits_t   stop   = UART_STOP_BITS_1)
    {
        port_ = port;

        // Inter-frame gap = 3.5 character times (11 bits/char worst case), min 2 ms.
        gap_ms_ = static_cast<uint32_t>((3.5f * 11.0f * 1000.0f) / static_cast<float>(baud)) + 1;
        if (gap_ms_ < 2) gap_ms_ = 2;

        uart_config_t cfg = {};
        cfg.baud_rate  = baud;
        cfg.data_bits  = data;
        cfg.parity     = parity;
        cfg.stop_bits  = stop;
        cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        esp_err_t e;
        if ((e = uart_driver_install(port, kBufBytes, kBufBytes, 0, nullptr, 0)) != ESP_OK) return e;
        if ((e = uart_param_config(port, &cfg)) != ESP_OK) return e;
        if ((e = uart_set_pin(port, tx, rx, de, UART_PIN_NO_CHANGE)) != ESP_OK) return e;
        if ((e = uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX)) != ESP_OK) return e;
        ready_ = true;
        return ESP_OK;
    }

    // Perform one RTU transaction: send [slave|pdu|crc], read the response, strip the
    // address+CRC, and copy the response PDU into outPdu (*outLen in/out = capacity/size).
    // An exception response (function|0x80 + exception code) is returned as Ok with a
    // 2-byte PDU — the caller inspects the high bit of outPdu[0].
    Status transceive(uint8_t slave, const uint8_t* pdu, size_t pduLen,
                      uint8_t* outPdu, size_t* outLen, uint32_t timeoutMs)
    {
        if (!ready_)                             return Status::NotReady;
        if (pdu == nullptr || outPdu == nullptr || outLen == nullptr) return Status::InvalidArg;
        if (pduLen == 0 || pduLen > kMaxPdu)     return Status::InvalidArg;

        uint8_t adu[kMaxAdu];
        adu[0] = slave;
        std::memcpy(adu + 1, pdu, pduLen);
        const uint16_t crc = crc16(adu, 1 + pduLen);
        adu[1 + pduLen] = static_cast<uint8_t>(crc & 0xFF);   // CRC low byte first
        adu[2 + pduLen] = static_cast<uint8_t>(crc >> 8);
        const size_t aduLen = 3 + pduLen;

        uart_flush_input(port_);
        uart_write_bytes(port_, reinterpret_cast<const char*>(adu), aduLen);
        uart_wait_tx_done(port_, pdMS_TO_TICKS(100));   // block until the bus is released

        // Reassemble the response frame: read until a >= gap_ms_ silence follows data,
        // or the overall timeout elapses.
        uint8_t resp[kMaxAdu];
        size_t  got = 0;
        bool    started = false;
        const int64_t end_us = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
        while (esp_timer_get_time() < end_us && got < sizeof(resp)) {
            const int n = uart_read_bytes(port_, resp + got, sizeof(resp) - got,
                                          pdMS_TO_TICKS(started ? gap_ms_ : 20));
            if (n > 0)        { got += static_cast<size_t>(n); started = true; }
            else if (started) { break; }                 // gap after data -> frame complete
        }

        if (got < 4)                                     return Status::Timeout;      // addr+fc+crc min
        const uint16_t want = crc16(resp, got - 2);
        const uint16_t have = static_cast<uint16_t>(resp[got - 2] | (resp[got - 1] << 8));
        if (have != want)                                return Status::CrcError;
        if (resp[0] != slave)                            return Status::BadAddress;

        const size_t plen = got - 3;                     // strip address + 2 CRC bytes
        if (plen > *outLen)                              return Status::TooLong;
        std::memcpy(outPdu, resp + 1, plen);
        *outLen = plen;
        return Status::Ok;
    }

private:
    static constexpr int kBufBytes = 512;
    uart_port_t port_    = UART_NUM_1;
    uint32_t    gap_ms_  = 4;
    bool        ready_   = false;
};

} // namespace Modbus
