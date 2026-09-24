# Review findings — to fix and verify on hardware

Findings from a full code review. All projects compile cleanly against ESP‑IDF v5.4 /
`esp32c3` (22/22, only `-Wmissing-field-initializers` warnings), so everything below is
behavioural and **needs verifying with boards plugged in locally** before/after fixing.

Legend: `[ ]` open · **Bug** confirmed by reading the code · **Likely** probable on
hardware · **Doc** documentation/comment mismatch.

## Bugs
- [x] **Bug — `bt_to_i2c` rejects writes of 31–32 bytes.** *(fixed in code; verify on hardware)* `main.cpp:215`
  `acc[3 + kMaxData]` (35 B) can't hold a max frame of `2 + 3 + 32` = 37 B; the frame is
  discarded and no reply is sent. Fix: `acc[2 + 3 + kMaxData]`, check `len > 3 + kMaxData`.
  *Test:* `bt_to_i2c` + `i2c_master_slave/slave`, send wlen=32.

## Likely bugs
- [ ] **`EN_5V` (GPIO3) never driven HIGH** in any RS232/RS485/CAN example (loopbacks,
  Wi‑Fi/BT bridges, Modbus), although `board_pins.h` says HIGH powers the transceiver
  rail. Works only if the pin defaults high. *Test:* measure 5 V rail at boot; confirm
  schematic pull-up. Fix: drive HIGH explicitly, or document the pull-up.
- [ ] **BLE CAN/SPI/I2C bridges lose data / desync** (`bt_to_can`, `bt_to_spi`, `bt_to_i2c`):
  - [x] overflow path discards the newly read bytes as well — *fixed: receive straight
    into the accumulator's free space*;
  - [x] `xStreamBufferSend` partial writes — *fixed: whole write or disconnect*;
  - [x] reassembly state not reset on disconnect — *fixed: flush on disconnect*;
  - [x] framing error can't resync — *fixed: drop the connection*;
  - [ ] partial notify on `BLE_HS_ENOMEM` leaves a half frame on the stream (TX side, open).
  *Test:* burst several frames in one write; send a bad length; disconnect mid-frame
  then reconnect and check the next frame is handled.
- [ ] **Dead TCP clients hang the Wi‑Fi converters and the Modbus gateway.** No
  keepalive / receive timeout, single client, `listen(1)`: a client that vanishes
  without FIN blocks all new clients. Fix: `SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT`
  and/or `SO_RCVTIMEO`. *Test:* connect, power off the client, try to reconnect.
- [ ] **Modbus RTU timing** (`components/modbus/include/modbus_rtu.h`):
  - `pdMS_TO_TICKS(gap_ms)` is 0 at the default 100 Hz tick → frames > ~120 B (FIFO
    threshold) truncated / CRC fail (e.g. FC03 > ~57 regs at 9600);
  - UART RX timeout is the default 10 chars, not 3.5 → set `uart_set_rx_timeout`;
  - `uart_wait_tx_done(100 ms)` too short for large frames at 9600; results unchecked;
  - master doesn't check reply function code; accepts 4-byte frames.
  *Test:* read 100 registers at 9600 baud.
- [ ] **CAN:**
  - `can_to_can` — both nodes transmit ID `0x100` (illegal on a shared bus); give each
    node a unique ID;
  - no bus-off recovery anywhere (`twai_initiate_recovery`);
  - `wifi_to_can` forwards ≤ ~100 frames/s with a 5-frame RX queue; stale frames not
    cleared between clients.
  *Test:* busy bus at 500 kbit/s; unplug/short bus to force bus-off.
- [ ] **`modbus_tcp_client`** closes/reopens the TCP connection on every Modbus
  exception instead of logging it; byte count not checked against the request.
- [ ] **I2C bridges** don't reject addresses > 0x7F (0xC2 silently addresses 0x42).
- [ ] **`ble_nus_tester`** can stall forever: `ble_gap_connect` failure and missing
  service/characteristics don't restart scanning / disconnect.
- [ ] **`bt_to_rs232` / `bt_to_rs485`** block the NimBLE host in `uart_write_bytes`
  when the UART TX buffer is full (minor).
- [ ] **`wifi_to_spi` / `bt_to_spi`** — `gpio_reset_pin(EN_5V)` enables the pull-up
  before driving LOW, briefly enabling the 5 V rail (minor).

## Documentation mismatches
- [ ] RS485 `~RE`: `board_pins.h` says DE/~RE tied; READMEs say receiver always
  enabled. Check schematic — decides whether TX echo matters.
- [ ] `board_pins.h` says "CAN needs re-verification"; README says all verified.
- [ ] `board_pins.h` SPI comment: "every borrowed pin is a transceiver input" —
  MISO/GPIO7 is an output (same block says so).
- [ ] `can_to_can/sdkconfig.defaults` and `rs485_to_rs485/sdkconfig.defaults` comments
  list wrong GPIOs (should be GPIO8/6 and GPIO9/7).
- [ ] "C++17 cap": `target_compile_features(cxx_std_17)` is a minimum; code actually
  builds as `gnu++2b`.
- [ ] BLE READMEs "requests MTU 247": the peripheral never initiates an exchange.
- [ ] `spi_master_slave` README: slave doesn't "lag by one transaction" — it sends its
  own counter.
- [ ] `wifi_to_spi` / `bt_to_spi` READMEs: test slave only does 4-byte transfers.
- [ ] Modbus component described as master-only; it also provides `RtuSlave`.

## Missing
- [ ] CI: GitHub Actions matrix build with `espressif/idf:v5.4` for all 22 projects.
- [ ] READMEs for `components/board` and `components/modbus` (incl. frame-size limit).
