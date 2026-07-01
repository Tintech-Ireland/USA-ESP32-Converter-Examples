# wifi_tcp_tester

**Test harness** (not a shipped converter) — the Wi-Fi counterpart of
`ble_nus_tester`. Runs on a spare board as a Wi-Fi **station**: joins a converter's
soft-AP, opens a TCP connection to its server, sends a test payload once per second,
and logs everything it receives back.

## Default behaviour
Out of the box it targets `wifi_to_rs232` and sends raw `hello #N`:
```cpp
constexpr char     AP_SSID[] = "USA2-RS232";
constexpr char     AP_PASS[] = "usa2rs232";
constexpr char     SRV_IP[]  = "192.168.4.1";
constexpr uint16_t SRV_PORT  = 3333;
```
With a wired loopback on the bridge (RS232 `TX_RS232`→`RX_RS232`), each `hello #N`
travels Wi-Fi → TCP → UART TX → loopback → UART RX → TCP → Wi-Fi and echoes straight
back — proving the whole path end to end.

## Targeting another converter
Edit the constants above to the converter's SSID / password / port:

| Converter | SSID | Pass | Port |
|-----------|------|------|------|
| `wifi_to_rs232` | `USA2-RS232` | `usa2rs232` | 3333 |
| `wifi_to_can` | `USA2-CAN` | `usa2can1` | 3334 |
| `wifi_to_spi` | `USA2-SPI` | `usa2spi1` | 3335 |
| `wifi_to_rs485` | `USA2-RS485` | `usa2rs485` | 3336 |
| `wifi_to_i2c` | `USA2-I2C` | `usa2i2c1` | 3337 |

For the **raw** converters (RS232/RS485) the default `hello #N` payload works as-is.
For the **framed** converters (CAN/SPI/I2C), also adjust the send/decode logic in
`client_task()` to emit the `[u16 len][payload]` framing that converter expects (see
its README). This is the same per-converter retargeting the project used to verify
each Wi-Fi bridge.

## Build & flash
```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
