# modbus_rtu_slave

A **minimal Modbus RTU slave** (server) on RS485 — the wired-side peer for
`wifi_modbus_gateway`. It serves a small holding-register bank via the
[`modbus`](../../components/modbus) component's `Modbus::RtuSlave` transport (RTU
framing + CRC); this example supplies the register model and function-code handling.

- **Slave address:** `1`
- **RS485:** UART1 via the SP3485EN, **TX=GPIO9, RX=GPIO7, DE=GPIO10**, **9600 8N1**
  by default. The line settings **must match the master/gateway** (`kBaud` / `kParity`).

## Register map (16 × uint16, addresses 0–15)
| Address | Contents |
|---------|----------|
| 0 | uptime in seconds (live) |
| 1 | number of requests served |
| 2–15 | general read/write registers, pre-set to `0x1002`…`0x100F` |

## Supported function codes
| FC | Function | Notes |
|----|----------|-------|
| 0x03 | Read Holding Registers | |
| 0x04 | Read Input Registers | aliased to the same bank |
| 0x06 | Write Single Register | |
| 0x10 | Write Multiple Registers | |

Out-of-range or unsupported requests return the matching Modbus exception
(`0x01` illegal function, `0x02` illegal data address, `0x03` illegal data value).
Broadcast requests (address 0) are processed but not answered, per spec.

## End-to-end test with the gateway
1. Flash `wifi_modbus_gateway` to board A and `modbus_rtu_slave` to board B.
2. Wire them on the RS485 bus: `A_RS485 ↔ A_RS485`, `B_RS485 ↔ B_RS485`, `GND`
   (termination at the ends). Confirm both use the same baud/parity.
3. Join the `USA2-MODBUS` Wi-Fi AP and poll over Modbus TCP (unit id `1`):
   ```sh
   mbpoll -m tcp -a 1 -r 1 -c 4 -t 4 192.168.4.1     # read holding regs 0..3
   ```
   Register 0 should tick up each second (uptime), proving the full
   **TCP → gateway → RTU → slave → RTU → gateway → TCP** path.

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
