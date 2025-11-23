#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <NimBLEServer.h>

static const char *OTA_SERVICE_UUID = "c7b44a0c-24c6-4af3-97ec-19ff34d45095";
static const char *OTA_CONTROL_CHAR_UUID =
    "0f6f8ff7-1b61-4d44-9f31-3536c3a601a7";
static const char *OTA_DATA_CHAR_UUID = "cb08c9fd-6c57-4b51-8bbe-20f3214bf3e9";
static const char *OTA_STATUS_CHAR_UUID =
    "d19d3c86-9ba9-4a52-9244-99118bd88d08";

void initOtaService(NimBLEServer *server);
void otaHandleBleDisconnect();

#endif
