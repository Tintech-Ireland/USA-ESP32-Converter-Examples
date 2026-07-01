# USA2 ESP32-C3 — Communication Interface Examples & Converters

Example firmware and reusable components for the bespoke **ESP32-C3-WROOM-02** "USA2"
board. The board breaks the ESP32-C3 out to a full set of wired communication
interfaces — **RS232, RS485, CAN**, plus **I2C / SPI** on a spare header — and the
radio interfaces **Wi‑Fi** and **Bluetooth LE**.

This repo provides three families of firmware:

1. **Protocol loopbacks** — two boards exercising each wired interface against itself.
2. **Wi‑Fi converters** — a board bridges a TCP socket to a wired interface.
3. **Bluetooth (BLE) converters** — a board bridges a BLE link to a wired interface.

All examples are written in **C++17** and built with **ESP‑IDF v5.4** for target
`esp32c3`. Every example listed here has been verified end-to-end on hardware.

> The authoritative pin map is [`components/board/include/board_pins.h`](components/board/include/board_pins.h);
> all examples reference it rather than hard-coding GPIO numbers.

---

## Examples

### Shared components (`components/`)
| Component | Description |
|-----------|-------------|
| `board` | Header-only `board_pins.h` — the single source of truth for the board pinout. |
| `modbus` | Header-only Modbus **RTU** transport over RS485 — `Modbus::RtuMaster` (client) and `Modbus::RtuSlave` (server), CRC‑16/MODBUS. Used by `wifi_modbus_gateway` and `modbus_rtu_slave`. |

### Protocol loopbacks (`examples/`)
| Example | Interface | What it does |
|---------|-----------|--------------|
| `can_to_can` | CAN (TWAI) | Two boards on a CAN bus, 500 kbit/s; each sends a counter frame and logs the other's. |
| `rs232_to_rs232` | RS232 | Two boards, 3-wire cross; each sends a counter line and prints what it receives. |
| `rs485_to_rs485` | RS485 | Two+ boards, half-duplex, DE auto-toggled; each sends a counter line and prints received. |
| `i2c_master_slave` | I2C | `master/` + `slave/` firmwares; master writes a counter to slave `0x42` @100 kHz. |
| `spi_master_slave` | SPI | `master/` + `slave/` firmwares; full-duplex counter exchange @1 MHz, mode 0. |

### Wi‑Fi converters (`examples/`)
Each comes up as a Wi‑Fi **soft‑AP** with a TCP server on `192.168.4.1`.
| Example | Bridges | TCP port | Framing |
|---------|---------|----------|---------|
| `wifi_to_rs232` | TCP ↔ RS232 | 3333 | raw bytes |
| `wifi_to_rs485` | TCP ↔ RS485 (half-duplex) | 3336 | raw bytes |
| `wifi_to_can` | TCP ↔ CAN | 3334 | 2-byte length-prefixed frames |
| `wifi_to_spi` | TCP ↔ SPI master | 3335 | length-prefixed request/response |
| `wifi_to_i2c` | TCP ↔ I2C master | 3337 | length-prefixed request/response |

### Modbus (`examples/`)
| Example | Role | Interface |
|---------|------|-----------|
| `wifi_modbus_gateway` | Modbus **TCP** ↔ Modbus **RTU** gateway (MBAP ↔ address + CRC‑16) | Wi‑Fi TCP :502 ↔ RS485 |
| `modbus_rtu_slave` | Modbus **RTU slave** (test peer) — 16-register bank, FC 0x03/0x04/0x06/0x10 | RS485, addr 1 |
| `modbus_tcp_client` | Modbus **TCP client** (WiFi station) — polls/writes the gateway; on-board `pymodbus` equivalent | Wi‑Fi TCP :502 |

Unlike the transparent `wifi_to_rs485` bridge, the gateway speaks Modbus on both
sides: a Wi‑Fi Modbus TCP server translates each request to a Modbus RTU transaction
on the bus using the `modbus` component, so any Modbus TCP client can poll RTU slaves
over the air. Flash the gateway to one board and `modbus_rtu_slave` to another on the
same RS485 bus to exercise the whole path end to end.

### Bluetooth LE converters (`examples/`)
The ESP32‑C3 is **BLE‑only** (no Classic Bluetooth / SPP). Each converter is a GATT
peripheral exposing the **Nordic UART Service (NUS)** as transport, so it works with
nRF Connect, Serial Bluetooth Terminal (BLE mode), or a custom app. The bridge logic
and wire framing are identical to the Wi‑Fi converters.
| Example | Bridges | Advertises as | Framing |
|---------|---------|---------------|---------|
| `bt_to_rs232` | BLE ↔ RS232 | `USA2-BT-RS232` | raw bytes |
| `bt_to_rs485` | BLE ↔ RS485 (half-duplex) | `USA2-BT-RS485` | raw bytes |
| `bt_to_can` | BLE ↔ CAN | `USA2-BT-CAN` | 2-byte length-prefixed frames |
| `bt_to_spi` | BLE ↔ SPI master | `USA2-BT-SPI` | length-prefixed request/response |
| `bt_to_i2c` | BLE ↔ I2C master | `USA2-BT-I2C` | length-prefixed request/response |

### Test harnesses (`examples/`)
| Example | Role |
|---------|------|
| `wifi_tcp_tester` | Wi‑Fi **station** + TCP client that drives the Wi‑Fi converters. |
| `ble_nus_tester` | BLE **central** that scans/connects/subscribes and drives the BLE converters. |

---

## Interfaces supported

- **RS232** — via an on-board MAX3232 charge-pump level shifter. Standard 2-wire
  asynchronous serial (TX/RX), 3.3 V logic on the MCU side, ±RS232 levels on the
  connector. Driven by an ESP32 UART peripheral.
- **RS485** — via an SP3485EN half-duplex transceiver (differential A/B pair). The
  driver-enable line (`RS485_TXEN`) is **active-high**; the ESP‑IDF UART RS485
  half-duplex mode toggles it automatically around each transmission. The receiver
  is always enabled in hardware. Fit termination at the two physical bus ends.
- **CAN** — via a TJA1042 transceiver, driven by the ESP‑IDF **TWAI** peripheral
  (500 kbit/s in the examples). Requires 120 Ω termination at each end of the bus.
- **I2C** — the ESP‑IDF v5.x bus-based master/slave drivers, 100 kHz, 7-bit
  addressing. Runs on the spare GPIO header with internal pull-ups (fit external
  4.7 kΩ pull-ups for longer/faster links).
- **SPI** — SPI2 (GPSPI2) master/slave, 1 MHz, mode 0, full-duplex.
- **Wi‑Fi** — soft‑AP + lwIP TCP server (converters) or station + TCP client (tester).
- **Bluetooth LE** — NimBLE host exposing the Nordic UART Service (NUS): an RX
  characteristic (central → device, Write) and a TX characteristic (device →
  central, Notify).
- **Modbus** — a Modbus **RTU master** transport (`components/modbus`) over RS485
  (CRC‑16/MODBUS, RTU inter-frame timing), plus a Modbus **TCP↔RTU gateway** example
  (`wifi_modbus_gateway`).
- **UART0 / USB** — UART0 is the TTL console header; the native USB‑Serial‑JTAG is
  used for flashing and the log console.

---

## Recommended pinout (ESP32‑C3, from `board_pins.h`)

| Interface | Signal | GPIO | Notes |
|-----------|--------|------|-------|
| RS232 | TX (→ MAX3232 T1IN) | **GPIO5** | |
| RS232 | RX (← MAX3232 R1OUT) | **GPIO4** | |
| RS485 | TX / DI | **GPIO9** | strapping pin; also the BOOT button — don't let the bus hold it low at reset |
| RS485 | RX / RO | **GPIO7** | |
| RS485 | DE (`RS485_TXEN`) | **GPIO10** | **active-high** driver enable |
| CAN | TX (→ TJA1042 TXD) | **GPIO8** | strapping pin |
| CAN | RX (← TJA1042 RXD) | **GPIO6** | |
| I2C | SDA | **GPIO0** | spare header; on-board 45 kΩ pull-up |
| I2C | SCL | **GPIO1** | spare header |
| SPI | MOSI | **GPIO2** | spare header (also a strapping pin) |
| SPI | SCLK | **GPIO5** | shared with RS232_TX (a transceiver input) |
| SPI | MISO | **GPIO7** | shared with RS485_RX (an SP3485 output — safe only while `EN_5V` is LOW) |
| SPI | CS | **GPIO10** | shared with RS485_TXEN |
| Power | `EN_5V` (5 V boost enable) | **GPIO3** | HIGH powers the transceiver rail; drive LOW to free the borrowed SPI pins |
| Console | UART0 TX / RX | GPIO21 / GPIO20 | TTL header |
| USB | D+ / D− | GPIO19 / GPIO18 | native USB‑Serial‑JTAG |

Notes:
- The board has **no dedicated I2C/SPI pins**. I2C uses two spare-header GPIOs; SPI
  needs four signals, so three are **borrowed** from transceiver nets. The SPI
  examples hold **`EN_5V` LOW** so the transceivers are powered down and off the bus.
- **Strapping pins:** GPIO2, GPIO8, GPIO9 are ESP32‑C3 strapping pins — avoid driving
  them during reset. GPIO9 also carries the BOOT button.
- Only GPIO0/1/2 are uncommitted, and they appear on a do-not-populate test header
  that must be hand-populated.

---

## Development environment

- **ESP‑IDF v5.4**, target **`esp32c3`**, language standard **C++17**.

### Install ESP‑IDF (one time)
```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32c3          # downloads the RISC-V toolchain + tools
```

### Activate the environment (once per shell)
```sh
. $HOME/esp/esp-idf/export.sh
```

### ESP‑IDF settings the examples rely on
Each project ships an `sdkconfig.defaults`; `idf.py` applies it on first configure.

- **All examples** route the console to the native USB‑Serial‑JTAG so flashing and
  logging share one cable:
  ```
  CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
  ```
  (Protocol links use UART1 / TWAI / SPI / I2C, so the console never clashes.)
- **Bluetooth examples** additionally enable the NimBLE host and a larger app
  partition:
  ```
  CONFIG_BT_ENABLED=y
  CONFIG_BT_NIMBLE_ENABLED=y
  CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
  ```
  `ble_nus_tester` also enables the central/observer roles
  (`CONFIG_BT_NIMBLE_ROLE_CENTRAL`, `CONFIG_BT_NIMBLE_ROLE_OBSERVER`).
- **C++17** is set per component with `target_compile_features(... cxx_std_17)`.
- **Wi‑Fi examples** cap the radio TX power (`esp_wifi_set_max_tx_power(34)`, ~8.5 dBm)
  to avoid a brownout on USB power; for full-power Wi‑Fi, power the board from its
  5–24 V `Vin` input instead of USB.

---

## Building & flashing

Each example is a **standalone ESP‑IDF project** that pulls in the shared components
via `EXTRA_COMPONENT_DIRS`. General flow:

```sh
. $HOME/esp/esp-idf/export.sh          # once per shell
cd examples/<example>                  # e.g. examples/wifi_to_rs232
idf.py set-target esp32c3              # once per project
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # exit monitor with Ctrl-]
```

For the split examples (`i2c_master_slave`, `spi_master_slave`), build/flash each
half with `-C`:
```sh
idf.py -C master -p /dev/ttyACM0 flash
idf.py -C slave  -p /dev/ttyACM1 flash
```

The ESP32‑C3 enumerates over native USB‑Serial‑JTAG, typically `/dev/ttyACM*` on
Linux. Per-example wiring and protocol details are in each example's `README.md`.

---

## Devices needed to test

The full test bench uses **three ESP32‑C3 (USA2) boards** on the same host over USB:

| Role | Runs | Purpose |
|------|------|---------|
| **Wireless board** | a converter's soft‑AP / peripheral, **or** a tester | Ideally a board with no header pins soldered — used purely for the radio side. |
| **Bridge board** | the converter under test (e.g. `wifi_to_spi`, `bt_to_can`) | Has the wired interface connected. |
| **Peer board** | the matching loopback/slave (e.g. `spi_master_slave/slave`, `can_to_can`) | The wired-side partner on the bus. |

Typical verification rig for a converter:

1. Flash the **converter** to the bridge board and wire its interface to the peer.
2. Flash the matching **loopback/slave** to the peer board on the same bus.
3. Flash a **tester** to the wireless board:
   - `wifi_tcp_tester` (Wi‑Fi station + TCP client) for the Wi‑Fi converters, or
   - `ble_nus_tester` (BLE central) for the Bluetooth converters, or a phone app
     (nRF Connect / Serial Bluetooth Terminal in BLE mode).

The testers default to sending a raw `hello #N`. For the message-framed converters
(CAN / SPI / I2C) the tester's payload/decoder is adjusted to that interface's
`[u16 length][payload]` framing (see the converter's `README.md`).

Minimal setups:
- A single **loopback** example needs **two** boards + the interface wiring
  (CAN/RS485 also need termination; I2C/SPI use the spare header).
- A **BLE converter** can also be exercised with just the bridge board + a **phone**
  running a BLE terminal app — no third board required.

---

## Framing (network/BLE ↔ wired)

- **Byte-stream interfaces (RS232, RS485):** transparent **raw** bridge — bytes in =
  bytes out, no framing added.
- **Message/transaction interfaces (CAN, SPI, I2C):** each message is delimited on
  the network/BLE side with a **2-byte big-endian length prefix**, `[u16 len][payload]`.
  This is needed because both TCP and BLE deliver a boundary-less/fragmentable byte
  stream. The same framing is used over Wi‑Fi (TCP) and Bluetooth (NUS), so a client
  speaks one protocol regardless of transport:
  - **CAN payload:** `[id u32 BE][flags: bit0=extended, bit1=RTR][dlc][data…]`
  - **SPI:** request `[MOSI bytes]` → reply `[MISO bytes]` (equal length; full-duplex).
  - **I2C:** request `[addr][wlen][rlen][write bytes]` → reply `[status][read bytes]`
    (supports write / read / write-then-read).
