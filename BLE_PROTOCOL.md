# BLE Protocol

## Primary Service
- UUID: `14f0514a-e15f-4ad3-89a6-b4cb3ac86abe`

### Characteristics
- `12c64fea-7ed9-40be-9c7e-9912a5050d23` (`READ`, `NOTIFY`)
  - JSON payload `{"lt":<latitude>,"lg":<longitude>,"hd":<heading>,"spd":<m/s>,"alt":<meters>}`.
- `3e4f5d6c-7b8a-9d0e-1f2a-3b4c5d6e7f8a` (`READ`, `NOTIFY`)
  - JSON payload `{"fix":<0|1>,"hdop":<value>,"signals":[...],"ttff":<seconds>}`.
- `a37f8c1b-281d-4e15-8fb2-0b7e6ebd21c0` (`READ`, `WRITE`)
  - Value `'0'`/`'1'`; writing `'1'` schedules Wi-Fi AP start. Read back to confirm current AP state.
- `d047f6b3-5f7c-4e5b-9c21-4c0f2b6a8f10` (`READ`, `WRITE`)
  - Value `'0'` keeps navigation mode; `'1'` enables serial passthrough mode. Read back to confirm the effective mode.
- `f3a1a816-28f2-4b6d-9f76-6f7aa2d06123` (`READ`, `WRITE`)
  - Decimal string for GPS UART baud (4800-921600). Writing reinitializes the GPS serial link with the requested speed and stores the value for the next boot.

## Serial Passthrough Mode
- BLE and Wi-Fi continue running; GNSS parsing and BLE telemetry pause.
- GPS UART stream is forwarded verbatim to the USB serial port; host bytes are relayed back to the GPS module.
- System logs are suppressed to keep UART output clean.
- Navigation/status characteristics remain static until the mode is disabled.
