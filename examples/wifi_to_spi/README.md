# wifi_to_spi

**WiFi (TCP) ↔ SPI (master)** bridge. SPI is a master-driven, full-duplex
transaction protocol — the master clocks *N* bytes out on MOSI while simultaneously
shifting *N* bytes in on MISO, and there is no unsolicited slave→master traffic — so
this bridge is **request/response**:

```
client -> [ uint16 len, big-endian ][ MOSI bytes : len ]   (bytes to clock out)
bridge -> one SPI transaction of len bytes
bridge -> [ uint16 len, big-endian ][ MISO bytes : len ]   (bytes clocked in)
```

The 2-byte big-endian length prefix delimits each message on the TCP side, exactly
as in `wifi_to_can`. The reply length always equals the request length, because a
full-duplex SPI transfer reads as many bytes as it writes.

- **WiFi:** soft-AP `USA2-SPI` (pass `usa2spi1`), IP `192.168.4.1`
- **TCP:** server on port `3335`, one client at a time
- **SPI:** master on bus SPI2 (GPSPI2), 1 MHz, mode 0. Pins borrowed from the spare
  header + transceiver-input nets: **MOSI=GPIO2, SCLK=GPIO5, MISO=GPIO7, CS=GPIO10**.
  `EN_5V` (GPIO3) is held **LOW** so the transceivers stay off and clear of the bus.
- Max single transaction: **64 bytes**.

## Why length-prefix instead of simprot here
Over TCP you already have reliable, ordered, checksummed delivery, so simprot's CRC
+ ENQ/ACK would be redundant. A length prefix gives just the message framing TCP
lacks. The request/response shape maps directly onto SPI's master-driven,
one-transaction-in-per-transaction-out nature.

## Test rig
- This board = `wifi_to_spi` (AP + TCP↔SPI master).
- A second board = SPI **slave** (e.g. `spi_master_slave/slave`), wired pin-for-pin
  (MOSI↔MOSI, SCLK↔SCLK, MISO↔MISO, CS↔CS, GND↔GND).
- A third board = WiFi station + TCP client that speaks the framing above (extend
  `wifi_tcp_tester` to send `[len][MOSI]` and read back `[len][MISO]`).
