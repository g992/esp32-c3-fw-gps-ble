#ifndef GPS_CONFIG_H
#define GPS_CONFIG_H

// Пины подключения GPS модуля
#define GPS_TX 4  // Вывод ESP TX к входу RX GPS
#define GPS_RX 3  // Вывод ESP RX к выходу TX GPS
#define GPS_EN 7  // Вывод включения питания GPS
#define GPS_PPS 6 // Вывод синхросигнала PPS

// Пины индикации
#define LED_STATUS_PIN 8 // Светодиод статуса с инверсным управлением

#define WIFI_AP_BUTTON_PIN 9   // Кнопка вызова режима точки доступа (активный уровень LOW)
#define WIFI_AP_TRIGGER_MS 5000 // Время удержания для включения AP, мс

// Настройки UART
#define GPS_BAUD_RATE 115200 // Скорость обмена с GPS по UART

// Интервал вывода информации в миллисекундах (10 Гц)
#define OUTPUT_INTERVAL_MS 100

// Максимальное количество спутников для отслеживания
#define MAX_SATELLITES 64

// Статусы индикации
#define STATUS_BOOTING 1  // Загрузка (три секунды непрерывного свечения)
#define STATUS_NO_FIX 2   // Нет фиксации (два импульса в секунду по 150 мс)
#define STATUS_FIX_SYNC 3 // Фиксация с PPS (мигание по фронту PPS)
#define STATUS_NO_MODEM 4 // Нет связи с модемом (три импульса в секунду)
#define STATUS_READY 5    // Готов к работе

// Временные интервалы для индикации (мс)
#define BOOT_DURATION_MS 3000 // Длительность подсветки при запуске
#define BLINK_DURATION_MS 200 // Продолжительность импульса
#define NO_FIX_INTERVAL_MS                                                     \
  800 // Интервал статуса «нет фиксации» (.--.------)
#define NO_MODEM_INTERVAL_MS                                                   \
  400 // Интервал статуса «нет модема» (.--.--.---)

#endif // Завершение охранного макроса
