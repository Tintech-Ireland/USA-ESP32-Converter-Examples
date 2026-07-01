# modbus_tcp_client

A self-contained **Modbus TCP client** (WiFi station) for `wifi_modbus_gateway` — the
ESP32-only equivalent of running `pymodbus`/`mbpoll` from a PC. It joins the gateway's
soft-AP, connects to the Modbus TCP server, and polls the RTU slave behind the gateway.

```
[modbus_tcp_client] ──WiFi/TCP:502──▶ [wifi_modbus_gateway] ──RS485──▶ [modbus_rtu_slave]
```

So the whole gateway ↔ RTU-slave demo runs on **three boards with no PC**.

## What it does
Once connected (to `192.168.4.1:502`, unit id `1`):
- **Reads** holding registers 0..3 (FC 0x03) once per second and logs them —
  against `modbus_rtu_slave` you'll see `[0]` (uptime) and `[1]` (request count) tick.
- Every 5th poll **writes** register 2 (FC 0x06) with an incrementing value — which
  then appears in the following read, demonstrating the write path.

Modbus exceptions are logged with their exception code.

## Configuration
Constants at the top of `main.cpp`: `AP_SSID`/`AP_PASS` (match the gateway), `SRV_IP`,
`SRV_PORT` (502), `UNIT_ID` (1).

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Full three-board demo
1. `wifi_modbus_gateway` → board A (RS485 master + AP)
2. `modbus_rtu_slave` → board B, wired to A on the RS485 bus (A/B/GND, terminated)
3. `modbus_tcp_client` → board C (this)

Board C's console shows the slave's live registers read back over the air. Any
standard Modbus TCP client works against the gateway too — this is just the
board-only version.
