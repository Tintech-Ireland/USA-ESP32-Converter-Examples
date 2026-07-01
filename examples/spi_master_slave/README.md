# spi_master_slave

SPI master ↔ slave demo across **two boards** (SPI is asymmetric → two firmwares):

| Project   | Flash to | Role                                                    |
|-----------|----------|---------------------------------------------------------|
| `master/` | board A  | clocks a 4-byte counter out (MOSI) @1 MHz, mode 0       |
| `slave/`  | board B  | pre-loads its own counter on MISO, logs master's counter |

## Pins (see board_pins.h)
The board has no dedicated SPI pins and SPI needs four signals, so three are
borrowed from transceiver nets. Both firmwares hold `5V_EN` (GPIO3) **low** to keep
the transceivers powered down, so the borrowed pins stay free of contention. Two of
them (SCLK, CS) are transceiver *inputs* (never driven by the chip); MISO shares
`RS485_RX` (SP3485 RO, an **output**), so it is contention-free only because the
SP3485 is held unpowered — that is why keeping `5V_EN` low is mandatory here.

| Signal | GPIO | Shares                              |
|--------|------|-------------------------------------|
| MOSI   | 2    | spare pin (also a strapping pin)    |
| SCLK   | 5    | RS232_TX (MAX3232 T1IN, input)      |
| MISO   | 7    | RS485_RX (SP3485 RO, **output**)    |
| CS     | 10   | RS485_TXEN (SP3485 DE, input)       |

## Wiring
Same signal to same signal between the two boards, plus a common ground:
```
master MOSI(GPIO2)  ── slave MOSI(GPIO2)
master SCLK(GPIO5)  ── slave SCLK(GPIO5)
master MISO(GPIO7)  ── slave MISO(GPIO7)
master CS  (GPIO10) ── slave CS  (GPIO10)
master GND          ── slave GND
```

## Build & flash (from this directory)
```sh
. $HOME/esp/esp-idf/export.sh
idf.py -C master -p /dev/ttyACM0 flash
idf.py -C slave  -p /dev/ttyACM1 flash
```
Master logs `TX counter=N  RX(from slave)=M`; slave logs `RX counter=N (sent back M)`.
The slave's returned value lags the master by one transaction — normal for an SPI
slave that pre-loads its response before the master clocks it.

## Notes
- GPIO2 (MOSI) is a strapping pin; if a board occasionally misboots, add a pull-up
  or move MOSI to another spare pin (one-line change in board_pins.h).
- 1 MHz keeps the jumper-wire link comfortable; raise `kClockHz` for a real PCB.
