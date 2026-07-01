# rs485_to_rs485

**RS485 half-duplex peer-to-peer** loopback. Drives UART1 through the on-board
SP3485EN transceiver on the differential A/B pair.

Flash the **same** firmware to two (or more) boards on one bus. Each node sends a
counter line once per second and prints what it receives.

- **Framing:** 115200 baud, 8N1.
- **Pins:** `RS485_TX=GPIO9`, `RS485_RX=GPIO7`, `RS485_DE=GPIO10` (see `board_pins.h`).

## Direction control
The SP3485's `~RE` is hard-wired enabled (receiver always on); the driver enable
`DE` is driven by `RS485_TXEN` (GPIO10) and is **active-HIGH**. The firmware uses
`UART_MODE_RS485_HALF_DUPLEX`, so the UART driver raises DE during transmit and drops
it otherwise — the correct polarity for this board — and suppresses the local echo of
its own transmission. No manual GPIO toggling.

## Wiring
```
all A_RS485  <->  A_RS485
all B_RS485  <->  B_RS485
all GND      <->  GND
```
The board carries on-board termination (~100 Ω each; ~50 Ω with two boards — a healthy
load). For a longer multi-drop bus, terminate at the two physical ends only.

> **Note:** `RS485_TX`=GPIO9 is an ESP32-C3 strapping pin and also the BOOT button —
> keep the bus from holding it low at reset.

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # repeat for the second board
```
Each console shows its own `TX` counter and the peer's line as `RX`.
