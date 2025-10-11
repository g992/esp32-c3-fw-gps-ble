#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "gps_config.h"
#include <Arduino.h>

// Переменные для управления светодиодом
extern uint8_t currentStatus;
extern unsigned long lastBlinkTime;
extern bool ledState;
extern bool ppsDetected;

/**
 * Инициализация светодиода статуса
 */
void initStatusLED();

/**
 * Установка статуса индикации
 * @param status новый статус (STATUS_BOOTING, STATUS_NO_FIX и другие).
 */
void setStatus(uint8_t status);

/**
 * Обновление индикации (вызывать в основном цикле)
 */
void updateStatusLED();

/**
 * Обработчик прерывания PPS
 */
void IRAM_ATTR onPPSInterrupt();

#endif // Завершение охранного макроса
