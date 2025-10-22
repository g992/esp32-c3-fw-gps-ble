#include "led_status.h"

#include "logger.h"

uint8_t currentStatus = STATUS_BOOTING;
unsigned long lastBlinkTime = 0;
bool ledState = false;
bool ppsDetected = false;
unsigned long bootStartTime = 0;

uint8_t blinkPatternStep = 0;
unsigned long patternStartTime = 0;

void initStatusLED() {
  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, HIGH);
  pinMode(GPS_PPS, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GPS_PPS), onPPSInterrupt, RISING);

  bootStartTime = millis();
  currentStatus = STATUS_BOOTING;

  logPrintln("[led] Initialising status LED (GPIO8)");
  logPrintln("[led] Mode set: boot (steady on)");
}

void setStatus(uint8_t status) {
  if (currentStatus != status) {
    currentStatus = status;
    lastBlinkTime = millis();
    ledState = false;
    blinkPatternStep = 0;
    patternStartTime = millis();

    switch (status) {
    case STATUS_BOOTING:
      logPrintln("[led] Mode set: boot (steady on)");
      break;
    case STATUS_NO_FIX:
      logPrintln("[led] Mode set: no fix (short-short-long)");
      break;
    case STATUS_FIX_SYNC:
      logPrintln("[led] Mode set: fix with PPS (pps synced)");
      break;
    case STATUS_NO_MODEM:
      logPrintln("[led] Mode set: modem lost (short-short-short)");
      break;
    case STATUS_READY:
      logPrintln("[led] Mode set: ready (off)");
      break;
    default:
      logPrintf("[led] Unknown status %d\n", status);
      break;
    }
  }
}

static inline void writeLedOn(bool on) {
  static bool lastPinHigh = true;
  bool wantPinHigh = on ? false : true;
  if (wantPinHigh != lastPinHigh) {
    digitalWrite(LED_STATUS_PIN, wantPinHigh ? HIGH : LOW);
    lastPinHigh = wantPinHigh;
  }
}

void updateStatusLED() {
  static unsigned long lastLedTick = 0;
  unsigned long now = millis();
  if (now - lastLedTick < 10) {
    return;
  }
  lastLedTick = now;
  unsigned long currentTime = now;

  switch (currentStatus) {
  case STATUS_BOOTING:
    writeLedOn(true);
    if (currentTime - bootStartTime >= BOOT_DURATION_MS) {
      setStatus(STATUS_NO_FIX);
    }
    break;

  case STATUS_NO_FIX: {
    unsigned long patternTime =
        (currentTime - patternStartTime) % 2000;

    if (patternTime < 200) {
      writeLedOn(true);
    } else if (patternTime < 600) {
      writeLedOn(false);
    } else if (patternTime < 800) {
      writeLedOn(true);
    } else {
      writeLedOn(false);
    }
  } break;

  case STATUS_FIX_SYNC:
    if (ppsDetected) {
      writeLedOn(true);
      ppsDetected = false;
      lastBlinkTime = currentTime;
    } else if (currentTime - lastBlinkTime >= BLINK_DURATION_MS) {
      writeLedOn(false);
    }
    break;

  case STATUS_NO_MODEM:
    {
      unsigned long patternTime =
          (currentTime - patternStartTime) % 2000;

      if (patternTime < 200) {
        writeLedOn(true);
      } else if (patternTime < 400) {
        writeLedOn(false);
      } else if (patternTime < 600) {
        writeLedOn(true);
      } else if (patternTime < 800) {
        writeLedOn(false);
      } else if (patternTime < 1000) {
        writeLedOn(true);
      } else {
        writeLedOn(false);
      }
    }
    break;

  case STATUS_READY:
    writeLedOn(false);
    break;
  }
}

void IRAM_ATTR onPPSInterrupt() { ppsDetected = true; }
