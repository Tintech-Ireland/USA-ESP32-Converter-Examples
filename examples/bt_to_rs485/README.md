# bt_to_rs485

**Transparent Bluetooth LE ↔ RS485** bridge — raw bytes both ways, no framing (the
BLE twin of `wifi_to_rs485`, on the half-duplex RS485 bus).

BLE-only transport via the **Nordic UART Service (NUS)**. A BLE central connects and:

- **writes** to the RX characteristic → bytes go out on the **RS485 bus**
- **subscribes** to the TX characteristic → **RS485 bus** data arrives as notifications

## Service (NUS, 128-bit UUIDs)
| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write-No-Response (central → device) |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify (device → central) |

- **Advertises as** `USA2-BT-RS485`.
- **RS485:** UART1 via the on-board SP3485EN, TX=GPIO9, RX=GPIO7, 115200 8N1,
  half-duplex. DE = `RS485_TXEN` (GPIO10), active-HIGH; `UART_MODE_RS485_HALF_DUPLEX`
  auto-toggles it and suppresses the local TX echo.
- **MTU:** requests 247; UART→BLE notifications chunked to the negotiated `ATT_MTU - 3`.
- No bonding/pairing required — just scan and connect.

RS485 is a shared multi-drop bus; this bridge is transparent, so any addressing or
arbitration is up to the bytes you send.

## Testing
With **nRF Connect** / **Serial Bluetooth Terminal** (BLE mode): connect to
`USA2-BT-RS485`, enable TX notifications, and write bytes to the RX characteristic —
they go out on the bus; bus traffic comes back as notifications. Pair with another
board on the RS485 bus (e.g. `rs485_to_rs485`) to see live traffic.
