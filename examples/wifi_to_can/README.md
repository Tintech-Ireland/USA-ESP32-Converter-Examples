# wifi_to_can

**WiFi (TCP) ↔ CAN (TWAI)** bridge. CAN is message-oriented and TCP is a
boundary-less stream, so frames are delimited on the TCP side with a **2-byte
big-endian (network-order) length prefix**:

```
[ uint16 length, big-endian ][ payload : length bytes ]
```

…on every TCP send, and parsed off every TCP receive. The payload is one CAN frame:

```
length = 6 + dlc
  [0..3] identifier  uint32 big-endian   (11-bit std or 29-bit extended)
  [4]    flags        bit0 = extended id, bit1 = remote (RTR)
  [5]    dlc          0..8
  [6..]  data         dlc bytes
```

- **WiFi:** soft-AP `USA2-CAN` (pass `usa2can1`), IP `192.168.4.1`
- **TCP:** server on port `3334`, one client at a time
- **CAN:** TJA1042, TX=GPIO8, RX=GPIO6, 500 kbit/s, normal mode. **120 Ω termination
  at each end of the bus** is required.

Frames sent over TCP are transmitted on the bus; frames received from the bus are
sent (length-prefixed) to the TCP client.

## Why length-prefix instead of simprot here
Over TCP you already have reliable, ordered, checksummed delivery, so simprot's
CRC + ENQ/ACK would be redundant, and its stop-and-wait handshake would bottleneck
a busy CAN bus. A length prefix gives just the message framing TCP lacks, with no
per-frame round-trip. (For request/response converters, simprot over a
`ReaderWriter`/TCP backend remains a clean option.)

## Test rig
- This board = `wifi_to_can` (AP + TCP↔CAN).
- A second board on the CAN bus (e.g. `can_to_can`) + 120 Ω at each end.
- A third board = WiFi station + TCP client that speaks the framing above
  (extend `wifi_tcp_tester` to wrap/unwrap CAN payloads).
