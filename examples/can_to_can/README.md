# can_to_can

**CAN (TWAI) peer-to-peer** loopback across two boards. The ESP32-C3's single TWAI
controller drives the on-board TJA1042 transceiver.

Flash the **same** firmware to two boards. Each node transmits a counter frame once
per second and logs every frame it receives from the other — demonstrating a working
bidirectional CAN link with bus ACK.

- **Bus:** 500 kbit/s, standard 11-bit identifiers (`id=0x100`, `dlc=8`), normal (ACK) mode.
- **Pins:** `CAN_TX=GPIO8`, `CAN_RX=GPIO6` (see `board_pins.h`).

## Wiring
```
board A CANH  <->  board B CANH
board A CANL  <->  board B CANL
board A GND   <->  board B GND
```
Fit a **120 Ω terminator across CANH–CANL at each end** of the bus (two resistors).
CAN needs both terminators — a single unterminated node shows TX errors instead of ACKs.

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # repeat for the second board
```
Each console shows its own `TX id=0x100 counter=N` and the peer's `RX id=0x100 … value=M`.
