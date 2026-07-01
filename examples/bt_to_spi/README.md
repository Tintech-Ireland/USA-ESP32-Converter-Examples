# bt_to_spi

**Bluetooth LE ↔ SPI (master)** bridge. BLE-only transport via the **Nordic UART
Service (NUS)**. SPI is master-driven and full-duplex, so like `wifi_to_spi` this is
**request/response** over the framed NUS byte stream:

```
central -> [ u16 len ][ MOSI bytes : len ]   (bytes to clock out, 1..64)
bridge  -> one SPI transaction of len bytes
bridge  -> [ u16 len ][ MISO bytes : len ]   (bytes clocked in; len == request len)
```

A full-duplex transfer reads as many bytes as it writes, so the reply length equals
the request length. The 2-byte big-endian length prefix delimits each message
(NUS writes / notifications can fragment).

## Service (NUS, 128-bit UUIDs)
| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write-No-Response |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- **Advertises as** `USA2-BT-SPI`.
- **SPI:** master on SPI2, 1 MHz, mode 0. Borrowed pins **MOSI=GPIO2, SCLK=GPIO5,
  MISO=GPIO7, CS=GPIO10**; `EN_5V` held low so the transceivers stay off the bus.
- **MTU:** requests 247; notifications chunked to `ATT_MTU - 3`. Max 64 B/transaction.

## Testing
With nRF Connect: connect to `USA2-BT-SPI`, subscribe to TX, and write
`[len][MOSI]` bytes to RX; the `[len][MISO]` reply arrives as a notification. Pair
with a second board running `spi_master_slave/slave` on the SPI bus.
