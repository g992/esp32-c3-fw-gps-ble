#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "gps_config.h"
#include <Arduino.h>

#include "status_indicator.h"

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
 * Текущий статус индикации.
 */
uint8_t getStatusIndicatorState();

/**
 * Обработчик прерывания PPS
 */
void IRAM_ATTR onPPSInterrupt();

#endif // Завершение охранного макроса
