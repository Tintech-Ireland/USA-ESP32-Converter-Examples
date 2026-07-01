# bt_to_can

**Bluetooth LE ↔ CAN (TWAI)** bridge. BLE-only transport via the **Nordic UART
Service (NUS)**. CAN is message-oriented and NUS carries an unframed, fragmentable
byte stream, so each message is delimited with a **2-byte big-endian length prefix**
(identical to `wifi_to_can`):

```
[ u16 length ][ payload : length bytes ]        length = 6 + dlc
  payload:  [0..3] id (u32 BE)  [4] flags(bit0=extd,bit1=rtr)  [5] dlc  [6..] data
```

- A central **writes** framed CAN frames to the RX characteristic → transmitted on the bus.
- A central **subscribes** to the TX characteristic → receives framed bus frames as notifications.

## Service (NUS, 128-bit UUIDs)
| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write-No-Response |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

- **Advertises as** `USA2-BT-CAN`.
- **CAN:** TJA1042, TX=GPIO8, RX=GPIO6, 500 kbit/s, normal mode. **120 Ω termination
  at each bus end** required.
- **MTU:** requests 247; notifications chunked to `ATT_MTU - 3`. Outbound frames are
  mutex-serialized so a bus frame and a reply never interleave on the byte stream.

## Notes
- BLE RX bytes are handed to a stream buffer so the BLE host callback stays lean; a
  task reassembles frames and calls `twai_transmit`. A second task forwards received
  bus frames back as notifications.
- Same framing as `wifi_to_can`, so a client that speaks that framing works over
  either transport. Test with nRF Connect (write/subscribe raw bytes) plus a second
  board on the CAN bus (e.g. `can_to_can`).
