# USA-ESP32 (USA2) — ESP-IDF examples & converters

Libraries and example firmware for the bespoke **ESP32-C3-WROOM-02** "USA2" board
(schematic `USA_HP.pdf`). Built with **ESP-IDF v5.4**, C++17.

See [`BACKLOG.md`](BACKLOG.md) for the task list and open hardware questions, and
[`components/board/include/board_pins.h`](components/board/include/board_pins.h)
for the authoritative pin map.

## Layout

```
components/
  board/             header-only: board_pins.h (shared pinout, single source of truth)
  serial_transport/  header-only: SimpleProtocol framing + IDF UART backend (no GSL)
examples/
  can_to_can/        CAN (TWAI) peer-to-peer  [HW-VERIFIED]
  rs232_to_rs232/    RS232 UART loopback      [HW-VERIFIED]
  rs485_to_rs485/    RS485 half-duplex        [HW-VERIFIED]
  i2c_master_slave/  I2C master + slave       [HW-VERIFIED]
  spi_master_slave/  SPI master + slave       [HW-VERIFIED]
  serial_selftest/   serial_transport smoke test / converter template [builds]
  wifi_to_rs232/     transparent WiFi(TCP) <-> RS232 bridge   [HW-VERIFIED]
  wifi_to_rs485/     transparent WiFi(TCP) <-> RS485 bridge (half-duplex)     [HW-VERIFIED]
  wifi_tcp_tester/   test harness: WiFi STA + TCP client (drives the WiFi converters)
  wifi_to_can/       WiFi(TCP) <-> CAN bridge, 16-bit length-prefixed frames  [HW-VERIFIED]
  wifi_to_spi/       WiFi(TCP) <-> SPI master request/response bridge     [HW-VERIFIED]
  wifi_to_i2c/       WiFi(TCP) <-> I2C master request/response bridge     [HW-VERIFIED]
  bt_to_rs232/       BLE (NUS) <-> RS232 transparent bridge               [HW-VERIFIED]
  bt_to_rs485/       BLE (NUS) <-> RS485 transparent bridge (half-duplex) [HW-VERIFIED]
  bt_to_can/         BLE (NUS) <-> CAN bridge, 16-bit length-prefixed frames [HW-VERIFIED]
  bt_to_spi/         BLE (NUS) <-> SPI master request/response bridge     [HW-VERIFIED]
  bt_to_i2c/         BLE (NUS) <-> I2C master request/response bridge     [HW-VERIFIED]
  ble_nus_tester/    test harness: BLE central that drives the BT converters (NUS)
```

The Bluetooth converters use **BLE** (the ESP32-C3 has no Classic BT / SPP): they
expose the **Nordic UART Service (NUS)** as transport, so they work with nRF Connect,
Serial Bluetooth Terminal (BLE mode), or a custom app. Raw bytes for RS232/RS485;
2-byte length-prefixed frames for CAN/SPI/I2C (same framing as the WiFi converters).

Each example is a standalone ESP-IDF project that pulls in the shared components
via `EXTRA_COMPONENT_DIRS`.

## Building an example

```sh
# One-time per shell: load the IDF environment
. $HOME/esp/esp-idf/export.sh

cd examples/can_to_can
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The ESP32-C3 enumerates over its native USB-Serial-JTAG (typically `/dev/ttyACM*`),
so flashing and monitoring share one USB cable. Each example ships an
`sdkconfig.defaults` that routes the console (stdout + `ESP_LOG`) to that USB port
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) — the default would otherwise send logs to
the UART0 TTL header. The protocol links use UART1 / TWAI, so this does not clash.

### can_to_can
Flash the **same** firmware to two boards, join their `CANH`/`CANL` lines with a
120 Ω terminator at each end, and power both. Each node transmits a counter frame
every second at 500 kbit/s and logs frames received from the other.
