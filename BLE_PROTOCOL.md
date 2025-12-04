# BLE Protocol

## Service & Advertising
- Primary service UUID: `14f0514a-e15f-4ad3-89a6-b4cb3ac86abe`.
- GAP device name: `ESP32-GPS-BLE`, advertising name: `GPS-C3`, no scan response payload.
- Advertising intervals: 0x0800–0x1000; service UUID is included in the advertisement.

## Characteristics
- `12c64fea-7ed9-40be-9c7e-9912a5050d23` (`READ`, `NOTIFY`) — navigation telemetry. JSON `{"lt":<lat>,"lg":<lon>,"hd":<deg>,"spd":<m/s>,"alt":<m>}` with decimal degrees for lat/lon; notifications fire when changes exceed epsilons (≈1e-5° lat/lon, 1.0° heading, 0.2 m/s speed, 0.5 m altitude).
- `3e4f5d6c-7b8a-9d0e-1f2a-3b4c5d6e7f8a` (`READ`, `NOTIFY`) — system status. JSON `{"fix":<0|1>,"hdop":<float>,"signals":[...],"ttff":<sec>}`; `signals` is an array of ASCII digits (`'1'`/`'2'`/`'3'` for weak/medium/strong SNR buckets). `ttff` stays `-1` until the first fix.
- `f877c02d-5a02-4cc7-a4f6-e4bb49519eb9` (`READ`) — debug snapshot. JSON `{"signalsDb":[...],"visible":<n>,"active":<n>,"temp":<float|null>,"satellites":[{"id":<prn>,"snr":<dB>,"c":<1-5>,"active":<0|1>,"el":<deg>,"az":<deg>}],"uptime":<sec>}`. `signalsDb` contains SNRs for active satellites; `visible`/`active` mirror parser counters; `temp` is chip temperature in °C if available; each `satellites` entry shows PRN, raw SNR, constellation code (1 GPS, 2 GLONASS, 3 Galileo, 4 BeiDou, 5 QZSS), active flag, elevation, and azimuth. Read-only, no notifications; uptime computed at read time.
- `81b2c6f8-cb9e-4069-9a2e-9e5abca5d56e` (`READ`, `NOTIFY`) — input voltage. JSON `{"vin":<volts>}` derived from IO1 divider (100k→VCC, 12.1k→GND) plus 0.3 V diode compensation; sampled every second.
- `9b9a3f07-3a36-4c74-a48a-4ad0d68f1d39` (`READ`) — Wi‑Fi status. JSON `{"st":"connected|connecting|disconnected","ip":"<optional ip>"}`; `ip` is set when STA is up or AP is active.
- `a37f8c1b-281d-4e15-8fb2-0b7e6ebd21c0` (`READ`, `WRITE`) — Wi‑Fi AP control. Write `'1'` to start AP, `'0'` to request shutdown; reads mirror the active state.
- `d047f6b3-5f7c-4e5b-9c21-4c0f2b6a8f10` (`READ`, `WRITE`) — operation mode. `'0'` = navigation (default), `'1'` = UART passthrough; characteristic always reflects the real mode.
- `2ffc9c6e-34e2-4ad4-af74-9493f5276965` (`READ`, `WRITE`) — GNSS receiver type. `'0'` = u-blox (default), `'1'` = generic NMEA-only. Stored in NVS; when set to generic the firmware skips UBX configuration on boot and keeps plain NMEA parsing.
- `f3a1a816-28f2-4b6d-9f76-6f7aa2d06123` (`READ`, `WRITE`) — GPS UART baud rate. ASCII decimal `4800`–`921600`; valid writes reinit the GPS UART and persist to NVS for reboot.
- `1fd95e59-993e-4bf5-a0b7-f481508c9a94` (`READ`, `WRITE`) — UBX GNSS profile. `'0'` Full systems (default), `'1'` GLONASS+BeiDou+Galileo, `'2'` GLONASS only, `'3'` Custom. Persists to NVS; custom uses the stored CFG-VALSET frame or falls back to Full systems if absent.
- `7f0c9ad9-c6e8-4d2a-b3c1-1703708c6c2d` (`READ`, `WRITE`) — UBX base settings profile. `'0'` Default RAM+BBR script, `'1'` Custom RAM-only script. Persists to NVS; custom replays only the stored command.
- `0abf4f57-12a2-47d9-9c61-96e0d47f332b` (`READ`, `WRITE`) — custom UBX GNSS profile frame. Space-separated hex of a full UBX frame (sync, class, id, LEN, payload, checksum); validated then stored in NVS and applied on boot or when profile = custom.
- `4b88f5a8-3b35-4c64-a241-0c7fdfced0e0` (`READ`, `WRITE`) — custom UBX base settings frame. Same hex format; stored in NVS and replayed to RAM when base settings profile = custom.
- `c4e6f890-6b5e-4f1b-9d2e-7a3c8d2f1b01` (`READ`, `NOTIFY`) — build version. ASCII `BUILD_VERSION` string (timestamp-like, e.g. `20251124164604`) for firmware identification; updated on boot.
- `6b5d5304-4523-4db4-9a31-0f3d88c2ce11` (`WRITE`) — keepalive. Write any byte at least once every 10 s; inactivity drops the BLE link. Payload is ignored.
- `0f6f8ff7-1b61-4d44-9f31-3536c3a601a7` (`READ`, `WRITE`, `NOTIFY`) — OTA enable/guard. Write `'1'` to open the OTA window, `'0'` to close. Reads mirror state; notifications fire on auto-close. When enabled, ElegantOTA UI is served at `http://<ip>/update` on port 80. If no STA/AP is up, the device auto-starts AP for OTA. The window closes after 10 minutes, on BLE disconnect, or right after a successful upload; AP started for OTA is shut down on close.

## Serial Passthrough Mode
- BLE and Wi‑Fi stay active; GNSS parsing pauses and nav/status characteristics stop updating while passthrough is on.
- GPS UART bytes forward to USB serial; host bytes feed back to the GPS module. System logs are muted to keep the stream clean.
- Keepalive writes are still required; switch back to `'0'` on the mode characteristic to resume navigation telemetry.
