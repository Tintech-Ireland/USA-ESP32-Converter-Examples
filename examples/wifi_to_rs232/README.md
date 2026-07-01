# wifi_to_rs232

Transparent **WiFi (TCP) ↔ RS232** bridge. Bytes are passed verbatim in both
directions — no framing — so any plain TCP client can drive the RS232 port.

- **WiFi:** soft-AP (the board is its own access point)
  - SSID `USA2-RS232`, password `usa2rs232` (WPA2), IP `192.168.4.1`
- **TCP:** server on port `3333`, one client at a time (TCP_NODELAY for prompt keystrokes)
- **RS232:** UART1 via the MAX3232 (TX=GPIO5, RX=GPIO4), 115200 8N1

## Use
```sh
# 1. flash
idf.py -p /dev/ttyACM0 flash monitor
# 2. on your laptop: join WiFi "USA2-RS232" (pass usa2rs232)
# 3. open a raw TCP connection and type:
nc 192.168.4.1 3333
```
Whatever you send goes out RS232 `TX_RS232`; whatever arrives on `RX_RS232`
comes back to the TCP client.

### Quick loopback test (one board)
Jumper `TX_RS232 → RX_RS232` on the board. Then everything you type into
`nc 192.168.4.1 3333` echoes straight back — proving the full TCP→UART→TCP path.

## Notes / tuning
- **Soft-AP vs station:** to join an existing network instead, replace
  `wifi_init_softap()` with a STA init and use the DHCP-assigned IP; the TCP
  server and bridge are unchanged.
- Credentials, port, and UART params are constants at the top of `main.cpp`.
- Raw bridge by design. A framed variant (adding a CRC + ACK handshake) is
  unnecessary over TCP, which is already reliable and ordered; this bridge stays
  transparent.
