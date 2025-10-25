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
- `6b5d5304-4523-4db4-9a31-0f3d88c2ce11` (`WRITE`) — connection keepalive
  - Clients should write any payload (for example `'1'`) at least once every 10 seconds; otherwise the server disconnects the link. The value itself is ignored.

## Serial Passthrough Mode
- BLE and Wi-Fi continue running; GNSS parsing and BLE telemetry pause.
- GPS UART stream is forwarded verbatim to the USB serial port; host bytes are relayed back to the GPS module.
- System logs are suppressed to keep UART output clean.
- Navigation/status characteristics remain static until the mode is disabled; keepalive writes are still required to stay connected.
