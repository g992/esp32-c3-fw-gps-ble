#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <NimBLEServer.h>

static const char *OTA_ENABLE_CHAR_UUID = "0f6f8ff7-1b61-4d44-9f31-3536c3a601a7";

void initOtaService(NimBLEService *service);
void otaHandleBleDisconnect();
void otaTick();
bool otaUpdatesEnabled();
bool otaUpdateInProgress();

#endif
