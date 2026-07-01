# wifi_modbus_gateway

**Modbus TCP ↔ Modbus RTU (RS485)** gateway. The board is a Wi-Fi soft-AP running a
**Modbus TCP server on port 502**; each TCP request is translated to a **Modbus RTU**
transaction on the RS485 bus (via the [`modbus`](../../components/modbus) component)
and the RTU response is wrapped back into Modbus TCP.

Any Modbus TCP client — `pymodbus`, `mbpoll`, a SCADA/HMI — can reach the RTU slaves
on the wired bus over Wi-Fi.

```
Modbus TCP client ──WiFi/TCP:502──▶ [gateway] ──RS485 (Modbus RTU)──▶ RTU slave(s)
```

## Protocol translation
Modbus TCP frame = **MBAP header (7 bytes)** + PDU:

| Bytes | Field | Handling |
|-------|-------|----------|
| 0–1 | transaction id | echoed back unchanged |
| 2–3 | protocol id (0) | validated |
| 4–5 | length | bytes following = unit id + PDU |
| 6 | unit id | used as the **RTU slave address** |
| 7… | PDU (function + data) | forwarded to RTU |

On the RTU side the component adds the slave address and a **CRC-16/MODBUS**, drives
the frame on RS485 (half-duplex, DE auto-toggled), reassembles the reply by RTU
inter-frame timing, checks the CRC, and returns the response PDU under the original
MBAP. If the slave does not answer (timeout / CRC error) the gateway returns a Modbus
exception `function|0x80` with code **0x0B** ("gateway target device failed to respond").

## Configuration
- **Wi-Fi:** soft-AP `USA2-MODBUS` (pass `usa2mbus1`), IP `192.168.4.1`, TCP port `502`.
- **RS485:** UART1 via the SP3485EN, **TX=GPIO9, RX=GPIO7, DE=GPIO10** (active-high).
- **Line format:** defaults to **9600 8N1** (`kBaud` / `kParity` at the top of
  `main.cpp`). The Modbus spec default is 19200 8E1 — set these to match your bus.
- **Timeout:** 1000 ms per RTU transaction.

## Quick test with pymodbus
Join the `USA2-MODBUS` AP, then from a host on that network:
```python
from pymodbus.client import ModbusTcpClient
c = ModbusTcpClient("192.168.4.1", port=502)
c.connect()
print(c.read_holding_registers(address=0, count=4, slave=1))   # unit id 1
```
Or with `mbpoll`:
```sh
mbpoll -m tcp -a 1 -r 1 -c 4 -t 4 192.168.4.1
```

## Notes
- The `modbus` component is a **master/client** RTU transport (`Modbus::RtuMaster`) —
  reusable on its own for a straight RTU master. Not yet hardware-verified against a
  real RTU slave; wire the gateway board's RS485 to a Modbus RTU device (or a second
  board acting as an RTU slave) and poll it over TCP to verify.
- Modbus TCP is not authenticated; run this on a trusted/isolated network.
