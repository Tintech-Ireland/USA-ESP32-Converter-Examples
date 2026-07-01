#pragma once

//
// board_pins.h
//
// Single source of truth for the USA2 (USA-ESP32) bespoke ESP32-C3 board pinout.
// Derived from the KiCad schematic USA_HP.pdf (rev 0.1, 2025-05-08), sheet 3/6 (CPU),
// which maps the ESP32-C3-WROOM-02 GPIOs onto the on-board transceiver nets.
//
// All example code and converters should reference these symbols rather than
// hard-coding GPIO numbers, so that a board re-spin only needs to touch this file.
//
// MCU: ESP32-C3-WROOM-02 (RISC-V single core, USB-Serial-JTAG on GPIO18/19).
//

#include "driver/gpio.h"   // gpio_num_t

namespace Board {

    // -------------------------------------------------------------------------
    // RS232  -  MAX3232 (U4), 3.3 V logic, charge-pump level shifter.
    //           Lines TX_RS232 / RX_RS232 break out to connector J1.
    //           Wire up to a UART peripheral as a standard 2-wire TTL link.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t RS232_TX = GPIO_NUM_5;   // net RS232_TX  -> MAX3232 T1IN  (MCU transmits)
    constexpr gpio_num_t RS232_RX = GPIO_NUM_4;   // net RS232_RX  <- MAX3232 R1OUT (MCU receives)

    // -------------------------------------------------------------------------
    // RS485  -  SP3485EN (U2), half-duplex, A/B differential pair on J1.
    //           DE and ~RE are tied together and driven by RS485_DE.
    //           Use uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX) so the IDF
    //           UART driver toggles the direction pin automatically.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t RS485_TX = GPIO_NUM_9;   // net RS485_TX   -> SP3485 DI  (MCU transmits)
    constexpr gpio_num_t RS485_RX = GPIO_NUM_7;   // net RS485_RX   <- SP3485 RO  (MCU receives)
    constexpr gpio_num_t RS485_DE = GPIO_NUM_10;  // net RS485_TXEN -> SP3485 DE/~RE (driver enable)
                                                  //   Re-verified against USA_HP.pdf sheet 3/6: IO7=RS485_RX,
                                                  //   IO8=TX_CAN. A prior edit wrongly swapped RS485_RX (GPIO7)
                                                  //   with CAN_TX (GPIO8); confirmed on hardware. RS485_TX=GPIO9
                                                  //   is a strapping pin and carries the BOOT button - keep the
                                                  //   bus from holding it low during reset.

    // -------------------------------------------------------------------------
    // CAN  -  TJA1042T-3 (U9), CANH/CANL on J1, separate VIO logic rail.
    //         Drive with the ESP-IDF TWAI (two-wire automotive interface) driver.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t CAN_TX = GPIO_NUM_8;     // net TX_CAN -> TJA1042 TXD (MCU transmits) [strapping pin]
    constexpr gpio_num_t CAN_RX = GPIO_NUM_6;     // net RX_CAN <- TJA1042 RXD (MCU receives)
                                                  //   CAN_TX corrected GPIO7 -> GPIO8 per USA_HP.pdf sheet 3/6
                                                  //   (IO8=TX_CAN). CAN needs re-verification after this change.

    // -------------------------------------------------------------------------
    // UART0 / TTL console  -  brought out on the J1 "TXRX" header.
    //                         Default ESP-IDF stdio / log console.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t UART0_TX = GPIO_NUM_21;  // net TXD (IO21)
    constexpr gpio_num_t UART0_RX = GPIO_NUM_20;  // net RXD (IO20)

    // -------------------------------------------------------------------------
    // USB  -  native USB-Serial-JTAG of the ESP32-C3.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t USB_DP = GPIO_NUM_19;    // net D+ (IO19)
    constexpr gpio_num_t USB_DM = GPIO_NUM_18;    // net D- (IO18)

    // -------------------------------------------------------------------------
    // Power control
    // -------------------------------------------------------------------------
    constexpr gpio_num_t EN_5V = GPIO_NUM_3;      // net 5V_EN -> enables the 5 V boost (TPS60150).
                                                  //   Drive HIGH to power the transceiver rail.

    // -------------------------------------------------------------------------
    // General-purpose / spare GPIO.
    //
    // WARNING: these are the ONLY uncommitted GPIOs on the board, and they are
    // exposed only on the "GPIO1" test header which is marked DO-NOT-POPULATE
    // (DNP) on the schematic. They must be populated by hand to be usable.
    //
    //   * I2C (SDA + SCL) fits in two of these pins.
    //   * SPI (MOSI/MISO/SCLK/CS) needs four pins and therefore does NOT fit on
    //     the spare header alone - it must reuse the RS232/RS485/CAN GPIOs.
    //
    // GPIO2 is also an ESP32-C3 strapping pin - avoid driving it during reset.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t SPARE_IO0 = GPIO_NUM_0;  // net GPIO0 (has on-board 45k pull-up; not a strap)
    constexpr gpio_num_t SPARE_IO1 = GPIO_NUM_1;  // net GPIO1
    constexpr gpio_num_t SPARE_IO2 = GPIO_NUM_2;  // net GPIO2 (ESP32-C3 strapping pin)

    // -------------------------------------------------------------------------
    // Suggested I2C assignment (using two of the spare pins).
    // Provisional - confirm once the spare header strategy is decided.
    // -------------------------------------------------------------------------
    constexpr gpio_num_t I2C_SDA = SPARE_IO0;
    constexpr gpio_num_t I2C_SCL = SPARE_IO1;

    // -------------------------------------------------------------------------
    // SPI (master <-> slave demo).
    //
    // The board has no dedicated SPI pins, and SPI needs four signals, so three
    // are borrowed from transceiver nets. They are chosen to avoid bus contention:
    // every borrowed pin is a transceiver *input* (the transceiver chip never
    // drives it), so the ESP can drive/read it safely whether or not the
    // transceivers are powered. Keep EN_5V LOW (transceivers off) when running
    // SPI, so the RS232/RS485/CAN lines stay idle.
    //
    //   MOSI -> GPIO2  spare pin (note: GPIO2 is also a strapping pin)
    //   SCLK -> GPIO5  shares RS232_TX  (MAX3232 T1IN  - an input)
    //   MISO -> GPIO7  shares RS485_RX  (SP3485 RO - an OUTPUT; safe only while EN_5V is held LOW,
    //                                    so the SP3485 is powered down and RO stays high-Z)
    //   CS   -> GPIO10 shares RS485_TXEN(SP3485 DE      - an input)
    // -------------------------------------------------------------------------
    constexpr gpio_num_t SPI_MOSI = GPIO_NUM_2;
    constexpr gpio_num_t SPI_SCLK = GPIO_NUM_5;
    constexpr gpio_num_t SPI_MISO = GPIO_NUM_7;
    constexpr gpio_num_t SPI_CS   = GPIO_NUM_10;

} // namespace Board
