# План перехода на ООП-архитектуру

## Цели
- Уменьшить объём глобального состояния и скрыть аппаратные детали за объектами.
- Сделать жизненный цикл компонентов явным (`setup`/`loop` → методы экземпляров).
- Облегчить покрытие тестами и расширение функциональности (например, новые режимы, другие транспорты).

## Карта зависимостей (текущее состояние)
```
FirmwareApp stub (src/main.cpp)
├─ initSystemMode / isSerialPassthroughMode / setOperationMode         (system_mode.cpp / SystemModeService)
│    └─ Preferences, GPS_EN pin, wifiManagerSetGnssStreamingEnabled
├─ gpsController().begin()/loop()                                      (gps_controller.cpp)
│    ├─ HardwareSerial GPS UART, iarduino_GPS_NMEA parser
│    └─ updateNavData / updateSystemStatus publishers
├─ initBLE / BLE characteristic updates                                (gps_ble.cpp)
│    ├─ wifiManagerUpdateNavSnapshot / wifiManagerUpdateStatusSnapshot
│    └─ setGpsSerialBaud / registerModeChangeHandler
├─ initStatusLED / setStatus / updateStatusLED                         (led_status.cpp / StatusIndicator)
│    └─ logger.cpp → Serial.* (только при включённых логах)
├─ initWifiManager / updateWifiManager                                 (wifi_manager.cpp)
│    ├─ WiFi, WebServer, DNSServer, Preferences, protobuf
│    └─ логгер (logPrintf/ln) для статуса
└─ Preferences (ESP32 NVS)                                             (gps_controller.cpp, system_mode.cpp)
```

### Глобальные переменные, влияющие на сопряжение
- BLE характеристики и `NimBLEServer` объявлены как `extern` в `gps_ble.h`.
- `system_mode.cpp` хранит `currentMode`, обработчики и флаг логгера.
- `wifi_manager.cpp` содержит объёмное состояние Wi-Fi/AP и TCP-сервера.

### Внешние и аппаратные зависимости
- Аппарат: `HardwareSerial`, GPIO (`GPS_EN`, `LED_STATUS_PIN`, `GPS_PPS`), `Preferences` NVS.
- Библиотеки: `iarduino_GPS_NMEA`, `NimBLE-Arduino`, `WiFi`, `ESPmDNS`, `DNSServer`, `WebServer`, nanopb.
- Все компоненты используют `Arduino` API напрямую — затрудняет подмену в тестах.

## Предлагаемая структура классов
```
FirmwareApp
 ├── SystemModeService
 ├── GpsModule
 │     ├── GpsSerialController
 │     └── GpsDataAggregator (обёртка над iarduino_GPS_NMEA)
 ├── BleTransport
 ├── WifiCoordinator
 │     ├── AccessPointController
 │     ├── StationConnector
 │     └── GnssTcpBroadcaster
 └── StatusIndicator
        └── PpsEdgeDetector
```

### Краткие описания компонентов
- **FirmwareApp**: единая точка входа; инициализирует подкомпоненты в `begin()` и вызывает их `tick()` из `loop()`. Следит за изменением режима и маршрутизирует события (например, переход в passthrough).
- **SystemModeService**: инкапсулирует хранение режима в `Preferences`, управление логами и перезапуск GPS. Публикует события изменения режима через подписчиков (статический массив → `std::array` + метод `subscribe`).
- **GpsModule**: владеет `GpsSerialController` и `GpsDataAggregator`. Отвечает за конфигурацию UART, перезапуск парсера, расчёт TTFF, хранение `satsInfo` и публикацию `NavState`. Предоставляет интерфейсы `read()` (обновить данные), `enterPassthrough()`, `exitPassthrough()`.
- **GpsSerialController**: обёртка над `HardwareSerial`, хранит текущий baudrate, читает/пишет данные, управляет `GPS_EN`. Экспортирует методы `setBaud`, `persistBaud`, `loadBaud`.
- **GpsDataAggregator**: адаптер для `iarduino_GPS_NMEA`, покрывает сбор метрик (fix, HDOP, speed, signals). Не знает о BLE/Wi-Fi; отдаёт чистые структуры.
- **BleTransport**: владеет `NimBLEServer` и характеристиками. Имеет методы `publishNavState`, `publishStatus`, `publishConfig`. Подписывается на `SystemModeService` и `GpsModule` через узкие интерфейсы. События от характеристик переводятся в колбэки (`BleEventListener`), которые реализует `FirmwareApp`.
- **WifiCoordinator**: агрегирует работу с Wi-Fi и веб-сервером. Подкомпоненты:
  - `AccessPointController` — включает/выключает AP, хранит SSID, вызывает колбэки UI.
  - `StationConnector` — управляет подключением к известным сетям, расписанием реконнекта.
  - `GnssTcpBroadcaster` — формирует и рассылает protobuf-пакеты, слушает `NavState`/`Status`.
  Каждая часть получает доступ к logger’у через интерфейс, не напрямую.
- **StatusIndicator**: управляет GPIO светодиода и паттернами. Получает статусы (`SystemStatus`) и PPS события (`PpsEdgeDetector`). Реализует методы `setMode`, `tick`.
- **PpsEdgeDetector**: изолирует ISR для PPS и предоставляет флаг `consumePulse()`. Это позволяет тестировать `StatusIndicator` без железа.

### Интерфейсы данных и событий
- `NavState`, `SystemStatus`, `ConfigState` — структуры-переносчики без логики (можно вынести в `include/domain/`).
- `INavDataConsumer`, `ISystemStatusConsumer` — простые интерфейсы для рассылки данных BLE и Wi-Fi без жёстких связок.
- Единый журнал `ILogger` с реализацией `SerialLogger`, включаемой/отключаемой через `SystemModeService`.

## Этапы миграции
- [x] **Подготовка**: вынести текущие структуры состояния (`satsInfo`, счётчики) в `struct`, добавить интерфейсы для публикации данных без изменения поведения.
- [x] **Выделение сервисов**: создать классы `SystemModeService`, `StatusIndicator` и обернуть существующую логику, сохраняя C-стиль API для совместимости.
- [x] **GpsModule**: перенести настройку `HardwareSerial`, работу с `Preferences` и TTFF в класс; заменить глобальные `static` на поля.
- [x] **BleTransport и WifiCoordinator**: разделить BLE/Wi-Fi на отдельные объекты, внедрить подписчиков на `NavState` и `SystemStatus`.
- [x] **FirmwareApp**: собрать каркас, перевести `setup`/`loop` на вызовы методов экземпляров, убрать оставшиеся глобалы (большая часть логики переведена, остатки глобального API остаются для совместимости).
- [ ] **Уборка**: удалить временные C-стиль обёртки, обновить `BLE_PROTOCOL.md`, дописать тесты под новые классы.

## Риски и зависимости
- Нужно обеспечить, что ISR (`onPPSInterrupt`) и NimBLE callbacks видят живые объекты (статический синглтон или `static FirmwareApp app;` в `main.cpp`).
- Перезапуск GPS и восстановление UART должны остаться детерминированными; потребуется регрессионное тестирование на железе.
- Придётся обновить моки или создать новые для Wi-Fi/Preferences, чтобы не завязывать юнит-тесты на аппарат.
- **Выполненные шаги**
- Состояние режимов перенесено в `SystemModeService`, публичный API сохранён.
- Лед-индикатор инкапсулирован в `StatusIndicator`, глобальные переменные убраны.
- Навигационный цикл и управление UART вынесены в `GpsController`, `main.cpp` сокращён до координации сервисов.
- BLE-слой реализует `NavDataPublisher`/`SystemStatusPublisher`, а `GpsController` рассылает данные через интерфейсы вместо прямых вызовов.
- Wi-Fi менеджер использует тот же publisher-интерфейс, поэтому данные GNSS рассылаются параллельно BLE и TCP без прямых зависимостей.
- Добавлен `FirmwareApp`, который координирует инициализацию и цикл, а `setup`/`loop` делегируют ему управление.
- Удалены временные C-стиль функции `updateNavData`/`updateSystemStatus` и `wifiManagerUpdate*Snapshot`, данные расходятся только через publisher API.
