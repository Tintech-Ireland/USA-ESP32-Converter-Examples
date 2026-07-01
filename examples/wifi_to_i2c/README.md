# wifi_to_i2c

**WiFi (TCP) ↔ I2C (master)** bridge. I2C is master-driven and transactional, so
like `wifi_to_spi` this bridge is **request/response** — each length-prefixed TCP
frame is one I2C transaction the board runs as bus master:

```
client -> [ u16 len ][ addr | wlen | rlen | write bytes(wlen) ]
bridge -> one I2C transaction (write / read / write-then-read)
bridge -> [ u16 len ][ status | read bytes(rlen, only if status==0) ]
```

- `addr`   — 7-bit device address
- `wlen`   — bytes to write (0..32), `rlen` — bytes to read (0..32)
- `status` — `0` = OK, non-zero = failed (e.g. address NACKed); `0xFE` = malformed request

The transaction shape follows from `wlen`/`rlen`:

| wlen | rlen | operation                                   |
|------|------|---------------------------------------------|
| >0   | >0   | write-then-read (repeated START, e.g. reg read) |
| >0   | 0    | write only                                  |
| 0    | >0   | read only                                   |

- **WiFi:** soft-AP `USA2-I2C` (pass `usa2i2c1`), IP `192.168.4.1`
- **TCP:** server on port `3337`, one client at a time
- **I2C:** master on the spare header, **SDA=GPIO0, SCL=GPIO1**, 100 kHz, internal
  pull-ups (adequate for a short bench link; fit external 4.7 kΩ for longer/faster).
  Uses the ESP-IDF v5.x bus-based master driver; a device handle is created per
  address on demand and cached.

## Test rig
- This board = `wifi_to_i2c` (AP + TCP↔I2C master).
- A second board = I2C **slave** (`i2c_master_slave/slave`, addr `0x42`), wired
  SDA↔SDA / SCL↔SCL / GND. That slave is receive-only, so it exercises the **write**
  path: a `write` transaction to `0x42` shows up on its console as `RX counter=N`.
- A third board = WiFi station + TCP client (`wifi_tcp_tester`) that speaks the
  framing above, or any laptop on the AP.
