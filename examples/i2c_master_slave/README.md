# i2c_master_slave

I2C master ↔ slave demo across **two boards**. I2C is asymmetric, so it's two
firmwares:

| Project   | Flash to     | Role                                            |
|-----------|--------------|-------------------------------------------------|
| `master/` | board A      | writes a 4-byte counter to addr `0x42` @ 100 kHz |
| `slave/`  | board B      | receives at `0x42`, logs each counter           |

The board has no dedicated I2C pins, so the link uses the spare GPIO0/GPIO1 header
(`I2C_SDA`=GPIO0, `I2C_SCL`=GPIO1).

## Wiring
```
master SDA (GPIO0) ── slave SDA (GPIO0)
master SCL (GPIO1) ── slave SCL (GPIO1)
master GND         ── slave GND
```
I2C requires pull-ups. The master enables its internal (~45 kΩ) pull-ups, which is
fine for a short link at 100 kHz. For longer/faster buses fit external 4.7 kΩ
pull-ups to 3V3.

## Build & flash (from this directory)
```sh
. $HOME/esp/esp-idf/export.sh
idf.py -C master -p /dev/ttyACM0 flash      # master board
idf.py -C slave  -p /dev/ttyACM1 flash      # slave board
```
Then monitor each: the master logs `TX counter=N`, the slave logs `RX counter=N`.

## Notes / possible extensions
- One-way (master→slave write) for clarity. A read-back can be added with
  `i2c_master_transmit_receive` on the master and `i2c_slave_transmit` priming a
  response on the slave.
- The slave uses the default v1 driver: an ISR `on_recv_done` callback queues the
  value and a task re-arms the next receive. At 1 Hz there's no overlap; for
  back-to-back transfers, re-arm earlier / increase the queue depth.
