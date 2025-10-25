#include "led_status.h"

#include "logger.h"

StatusIndicator &statusIndicator() {
  static StatusIndicator instance;
  return instance;
}

void StatusIndicator::begin() {
  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, HIGH);
  pinMode(GPS_PPS, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GPS_PPS), onPPSInterrupt, RISING);

  bootStartTime = millis();
  currentStatusValue = STATUS_BOOTING;
  lastBlinkTime = bootStartTime;
  ledState = false;
  ppsDetected = false;
  patternStartTime = bootStartTime;
  lastLedTick = 0;
  lastPinHigh = true;

  logPrintln("[led] Initialising status LED (GPIO8)");
  logPrintln("[led] Mode set: boot (steady on)");
}

void StatusIndicator::setStatus(uint8_t status) {
  if (currentStatusValue != status) {
    currentStatusValue = status;
    lastBlinkTime = millis();
    ledState = false;
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

void StatusIndicator::writeLedOn(bool on) {
  bool wantPinHigh = on ? false : true;
  if (wantPinHigh != lastPinHigh) {
    digitalWrite(LED_STATUS_PIN, wantPinHigh ? HIGH : LOW);
    lastPinHigh = wantPinHigh;
  }
  ledState = on;
}

void StatusIndicator::update() {
  unsigned long now = millis();
  if (now - lastLedTick < 10) {
    return;
  }
  lastLedTick = now;
  unsigned long currentTime = now;

  switch (currentStatusValue) {
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

  case STATUS_NO_MODEM: {
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
  } break;

  case STATUS_READY:
    writeLedOn(false);
    break;
  }
}

void StatusIndicator::onPpsPulse() { ppsDetected = true; }

uint8_t StatusIndicator::status() const { return currentStatusValue; }

void initStatusLED() { statusIndicator().begin(); }

void setStatus(uint8_t status) { statusIndicator().setStatus(status); }

void updateStatusLED() { statusIndicator().update(); }

uint8_t getStatusIndicatorState() { return statusIndicator().status(); }

void IRAM_ATTR onPPSInterrupt() { statusIndicator().onPpsPulse(); }
