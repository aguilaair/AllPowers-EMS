#pragma once

#include "ems_controller.h"

class StatusUi {
 public:
  bool begin();
  void setBacklight(uint8_t duty);

  // Boot splash — call while connecting peripherals.
  void showBootStatus(const char *line);

  void setBroadlinkOk(bool ok);

  // Poll touch and apply EMS actions; redraw when needed.
  // Call poll() before ems.tick(), then draw() after.
  void poll(EmsController &ems);
  void draw(EmsController &ems);

 private:
  enum class Screen : uint8_t { Main, ConfirmLowSoc };

  void flushDisplay();
  void render(const EmsSnapshot &snap);
  void drawMain(const EmsSnapshot &snap);
  void drawConfirm(const EmsSnapshot &snap);
  void drawLinkLeds(int midY, bool bleOk, bool blOk);
  bool handleTouch(EmsController &ems, uint16_t x, uint16_t y);

  bool ready_ = false;
  bool dirty_ = true;
  bool broadlinkOk_ = false;
  Screen screen_ = Screen::Main;
  EmsSnapshot last_{};
  uint32_t lastDrawMs_ = 0;
};
