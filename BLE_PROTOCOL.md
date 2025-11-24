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
- `9b9a3f07-3a36-4c74-a48a-4ad0d68f1d39` (`READ`) — Wi-Fi status
  - JSON payload `{"st":"connected|connecting|disconnected","ip":"<optional ip>"}`.
  - `ip` is present when a station link is connected or when the built-in AP is active; it carries the reachable IPv4 address for the device.
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
- `0f6f8ff7-1b61-4d44-9f31-3536c3a601a7` (`READ`, `WRITE`, `NOTIFY`) — OTA enable/guard
  - Write `'1'` to open the OTA window; write `'0'` to close it. Reads mirror the effective state, and notifications fire when the device auto-closes the window.
  - When enabled, the device exposes the ElegantOTA UI on port `80` at `/update`. If neither STA nor AP is active, a BLE request automatically starts the AP to expose the update page.
  - The update UI lives at `http://<ip>/update`. Successful uploads reboot automatically; failures keep the window open so the client can retry within the window.
  - The OTA window closes after 10 minutes, on BLE disconnect, or immediately after a successful upload. If the AP was started for OTA, it is shut down when the window closes.

## Serial Passthrough Mode
- BLE and Wi-Fi continue running; GNSS parsing and BLE telemetry pause.
- GPS UART stream is forwarded verbatim to the USB serial port; host bytes are relayed back to the GPS module.
- System logs are suppressed to keep UART output clean.
- Navigation/status characteristics remain static until the mode is disabled; keepalive writes are still required to stay connected.
