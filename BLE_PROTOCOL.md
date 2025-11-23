# BLE Protocol

## Primary Service
- UUID: `14f0514a-e15f-4ad3-89a6-b4cb3ac86abe`
- Advertised as `GPS-C3` with no scan response payload.

### Characteristics
- `12c64fea-7ed9-40be-9c7e-9912a5050d23` (`READ`, `NOTIFY`) — navigation telemetry
  - JSON payload `{"lt":<latitude>,"lg":<longitude>,"hd":<heading>,"spd":<m/s>,"alt":<meters>}`.
  - Values are decimal degrees (latitude/longitude), degrees (`hd`), meters per second (`spd`), and meters (`alt`). Notifications only occur when the payload changes beyond small EPS thresholds.
- `3e4f5d6c-7b8a-9d0e-1f2a-3b4c5d6e7f8a` (`READ`, `NOTIFY`) — system status
  - JSON payload `{"fix":<0|1>,"hdop":<value>,"signals":[...],"ttff":<seconds>}`.
  - `signals` is an array of ASCII digits where `'1'`/`'2'`/`'3'` stand for weak/medium/strong SNR buckets of tracked satellites. `ttff` is `-1` until the first fix is acquired.
- `a37f8c1b-281d-4e15-8fb2-0b7e6ebd21c0` (`READ`, `WRITE`) — Wi-Fi AP control
  - Write `'1'` to schedule the access point start, `'0'` to request shutdown. Reading back returns the actual AP state.
- `d047f6b3-5f7c-4e5b-9c21-4c0f2b6a8f10` (`READ`, `WRITE`) — operation mode
  - `'0'` keeps the GNSS navigation loop active; `'1'` enables UART passthrough. The characteristic always mirrors the effective mode.
- `f3a1a816-28f2-4b6d-9f76-6f7aa2d06123` (`READ`, `WRITE`) — GPS UART baud rate
  - Accepts an ASCII decimal string between `4800` and `921600`. Successful writes reinitialize the GPS serial port at the requested baud and persist it for subsequent boots.
- `1fd95e59-993e-4bf5-a0b7-f481508c9a94` (`READ`, `WRITE`) — UBX GNSS profile
  - `'0'` = Full systems (default), `'1'` = GLONASS + BeiDou + Galileo, `'2'` = GLONASS only. Writing replays the UBX script (NMEA off → MON-VER → defaults → selected profile → CFG-VALGET verify → NMEA on) and stores the selection in NVS.
- `6b5d5304-4523-4db4-9a31-0f3d88c2ce11` (`WRITE`) — connection keepalive
  - Clients should write any payload (for example `'1'`) at least once every 10 seconds; otherwise the server disconnects the link. The value itself is ignored.

## Serial Passthrough Mode
- BLE and Wi-Fi continue running; GNSS parsing and BLE telemetry pause.
- GPS UART stream is forwarded verbatim to the USB serial port; host bytes are relayed back to the GPS module.
- System logs are suppressed to keep UART output clean.
- Navigation/status characteristics remain static until the mode is disabled; keepalive writes are still required to stay connected.

## OTA Update Service
- UUID: `c7b44a0c-24c6-4af3-97ec-19ff34d45095` (advertised alongside the primary service)
- Clients must keep the navigation service connected to retain the link while streaming updates.

### Characteristics
- `0f6f8ff7-1b61-4d44-9f31-3536c3a601a7` (`READ`, `WRITE`, `WRITE_NR`) — control plane
  - Text payload with semicolon-separated key/value pairs. Required flow:
    - `CMD=START;SIZE=<bytes>;SHA256=<64 hex>;CRC32=<hex>`
    - `CMD=FINISH` after all chunks are delivered.
    - `CMD=ABORT` cancels the active session.
  - `SIZE` must not exceed the `app1` partition. SHA256 and CRC32 are both mandatory and validated before boot swap.
- `cb08c9fd-6c57-4b51-8bbe-20f3214bf3e9` (`WRITE`) — data plane
  - Each packet is acknowledged and contains:
    - 32-bit little-endian image offset (must equal the expected write cursor).
    - 16-bit little-endian payload length (`<= 480` bytes so the full ATT packet stays under 512 B).
    - Raw firmware bytes.
    - 32-bit little-endian CRC32 of the payload bytes.
  - The device checks offset sequencing and per-packet CRC32 before committing to flash; any mismatch aborts the session.
  - Clients must wait for the acknowledgement notification before sending the next packet. The GATT write response only confirms receipt; the subsequent status notification confirms CRC verification.
- `d19d3c86-9ba9-4a52-9244-99118bd88d08` (`READ`, `NOTIFY`) — status
  - JSON updates such as `{"state":"idle"}`, `{"state":"receiving","received":4096,"total":65536}`,
    `{"state":"chunk_ack","next":4096}`, `{"state":"error","message":"sha_mismatch"}`, and `{"state":"ready","message":"rebooting"}`.
  - Chunk acknowledgements mirror the next required offset; errors include `"message"` plus `"offset"` when applicable. The device aborts on disconnect, offset errors, checksum failures, or OTA API errors.
  - See `docs/OTA_STATUS_JSON.md` for a catalog of complete JSON examples for the Android team.

### Boot Behavior
- Firmware images stream into the `ota_1` (app1) slot; the factory `ota_0` image remains untouched for rollback.
- After `FINISH`, the device re-reads the written partition, validates SHA256/CRC32 (when supplied), switches the boot slot, and schedules a restart.
- The freshly booted image must call `esp_ota_mark_app_valid_cancel_rollback()` once self-tests pass; otherwise the bootloader automatically reverts to the factory slot.
