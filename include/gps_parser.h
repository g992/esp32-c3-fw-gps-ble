#ifndef GPS_PARSER_H
#define GPS_PARSER_H

#include "gps_config.h"
#include <Arduino.h>

// Счетчики спутников по навигационным системам
extern uint8_t gpsSatellites;
extern uint8_t glonassSatellites;
extern uint8_t galileoSatellites;
extern uint8_t beidouSatellites;
extern uint8_t qzssSatellites;

// Совокупные счетчики уровней сигнала
extern uint8_t totalStrongSignal; // Сильный сигнал (более 30 дБ-Гц)
extern uint8_t totalMediumSignal; // Средний сигнал (20-30 дБ-Гц)
extern uint8_t totalWeakSignal;   // Слабый сигнал (менее 20 дБ-Гц)

// Массив для хранения отношения сигнал/шум используемых спутников
extern uint8_t usedSatellitesSNR[MAX_SATELLITES];
extern uint8_t usedSatellitesCount;

/**
 * Простой разбор сообщений GSV без сложных структур.
 * @param nmea NMEA-строка для разбора.
 */
void parseSimpleGSV(const char *nmea);

// Функции вывода удалены: данные передаются только по беспроводному каналу

/**
 * Обновляет массив значений отношения сигнал/шум активных спутников.
 * @param usedCount количество используемых спутников.
 */
void updateUsedSatellitesSNR(uint8_t usedCount);

/**
 * Подсчитывает уровни сигнала только по используемым спутникам.
 */
void countUsedSatellitesSignals();

/**
 * Преобразует координаты из формата DDMM.MMMM в десятичные градусы.
 * @param coord координата в формате DDMM.MMMM.
 * @return координата в десятичных градусах.
 */
float convertToDecimalDegrees(float coord);

#endif // Завершение охранного макроса
