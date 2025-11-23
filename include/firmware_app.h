#ifndef FIRMWARE_APP_H
#define FIRMWARE_APP_H

class FirmwareApp {
public:
  void begin();
  void tick();
  void requestRestart(const char *reason);

private:
  void configurePublishers();
  static void onWifiApStateChanged(bool active);
  void processPendingRestart();

  bool restartPending = false;
  const char *restartReason = nullptr;
  unsigned long restartRequestedAt = 0;
};

FirmwareApp &firmwareApp();

#endif
