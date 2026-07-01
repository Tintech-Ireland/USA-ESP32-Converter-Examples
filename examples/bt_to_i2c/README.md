# bt_to_i2c

**Bluetooth LE ↔ I2C (master)** bridge. BLE-only transport via the **Nordic UART
Service (NUS)**. I2C is master-driven and transactional, so like `wifi_to_i2c` this
is **request/response** over the framed NUS byte stream:

```
central -> [ u16 len ][ addr | wlen | rlen | write bytes(wlen) ]
bridge  -> one I2C transaction (write / read / write-then-read)
bridge  -> [ u16 len ][ status | read bytes(rlen, only if status==0) ]
```

- `addr` — 7-bit device address
- `wlen` — bytes to write (0..32), `rlen` — bytes to read (0..32)
- `status` — `0` = OK, non-zero = failed (e.g. address NACKed); `0xFE` = malformed request

| wlen | rlen | operation                                   |
|------|------|---------------------------------------------|
| >0   | >0   | write-then-read (repeated START, e.g. reg read) |
| >0   | 0    | write only                                  |
| 0    | >0   | read only                                   |

## Service (NUS, 128-bit UUIDs)
| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write-No-Response |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- **Advertises as** `USA2-BT-I2C`.
- **I2C:** master on the spare header, **SDA=GPIO0, SCL=GPIO1**, 100 kHz, internal
  pull-ups. ESP-IDF v5.x bus-based driver; a device handle is created per address on
  demand and cached.
- **MTU:** requests 247; notifications chunked to `ATT_MTU - 3`.

## Testing
With nRF Connect: connect to `USA2-BT-I2C`, subscribe to TX, and write a framed
request to RX. A `write` to `0x42` (`[00 07][42 04 00 <4 bytes>]`) shows up on a
second board running `i2c_master_slave/slave` (RX-only) as `RX counter=N`, and the
reply notification carries `status=0`.
