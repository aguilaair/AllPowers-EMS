#include "status_ui.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <cmath>
#include <cstring>

#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"
#include "fonts/FreeSansBold72pt7b.h"
#include "gt911_touch.h"

// Waveshare ESP32-S3-Touch-LCD-2.8C — ST7701 RGB + TCA9554 EXIO for RST/CS.
// https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C

namespace {

constexpr uint8_t TCA9554_ADDR = 0x20;
constexpr uint8_t TCA_REG_OUTPUT = 0x01;
constexpr uint8_t TCA_REG_CONFIG = 0x03;
constexpr uint8_t EXIO_LCD_RST = 0;  // EXIO1
constexpr uint8_t EXIO_LCD_CS = 2;   // EXIO3
constexpr uint8_t EXIO_BUZZER = 7;   // EXIO8

constexpr int PIN_LCD_SCK = 2;
constexpr int PIN_LCD_MOSI = 1;

Arduino_ESP32RGBPanel *g_rgb = nullptr;
Arduino_RGB_Display *g_panel = nullptr;
Arduino_GFX *g_gfx = nullptr;  // Canvas front-buffer; panel is scanned live
Gt911Touch g_touch;

uint8_t g_tcaOut = 0x7F;

constexpr uint16_t COL_BG = 0x0000;
constexpr uint16_t COL_TOP = 0xFFFF;
constexpr uint16_t COL_DISCHARGE = 0x1471;   // teal — net discharging
constexpr uint16_t COL_CHARGE = 0x1B5F;      // blue — net charging
constexpr uint16_t COL_IDLE = 0x5ACB;        // sage — idle, CA on
constexpr uint16_t COL_IDLE_AC_OFF = 0x8410; // cool gray — idle, CA off
constexpr uint16_t COL_TEXT = 0xFFFF;
constexpr uint16_t COL_INK = 0x0000;
constexpr uint16_t COL_MUTED = 0x9CF3;
constexpr uint16_t COL_OK = 0x07E0;
constexpr uint16_t COL_WARN = 0xFE60;
constexpr uint16_t COL_DANGER = 0xE146;
constexpr uint16_t COL_BTN_OFF = 0x3186;
constexpr uint16_t COL_CARD = 0x2124;

constexpr int IDLE_WATTS = 5;

constexpr int MID_Y = 240;
constexpr int LED_R = 10;
constexpr int LED_BLE_X = 200;
constexpr int LED_BL_X = 280;
constexpr int LED_Y = 28;
constexpr int LED_LABEL_Y = 52;
// Native 72pt SOC glyphs (no integer upscale — that looked pixelated).
constexpr int SOC_BASELINE_Y = MID_Y + 54;
constexpr int STATUS_BASELINE = 78;
constexpr int CA_BASELINE = 112;
constexpr int BOOT_TITLE_Y = 200;
constexpr int BOOT_LINE_Y = 280;

// Circular 480×480: keep chrome inside a chord near the bottom-center.
// At y≈428 the usable half-width collapses to ~100px — side labels vanish.
constexpr int TEMP_BASELINE = 348;
constexpr int TEMP_LEFT_CX = 155;
constexpr int TEMP_RIGHT_CX = 325;

constexpr int MODE_Y = 368;
constexpr int MODE_H = 50;
constexpr int MODE_SPAN = 260;
constexpr int MODE_X0 = (480 - MODE_SPAN) / 2;
constexpr int MODE_SEG_W = MODE_SPAN / 3;
constexpr int MODE_BASELINE = MODE_Y + 34;

// Emergency lights — shown when CA hard-cut is active (low SOC top half is free).
constexpr int EMERG_W = 200;
constexpr int EMERG_H = 48;
constexpr int EMERG_X = (480 - EMERG_W) / 2;
constexpr int EMERG_Y = 128;

// Confirm dialog — kept inside the circle
constexpr int BTN_NO_X = 70;
constexpr int BTN_NO_Y = 300;
constexpr int BTN_YES_X = 250;
constexpr int BTN_YES_Y = 300;
constexpr int BTN_CONF_W = 160;
constexpr int BTN_CONF_H = 64;

constexpr uint16_t COL_MUTED_INK = 0x7BEF;  // muted on white half


bool tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool tcaBegin() {
  if (!tcaWrite(TCA_REG_CONFIG, 0x00)) {
    Serial.println(F("[UI] TCA9554 no encontrado"));
    return false;
  }
  g_tcaOut = 0x7F;
  return tcaWrite(TCA_REG_OUTPUT, g_tcaOut);
}

void tcaSetPin(uint8_t exioBit, bool level) {
  if (level) {
    g_tcaOut |= (1 << exioBit);
  } else {
    g_tcaOut &= ~(1 << exioBit);
  }
  g_tcaOut &= ~(1 << EXIO_BUZZER);
  tcaWrite(TCA_REG_OUTPUT, g_tcaOut);
}

void tcaSetPinCb(uint8_t bit, bool level) { tcaSetPin(bit, level); }

void silenceBuzzer() {
  g_tcaOut &= ~(1 << EXIO_BUZZER);
  tcaWrite(TCA_REG_CONFIG, 0x00);
  tcaWrite(TCA_REG_OUTPUT, g_tcaOut);
}

void lcdHardwareReset() {
  silenceBuzzer();
  tcaSetPin(EXIO_LCD_RST, false);
  delay(10);
  tcaSetPin(EXIO_LCD_RST, true);
  delay(50);
  silenceBuzzer();
}

void spi9Write(uint8_t value, bool isData) {
  bool last = isData;
  digitalWrite(PIN_LCD_MOSI, last);
  digitalWrite(PIN_LCD_SCK, LOW);
  digitalWrite(PIN_LCD_SCK, HIGH);
  uint8_t bit = 0x80;
  while (bit) {
    const bool next = (value & bit) != 0;
    if (next != last) {
      last = next;
      digitalWrite(PIN_LCD_MOSI, last);
    }
    digitalWrite(PIN_LCD_SCK, LOW);
    bit >>= 1;
    digitalWrite(PIN_LCD_SCK, HIGH);
  }
}

void st7701Cmd(uint8_t c) { spi9Write(c, false); }
void st7701Data(uint8_t d) { spi9Write(d, true); }

void st7701InitWaveshare() {
  pinMode(PIN_LCD_SCK, OUTPUT);
  pinMode(PIN_LCD_MOSI, OUTPUT);
  digitalWrite(PIN_LCD_SCK, HIGH);
  digitalWrite(PIN_LCD_MOSI, HIGH);

  tcaSetPin(EXIO_LCD_CS, false);
  delay(10);

  st7701Cmd(0xFF);
  st7701Data(0x77);
  st7701Data(0x01);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x13);
  st7701Cmd(0xEF);
  st7701Data(0x08);
  st7701Cmd(0xFF);
  st7701Data(0x77);
  st7701Data(0x01);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x10);
  st7701Cmd(0xC0);
  st7701Data(0x3B);
  st7701Data(0x00);
  st7701Cmd(0xC1);
  st7701Data(0x10);
  st7701Data(0x0C);
  st7701Cmd(0xC2);
  st7701Data(0x07);
  st7701Data(0x0A);
  st7701Cmd(0xC7);
  st7701Data(0x00);
  st7701Cmd(0xCC);
  st7701Data(0x10);
  st7701Cmd(0xCD);
  st7701Data(0x08);
  st7701Cmd(0xB0);
  st7701Data(0x05);
  st7701Data(0x12);
  st7701Data(0x98);
  st7701Data(0x0E);
  st7701Data(0x0F);
  st7701Data(0x07);
  st7701Data(0x07);
  st7701Data(0x09);
  st7701Data(0x09);
  st7701Data(0x23);
  st7701Data(0x05);
  st7701Data(0x52);
  st7701Data(0x0F);
  st7701Data(0x67);
  st7701Data(0x2C);
  st7701Data(0x11);
  st7701Cmd(0xB1);
  st7701Data(0x0B);
  st7701Data(0x11);
  st7701Data(0x97);
  st7701Data(0x0C);
  st7701Data(0x12);
  st7701Data(0x06);
  st7701Data(0x06);
  st7701Data(0x08);
  st7701Data(0x08);
  st7701Data(0x22);
  st7701Data(0x03);
  st7701Data(0x51);
  st7701Data(0x11);
  st7701Data(0x66);
  st7701Data(0x2B);
  st7701Data(0x0F);
  st7701Cmd(0xFF);
  st7701Data(0x77);
  st7701Data(0x01);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x11);
  st7701Cmd(0xB0);
  st7701Data(0x5D);
  st7701Cmd(0xB1);
  st7701Data(0x3E);
  st7701Cmd(0xB2);
  st7701Data(0x81);
  st7701Cmd(0xB3);
  st7701Data(0x80);
  st7701Cmd(0xB5);
  st7701Data(0x4E);
  st7701Cmd(0xB7);
  st7701Data(0x85);
  st7701Cmd(0xB8);
  st7701Data(0x20);
  st7701Cmd(0xC1);
  st7701Data(0x78);
  st7701Cmd(0xC2);
  st7701Data(0x78);
  st7701Cmd(0xD0);
  st7701Data(0x88);
  st7701Cmd(0xE0);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x02);
  st7701Cmd(0xE1);
  st7701Data(0x06);
  st7701Data(0x30);
  st7701Data(0x08);
  st7701Data(0x30);
  st7701Data(0x05);
  st7701Data(0x30);
  st7701Data(0x07);
  st7701Data(0x30);
  st7701Data(0x00);
  st7701Data(0x33);
  st7701Data(0x33);
  st7701Cmd(0xE2);
  st7701Data(0x11);
  st7701Data(0x11);
  st7701Data(0x33);
  st7701Data(0x33);
  st7701Data(0xF4);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0xF4);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Cmd(0xE3);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x11);
  st7701Data(0x11);
  st7701Cmd(0xE4);
  st7701Data(0x44);
  st7701Data(0x44);
  st7701Cmd(0xE5);
  st7701Data(0x0D);
  st7701Data(0xF5);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x0F);
  st7701Data(0xF7);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x09);
  st7701Data(0xF1);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x0B);
  st7701Data(0xF3);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Cmd(0xE6);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x11);
  st7701Data(0x11);
  st7701Cmd(0xE7);
  st7701Data(0x44);
  st7701Data(0x44);
  st7701Cmd(0xE8);
  st7701Data(0x0C);
  st7701Data(0xF4);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x0E);
  st7701Data(0xF6);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x08);
  st7701Data(0xF0);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Data(0x0A);
  st7701Data(0xF2);
  st7701Data(0x30);
  st7701Data(0xF0);
  st7701Cmd(0xE9);
  st7701Data(0x36);
  st7701Data(0x01);
  st7701Cmd(0xEB);
  st7701Data(0x00);
  st7701Data(0x01);
  st7701Data(0xE4);
  st7701Data(0xE4);
  st7701Data(0x44);
  st7701Data(0x88);
  st7701Data(0x40);
  st7701Cmd(0xED);
  st7701Data(0xFF);
  st7701Data(0x10);
  st7701Data(0xAF);
  st7701Data(0x76);
  st7701Data(0x54);
  st7701Data(0x2B);
  st7701Data(0xCF);
  st7701Data(0xFF);
  st7701Data(0xFF);
  st7701Data(0xFC);
  st7701Data(0xB2);
  st7701Data(0x45);
  st7701Data(0x67);
  st7701Data(0xFA);
  st7701Data(0x01);
  st7701Data(0xFF);
  st7701Cmd(0xEF);
  st7701Data(0x08);
  st7701Data(0x08);
  st7701Data(0x08);
  st7701Data(0x45);
  st7701Data(0x3F);
  st7701Data(0x54);
  st7701Cmd(0xFF);
  st7701Data(0x77);
  st7701Data(0x01);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Data(0x00);
  st7701Cmd(0x11);
  delay(120);
  st7701Cmd(0x3A);
  st7701Data(0x66);
  st7701Cmd(0x36);
  st7701Data(0x00);
  st7701Cmd(0x35);
  st7701Data(0x00);
  st7701Cmd(0x29);
  tcaSetPin(EXIO_LCD_CS, true);
  delay(10);
}

bool hit(uint16_t x, uint16_t y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

int gfxTextWidth(const char *text, const GFXfont *font) {
  int w = 0;
  const uint16_t first = pgm_read_word(&font->first);
  const uint16_t last = pgm_read_word(&font->last);
  GFXglyph *glyphs = (GFXglyph *)pgm_read_pointer(&font->glyph);
  for (const char *p = text; *p; ++p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    if (c < first || c > last) {
      continue;
    }
    w += pgm_read_byte(&glyphs[c - first].xAdvance);
  }
  return w;
}

void drawCenteredFont(const char *text, int cx, int baselineY, const GFXfont *font,
                      uint16_t color) {
  g_gfx->setFont(font);
  g_gfx->setTextSize(1);
  g_gfx->setTextColor(color);
  const int w = gfxTextWidth(text, font);
  g_gfx->setCursor(cx - w / 2, baselineY);
  g_gfx->print(text);
  g_gfx->setFont(nullptr);
}

void drawButton(int x, int y, int w, int h, uint16_t fill, const char *label) {
  g_gfx->fillRoundRect(x, y, w, h, 18, fill);
  drawCenteredFont(label, x + w / 2, y + h / 2 + 5, &FreeSansBold12pt7b, COL_TEXT);
}

uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  if (a == 0) {
    return bg;
  }
  if (a >= 255) {
    return fg;
  }
  const uint8_t inv = 255 - a;
  const uint32_t fr = (fg >> 11) & 0x1F;
  const uint32_t fg6 = (fg >> 5) & 0x3F;
  const uint32_t fb = fg & 0x1F;
  const uint32_t br = (bg >> 11) & 0x1F;
  const uint32_t bg6 = (bg >> 5) & 0x3F;
  const uint32_t bb = bg & 0x1F;
  const uint32_t r = (fr * a + br * inv + 127) / 255;
  const uint32_t g = (fg6 * a + bg6 * inv + 127) / 255;
  const uint32_t b = (fb * a + bb * inv + 127) / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void drawGfxCharSplit(int16_t x, int16_t y, uint8_t c, const GFXfont *font, int midY,
                      uint16_t fgAbove, uint16_t fgBelow) {
  const uint16_t first = pgm_read_word(&font->first);
  const uint16_t last = pgm_read_word(&font->last);
  if (c < first || c > last) {
    return;
  }
  GFXglyph *glyph = &(((GFXglyph *)pgm_read_pointer(&font->glyph))[c - first]);
  uint8_t *bitmap = (uint8_t *)pgm_read_pointer(&font->bitmap);

  uint16_t bitOffset = pgm_read_word(&glyph->bitmapOffset);
  const uint8_t gw = pgm_read_byte(&glyph->width);
  const uint8_t gh = pgm_read_byte(&glyph->height);
  const int8_t xo = pgm_read_byte(&glyph->xOffset);
  const int8_t yo = pgm_read_byte(&glyph->yOffset);

  uint8_t bits = 0;
  uint8_t bit = 0;
  for (int16_t yy = 0; yy < gh; ++yy) {
    for (int16_t xx = 0; xx < gw; ++xx) {
      if (!(bit >>= 1)) {
        bits = pgm_read_byte(&bitmap[bitOffset++]);
        bit = 0x80;
      }
      if (!(bits & bit)) {
        continue;
      }
      const int px = x + xo + xx;
      const int py = y + yo + yy;
      if (px < 0 || px >= 480 || py < 0 || py >= 480) {
        continue;
      }
      g_gfx->drawPixel(px, py, py < midY ? fgAbove : fgBelow);
    }
  }
}

void drawSplitText(const char *text, int16_t x, int16_t baselineY, const GFXfont *font,
                   int midY, uint16_t fgAbove, uint16_t fgBelow) {
  const uint16_t first = pgm_read_word(&font->first);
  const uint16_t last = pgm_read_word(&font->last);
  for (const char *p = text; *p; ++p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    drawGfxCharSplit(x, baselineY, c, font, midY, fgAbove, fgBelow);
    if (c >= first && c <= last) {
      GFXglyph *glyph = &(((GFXglyph *)pgm_read_pointer(&font->glyph))[c - first]);
      x += pgm_read_byte(&glyph->xAdvance);
    }
  }
}

void drawCenteredSplit(const char *text, int cx, int baselineY, const GFXfont *font, int midY,
                       uint16_t fgAbove = COL_INK, uint16_t fgBelow = COL_TEXT) {
  drawSplitText(text, cx - gfxTextWidth(text, font) / 2, baselineY, font, midY, fgAbove,
                fgBelow);
}

void drawSplitSoc(const char *text, int midY) {
  drawCenteredSplit(text, 240, SOC_BASELINE_Y, &FreeSansBold72pt7b, midY);
}

int modeSegX(int index) { return MODE_X0 + index * MODE_SEG_W; }

AcOverride overrideForSeg(int index) {
  if (index == 0) {
    return AcOverride::Auto;
  }
  if (index == 1) {
    return AcOverride::ForceOn;
  }
  return AcOverride::ForceOff;
}

int segForOverride(AcOverride o) {
  if (o == AcOverride::ForceOn) {
    return 1;
  }
  if (o == AcOverride::ForceOff) {
    return 2;
  }
  return 0;
}

void drawModeStrip(AcOverride current, int midY) {
  static const char *const labels[3] = {"Auto", "On", "Off"};
  const int selected = segForOverride(current);

  for (int i = 0; i < 3; ++i) {
    const int cx = modeSegX(i) + MODE_SEG_W / 2;
    const bool on = (i == selected);
    const uint16_t above = on ? COL_INK : COL_MUTED_INK;
    const uint16_t below = on ? COL_TEXT : COL_MUTED;
    drawCenteredSplit(labels[i], cx, MODE_BASELINE, &FreeSansBold12pt7b, midY, above, below);
    if (on) {
      const int uw = gfxTextWidth(labels[i], &FreeSansBold12pt7b);
      const int uy = MODE_BASELINE + 8;
      for (int px = cx - uw / 2; px < cx + uw / 2; ++px) {
        if (px < 0 || px >= 480 || uy < 0 || uy >= 480) {
          continue;
        }
        g_gfx->drawPixel(px, uy, uy < midY ? above : below);
      }
    }
  }
}

bool snapChanged(const EmsSnapshot &a, const EmsSnapshot &b) {
  return a.mode != b.mode || a.socPct != b.socPct || a.acOn != b.acOn ||
         a.climate != b.climate || a.acOutletOn != b.acOutletOn ||
         a.outletCut != b.outletCut || a.outletEmergency != b.outletEmergency ||
         a.bleOk != b.bleOk || a.lowSoc != b.lowSoc ||
         a.overrideActive != b.overrideActive || a.acOverride != b.acOverride ||
         a.inWatts != b.inWatts || a.outWatts != b.outWatts ||
         fabsf(a.ambientC - b.ambientC) > 0.15f ||
         fabsf(a.humidityPct - b.humidityPct) > 1.0f ||
         strcmp(a.reasonEs ? a.reasonEs : "", b.reasonEs ? b.reasonEs : "") != 0;
}

int netWatts(const EmsSnapshot &snap) {
  const int inW = snap.inWatts < 0 ? 0 : snap.inWatts;
  const int outW = snap.outWatts < 0 ? 0 : snap.outWatts;
  return inW - outW;
}

uint16_t fillColorForPower(const EmsSnapshot &snap) {
  const int net = netWatts(snap);
  if (net > IDLE_WATTS) {
    return COL_CHARGE;
  }
  if (net < -IDLE_WATTS) {
    return COL_DISCHARGE;
  }
  return snap.acOn ? COL_IDLE : COL_IDLE_AC_OFF;
}

// Soft 2px waterline so the split isn't a harsh stair-step.
void fillSplitBackground(int midY, uint16_t fill) {
  if (midY <= 0) {
    g_gfx->fillScreen(fill);
    return;
  }
  if (midY >= 480) {
    g_gfx->fillScreen(COL_TOP);
    return;
  }
  g_gfx->fillRect(0, 0, 480, midY, COL_TOP);
  g_gfx->fillRect(0, midY, 480, 480 - midY, fill);
  if (midY > 0 && midY < 480) {
    const uint16_t edge = blend565(fill, COL_TOP, 128);
    g_gfx->drawFastHLine(0, midY - 1, 480, blend565(COL_TOP, fill, 64));
    g_gfx->drawFastHLine(0, midY, 480, edge);
  }
}

}  // namespace

bool StatusUi::begin() {
  pinMode(PIN_LCD_BL, OUTPUT);
  setBacklight(LCD_BACKLIGHT_DUTY);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  tcaBegin();
  silenceBuzzer();
  lcdHardwareReset();
  silenceBuzzer();

  st7701InitWaveshare();
  Serial.println(F("[UI] ST7701 listo"));

  g_rgb = new Arduino_ESP32RGBPanel(
      40, 39, 38, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21,
      1, 50, 8, 10, 1, 8, 2, 18, 0, 16000000);

  // Panel FB is scanned live — draw into a PSRAM canvas, then blit once (no blink).
  g_panel = new Arduino_RGB_Display(480, 480, g_rgb, 0, false, nullptr, GFX_NOT_DEFINED,
                                    nullptr, 0);
  g_gfx = new Arduino_Canvas(480, 480, g_panel);

  if (!g_gfx || !g_gfx->begin()) {
    Serial.println(F("[UI] LCD falló — solo Serial"));
    ready_ = false;
    silenceBuzzer();
    return false;
  }
  silenceBuzzer();

  if (!g_touch.begin(tcaSetPinCb)) {
    Serial.println(F("[UI] tactil no disponible"));
  }

  g_gfx->fillScreen(COL_TOP);
  g_gfx->fillRect(0, 240, 480, 240, COL_IDLE);
  drawCenteredFont("Refugio", 240, BOOT_TITLE_Y, &FreeSansBold24pt7b, COL_INK);
  drawCenteredFont("iniciando…", 240, BOOT_LINE_Y, &FreeSansBold12pt7b, COL_TEXT);
  flushDisplay();
  ready_ = true;
  dirty_ = true;
  Serial.println(F("[UI] listo"));
  return true;
}

void StatusUi::flushDisplay() {
  if (!g_gfx) {
    return;
  }
  g_gfx->flush();
  if (g_panel) {
    g_panel->flush();
  }
}

void StatusUi::showBootStatus(const char *line) {
  if (!ready_ || !g_gfx) {
    return;
  }
  // Keep splash composition; refresh the status line on the lower half.
  g_gfx->fillRect(40, BOOT_LINE_Y - 28, 400, 48, COL_IDLE);
  drawCenteredFont(line ? line : "", 240, BOOT_LINE_Y, &FreeSansBold12pt7b, COL_TEXT);
  flushDisplay();
}

void StatusUi::setBroadlinkOk(bool ok) {
  if (broadlinkOk_ == ok) {
    return;
  }
  broadlinkOk_ = ok;
  dirty_ = true;
}

void StatusUi::setBacklight(uint8_t duty) { analogWrite(PIN_LCD_BL, duty); }

void StatusUi::drawLinkLeds(int midY, bool bleOk, bool blOk) {
  g_gfx->fillCircle(LED_BLE_X, LED_Y, LED_R, bleOk ? COL_OK : COL_DANGER);
  g_gfx->fillCircle(LED_BL_X, LED_Y, LED_R, blOk ? COL_OK : COL_DANGER);
  // Tiny labels — split across waterline like other chrome.
  drawCenteredSplit("BLE", LED_BLE_X, LED_LABEL_Y, &FreeSans9pt7b, midY, COL_MUTED_INK, COL_MUTED);
  drawCenteredSplit("IR", LED_BL_X, LED_LABEL_Y, &FreeSans9pt7b, midY, COL_MUTED_INK, COL_MUTED);
}

void StatusUi::drawMain(const EmsSnapshot &snap) {
  // Fill rises with SOC; color = net charge / discharge / idle
  const int soc = snap.socPct < 0 ? 0 : (snap.socPct > 100 ? 100 : snap.socPct);
  const int margin = 36;
  const int midY = map(soc, 0, 100, 480 - margin, margin);
  const uint16_t fill = fillColorForPower(snap);
  const int net = netWatts(snap);

  fillSplitBackground(midY, fill);

  drawLinkLeds(midY, snap.bleOk, broadlinkOk_);

  char top[40];
  if (net > IDLE_WATTS) {
    snprintf(top, sizeof(top), "CARGANDO %dW", net);
  } else if (net < -IDLE_WATTS) {
    snprintf(top, sizeof(top), "DESCARGANDO %dW", -net);
  } else if (!snap.bleOk) {
    snprintf(top, sizeof(top), "SIN BATERIA");
  } else if (snap.lowSoc) {
    snprintf(top, sizeof(top), "RESERVA");
  } else {
    snprintf(top, sizeof(top), "EN ESPERA");
  }
  drawCenteredSplit(top, 240, STATUS_BASELINE, &FreeSansBold12pt7b, midY);

  // Climate state via IR (CA outlet stays powered for AC + emergency lights).
  if (snap.climate == ClimateCmd::Cool) {
    drawCenteredSplit("FRIO", 240, CA_BASELINE, &FreeSansBold12pt7b, midY);
  } else if (snap.climate == ClimateCmd::Heat) {
    drawCenteredSplit("CALOR", 240, CA_BASELINE, &FreeSansBold12pt7b, midY);
  } else {
    drawCenteredSplit("AC OFF", 240, CA_BASELINE, &FreeSansBold12pt7b, midY, COL_MUTED_INK,
                      COL_MUTED);
  }

  // Emergency lights on CA rail — visible whenever the ≤15% cut is latched.
  if (snap.outletCut || snap.outletEmergency) {
    const uint16_t fill = snap.outletEmergency ? COL_WARN : COL_DANGER;
    const char *label = snap.outletEmergency ? "LUCES ON" : "LUCES";
    drawButton(EMERG_X, EMERG_Y, EMERG_W, EMERG_H, fill, label);
  }

  char socBuf[12];
  if (snap.socPct >= 0) {
    snprintf(socBuf, sizeof(socBuf), "%d%%", snap.socPct);
  } else {
    snprintf(socBuf, sizeof(socBuf), "--");
  }
  drawSplitSoc(socBuf, midY);

  char left[16];
  char right[16];
  if (!isnan(snap.ambientC)) {
    snprintf(left, sizeof(left), "%.0f C", snap.ambientC);
  } else {
    snprintf(left, sizeof(left), "-- C");
  }
  if (!isnan(snap.humidityPct)) {
    snprintf(right, sizeof(right), "%.0f%%", snap.humidityPct);
  } else {
    snprintf(right, sizeof(right), "--%%");
  }
  drawCenteredSplit(left, TEMP_LEFT_CX, TEMP_BASELINE, &FreeSansBold12pt7b, midY);
  drawCenteredSplit(right, TEMP_RIGHT_CX, TEMP_BASELINE, &FreeSansBold12pt7b, midY);

  // Auto/On/Off can't drive climate under the ≤15% CA hard cut — hide them.
  if (!snap.outletCut) {
    drawModeStrip(snap.acOverride, midY);
  }
}

void StatusUi::drawConfirm(const EmsSnapshot &snap) {
  const int soc = snap.socPct < 0 ? 0 : (snap.socPct > 100 ? 100 : snap.socPct);
  const int midY = map(soc, 0, 100, 444, 36);
  const uint16_t fill = fillColorForPower(snap);
  fillSplitBackground(midY, fill);
  g_gfx->fillRoundRect(40, 110, 400, 280, 24, COL_CARD);

  drawCenteredFont("Bateria baja", 240, 155, &FreeSansBold12pt7b, COL_WARN);
  char line[40];
  if (snap.socPct >= 0) {
    snprintf(line, sizeof(line), "Queda un %d%%", snap.socPct);
  } else {
    snprintf(line, sizeof(line), "Reserva critica");
  }
  drawCenteredFont(line, 240, 200, &FreeSansBold12pt7b, COL_TEXT);
  drawCenteredFont("Encender el clima", 240, 245, &FreeSans9pt7b, COL_TEXT);
  drawCenteredFont("igualmente?", 240, 275, &FreeSans9pt7b, COL_TEXT);

  drawButton(BTN_NO_X, BTN_NO_Y, BTN_CONF_W, BTN_CONF_H, COL_BTN_OFF, "Cancelar");
  drawButton(BTN_YES_X, BTN_YES_Y, BTN_CONF_W, BTN_CONF_H, COL_DANGER, "Encender");
}

void StatusUi::render(const EmsSnapshot &snap) {
  Serial.printf(
      "[STATUS] mode=%s SOC=%d%% T=%.1fC RH=%.0f%% clima=%s BLE=%d | %s\n",
      emsModeName(snap.mode), snap.socPct, snap.ambientC, snap.humidityPct,
      climateCmdName(snap.climate), snap.bleOk ? 1 : 0, snap.reason);

  if (!ready_ || !g_gfx) {
    return;
  }
  if (screen_ == Screen::ConfirmLowSoc) {
    drawConfirm(snap);
  } else {
    drawMain(snap);
  }
  g_gfx->flush();
  if (g_panel) {
    g_panel->flush();
  }
  last_ = snap;
  lastDrawMs_ = millis();
  dirty_ = false;
}

bool StatusUi::handleTouch(EmsController &ems, uint16_t x, uint16_t y) {
  const EmsSnapshot snap = ems.snapshot();

  if (screen_ == Screen::ConfirmLowSoc) {
    if (hit(x, y, BTN_NO_X, BTN_NO_Y, BTN_CONF_W, BTN_CONF_H)) {
      screen_ = Screen::Main;
      dirty_ = true;
      return true;
    }
    if (hit(x, y, BTN_YES_X, BTN_YES_Y, BTN_CONF_W, BTN_CONF_H)) {
      ems.confirmLowSocAcOn();
      screen_ = Screen::Main;
      dirty_ = true;
      return true;
    }
    return false;
  }

  if ((snap.outletCut || snap.outletEmergency) &&
      hit(x, y, EMERG_X, EMERG_Y, EMERG_W, EMERG_H)) {
    ems.toggleOutletEmergency();
    dirty_ = true;
    return true;
  }

  // Mode strip is hidden (and inert) during ≤15% CA cut.
  if (!snap.outletCut && y >= MODE_Y && y < MODE_Y + MODE_H) {
    for (int i = 0; i < 3; ++i) {
      if (!hit(x, y, modeSegX(i), MODE_Y, MODE_SEG_W, MODE_H)) {
        continue;
      }
      const AcOverride want = overrideForSeg(i);
      if (want == snap.acOverride) {
        return true;
      }
      if (want == AcOverride::Auto) {
        ems.setAutoMode();
        dirty_ = true;
      } else if (want == AcOverride::ForceOn) {
        if (ems.requestAcOn()) {
          screen_ = Screen::ConfirmLowSoc;
        }
        dirty_ = true;
      } else {
        ems.requestAcOff();
        dirty_ = true;
      }
      return true;
    }
  }

  return false;
}

void StatusUi::poll(EmsController &ems) {
  TouchPoint tp;
  if (g_touch.readPress(tp)) {
    Serial.printf("[UI] toque %d,%d\n", tp.x, tp.y);
    handleTouch(ems, tp.x, tp.y);
  }
}

void StatusUi::draw(EmsController &ems) {
  const EmsSnapshot snap = ems.snapshot();
  // Confirm dialog is useless once CA hard-cut is active.
  if (screen_ == Screen::ConfirmLowSoc && snap.outletCut) {
    screen_ = Screen::Main;
    dirty_ = true;
  }
  if (dirty_ || snapChanged(last_, snap)) {
    render(snap);
  }
}
