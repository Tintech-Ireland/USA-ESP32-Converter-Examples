# rs232_to_rs232

**RS232 peer-to-peer** loopback across two boards. Drives UART1 through the on-board
MAX3232 level shifter, which presents true RS232 levels on connector J1.

Flash the **same** firmware to two boards. Each node sends a counter line once per
second and prints whatever it receives.

- **Framing:** 115200 baud, 8N1.
- **Pins:** `RS232_TX=GPIO5`, `RS232_RX=GPIO4` (see `board_pins.h`).

## Wiring
A standard 3-wire null-modem cross on the RS232 (J1) side:
```
board A TX_RS232  ->  board B RX_RS232
board A RX_RS232  <-  board B TX_RS232
board A GND       <->  board B GND
```

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # repeat for the second board
```
Each console shows its own `TX` counter and the peer's line as `RX`.
