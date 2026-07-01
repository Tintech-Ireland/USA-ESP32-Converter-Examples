# bt_to_rs232

**Transparent Bluetooth LE ↔ RS232** bridge — raw bytes both ways, no framing (the
BLE twin of `wifi_to_rs232`).

The ESP32-C3 is **BLE-only** (no Classic Bluetooth / SPP), so the transport is a
GATT peripheral exposing the **Nordic UART Service (NUS)** — the de-facto "BLE
serial" profile. A BLE central connects and:

- **writes** to the RX characteristic → bytes go out **RS232 TX**
- **subscribes** to the TX characteristic → **RS232 RX** arrives as notifications

## Service (NUS, 128-bit UUIDs)
| Role | UUID | Properties |
|------|------|------------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write-No-Response (central → device) |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify (device → central) |

- **Advertises as** `USA2-BT-RS232` (name in the adv packet, service UUID in the scan response).
- **RS232:** UART1 via the on-board MAX3232, TX=GPIO5, RX=GPIO4, 115200 8N1.
- **MTU:** requests 247; UART→BLE notifications are chunked to the negotiated `ATT_MTU - 3`.
- No bonding/pairing required — just scan and connect.

## Testing without an app
Use **nRF Connect** or **Serial Bluetooth Terminal** (BLE mode) on a phone:
1. Scan, connect to `USA2-BT-RS232`.
2. Enable notifications on the TX characteristic.
3. Write bytes to the RX characteristic → they appear on RS232 TX (loop RS232
   TX→RX on the board to see them notified straight back).

Any BLE central works, including a custom app — NUS is a standard profile.

## Build (NimBLE host)
Bluetooth is enabled via `sdkconfig.defaults` (`CONFIG_BT_ENABLED`,
`CONFIG_BT_NIMBLE_ENABLED`). Build/flash as usual with `idf.py`.
