# serial_selftest

Build/smoke test for the **`serial_transport`** component — the "SimpleProtocol"
(simprot) framing layer: STX/ETX framing + byte-stuffing + **CRC-16/MODBUS** + an
**ENQ/ACK** handshake, over an ESP-IDF UART backend (`UartByteSerial`).

It brings up UART1 on the RS232 pins, wraps it in a `SerialPort::UartByteSerial`, and
runs a `SimpleProtocol::SerialProtocol` over it. Purpose: prove the component compiles,
links, and runs on-target, and serve as a usage template.

- **Link:** UART1 via the MAX3232, `RS232_TX=GPIO5` / `RS232_RX=GPIO4`, 115200 8N1.
- **Component:** requires `serial_transport` (see `components/serial_transport`).

## Master / slave role
The role is a compile-time constant in `main/main.cpp`:
```cpp
constexpr bool kMaster = true;   // flash the second board with kMaster=false
```
- **Master** sends a framed `hello #N` each second and logs when it is ACKed.
- **Slave** receives and prints the framed payloads.
- Run on its own (no peer), a master just reports `receive()`/send timeouts — which
  still exercises the whole framing + CRC + handshake stack.

## Wiring (two boards)
RS232 3-wire cross, as in `rs232_to_rs232`:
```
board A TX_RS232 -> board B RX_RS232,   board A RX_RS232 <- board B TX_RS232,   GND common
```

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # master board (kMaster=true)
# edit kMaster=false, rebuild, flash the second board as the slave
```
