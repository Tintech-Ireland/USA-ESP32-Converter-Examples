# ble_nus_tester

**Test harness** (not a shipped converter) — the BLE equivalent of `wifi_tcp_tester`.
Runs on the spare ESP32-C3 as a **BLE central**: scans for a peripheral advertising
the name prefix `USA2-BT`, connects, discovers the **Nordic UART Service (NUS)**,
subscribes to the TX characteristic, and writes `hello #N` to RX once per second,
logging everything it is notified.

## Use
1. Flash a converter (e.g. `bt_to_rs232`) to a board with the wired side connected.
2. Flash `ble_nus_tester` to the spare board.
3. Watch both consoles. On the tester you should see: `scanning` → `found
   peripheral` → `connected` → `found NUS` → `subscribed`, then `TX 'hello #N'`.

**Raw converters** (`bt_to_rs232`, `bt_to_rs485`): put a loopback on the wired side
(e.g. RS232 TX→RX jumper, or a second board on the RS485 bus that echoes) and each
`hello #N` comes back as `RX notify`.

**Framed converters** (`bt_to_can`, `bt_to_spi`, `bt_to_i2c`): edit
`send_test_payload()` to emit the `[u16 len][payload]` framing that converter expects
(see its README) — exactly how `wifi_tcp_tester` was retargeted per WiFi converter.

## Notes
- Active scan (to receive the scan response carrying the device name) and match by
  name prefix. Requests ATT MTU 247.
- Discovery chain: service by UUID → all characteristics (find RX/TX value handles)
  → TX descriptors (find the 0x2902 CCCD) → write `0x0001` to subscribe.
- NimBLE central role (`CONFIG_BT_NIMBLE_ROLE_CENTRAL`).
