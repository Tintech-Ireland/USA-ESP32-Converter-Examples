# wifi_to_rs485

**Transparent WiFi (TCP) ↔ RS485** bridge — raw bytes both ways, no framing (the
byte-stream twin of `wifi_to_rs232`, on the half-duplex RS485 bus).

```
TCP bytes  ──►  RS485 TX (bus)
RS485 RX   ──►  TCP bytes
```

- **WiFi:** soft-AP `USA2-RS485` (pass `usa2rs485`), IP `192.168.4.1`
- **TCP:** server on port `3336`, one client at a time. Any plain client works:
  `nc 192.168.4.1 3336`
- **RS485:** UART1 via the on-board SP3485EN, **TX=GPIO9, RX=GPIO7**, 115200 8N1,
  half-duplex. DE = `RS485_TXEN` (GPIO10), **active-HIGH**; the driver's
  `UART_MODE_RS485_HALF_DUPLEX` raises it during TX and drops it otherwise (correct
  polarity for this board) and suppresses the local echo of our own transmission.

RS485 is a shared multi-drop bus; this bridge is transparent, so any addressing or
arbitration is up to the bytes you send.

## Why raw (no length prefix) here
Like RS232, RS485 is a byte stream with no message boundaries, so nothing needs
delimiting — bytes in = bytes out. (The length-prefix framing used by `wifi_to_can`
/ `wifi_to_spi` is only for the message/transaction backends.)

## Test rig
- This board = `wifi_to_rs485` (AP + TCP↔RS485).
- One or more other boards on the RS485 bus (e.g. `rs485_to_rs485`), wired
  A↔A / B↔B / GND, termination at the bus ends.
- A third board = WiFi station + TCP client (`wifi_tcp_tester`), or any laptop on
  the AP running `nc`. Bytes you send appear on the bus; bus traffic comes back.
