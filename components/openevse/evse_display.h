#pragma once
// Self-contained EVSE display renderer.
// No dependency on the OpenEVSE component — takes raw values only.
// Each "theme" is a Theme struct; swap the struct to swap the look.

#include "TFT_eSPI.h"

namespace evse_display {

// --- RGB565 helper ---
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// --- Theme ---
struct Theme {
  uint16_t bg;
  uint16_t panel;
  uint16_t border;
  uint16_t text;
  uint16_t label;
  uint16_t divider;
  uint16_t sess_bg;
  uint16_t sess_text;
};

static constexpr Theme DARK = {
  .bg      = 0x0000,
  .panel   = rgb(16, 16, 28),
  .border  = rgb(60, 130, 180),
  .text    = rgb(230, 230, 240),
  .label   = rgb(140, 140, 150),
  .divider = rgb(40, 40, 55),
  .sess_bg = rgb(22, 25, 40),
  .sess_text = rgb(0, 228, 120),
};

static constexpr Theme LIGHT = {
  .bg      = rgb(248, 250, 255),
  .panel   = rgb(255, 255, 255),
  .border  = rgb(50, 120, 170),
  .text    = rgb(25, 25, 35),
  .label   = rgb(90, 95, 110),
  .divider = rgb(200, 205, 215),
  .sess_bg = rgb(232, 236, 245),
  .sess_text = rgb(0, 140, 70),
};

// --- State colors ---
static constexpr uint16_t SC_GREEN  = rgb(0, 228, 120);
static constexpr uint16_t SC_CYAN   = rgb(0, 190, 220);
static constexpr uint16_t SC_RED    = rgb(240, 60, 60);
static constexpr uint16_t SC_ORANGE = rgb(240, 160, 40);
static constexpr uint16_t SC_BLUE   = rgb(80, 140, 220);
static constexpr uint16_t SC_GRAY   = rgb(140, 140, 150);
static constexpr uint16_t SC_DGRAY  = rgb(90, 90, 100);

// --- EVSE states ---
enum EvseState : uint8_t {
  STATE_STARTING          = 0x00,
  STATE_NOT_CONNECTED     = 0x01,
  STATE_CONNECTED         = 0x02,
  STATE_CHARGING          = 0x03,
  STATE_VENT_REQUIRED     = 0x04,
  STATE_DIODE_CHECK       = 0x05,
  STATE_GFI_FAULT         = 0x06,
  STATE_NO_GROUND         = 0x07,
  STATE_STUCK_RELAY       = 0x08,
  STATE_GFI_SELF_TEST     = 0x09,
  STATE_OVER_TEMPERATURE  = 0x0A,
  STATE_OVER_CURRENT      = 0x0B,
  STATE_SLEEPING          = 0xFE,
  STATE_DISABLED          = 0xFF,
};

// --- Nerd Font icons (UTF-8) ---
#define ICON_BOLT       "\xEF\x83\xA7"
#define ICON_PLUG       "\xEF\x87\xA6"
#define ICON_REFRESH    "\xEF\x80\xA1"
#define ICON_WARN       "\xEF\x81\xB1"
#define ICON_THERMO     "\xEF\x8B\x87"
#define ICON_MOON       "\xEF\x86\x86"
#define ICON_BAN        "\xEF\x81\x9E"
#define ICON_CLOCK      "\xEF\x80\x97"
#define ICON_BATTERY    "\xEF\x89\x80"

// --- LED state (for NeoPixel or RGB LED integration) ---
enum LedAnim : uint8_t {
  LED_OFF,
  LED_SOLID,
  LED_BREATHE,
  LED_BLINK_SLOW,   // ~1Hz
  LED_BLINK_FAST,    // ~3Hz
};

struct LedColor { uint8_t r, g, b; };

struct LedState {
  uint8_t r, g, b;
  LedAnim anim;
  uint16_t period_ms;  // full cycle time
};

// --- Display renderer ---
class EvseDisplay {
 public:
  static constexpr int16_t SCREENSHOT_BAND_H = 64;

  EvseDisplay(TFT_eSPI *tft, int16_t w, int16_t h);

  void set_fonts(const uint8_t *big, const uint8_t *medium, const uint8_t *small);
  void begin(bool dark = true);

  // Main update — call this with current sensor values.
  // Returns render time in microseconds.
  uint32_t update(EvseState state, float setpoint_a, float current_a,
                  float voltage_v, float power_kw, float session_kwh,
                  uint32_t elapsed_s, bool dark = true);

  // Render one horizontal band into a sprite for screenshot capture.
  // Sprite is created/deleted internally. Returns false on OOM.
  // Caller reads sprite.getPointer() for raw RGB565 pixel data.
  bool render_band(TFT_eSprite &sprite, int16_t band_y,
                   EvseState state, float setpoint_a, float current_a,
                   float voltage_v, float power_kw, float session_kwh,
                   uint32_t elapsed_s, bool dark);

  // State helpers — public so callers can read LED color, text, etc.
  static const char *state_text(EvseState s);
  static const char *state_icon(EvseState s);
  static uint16_t    state_color(EvseState s);
  static LedColor    state_led(EvseState s);
  static LedState    compute_led(EvseState state, bool evse_enabled, bool vehicle_connected, bool charging);

 private:
  TFT_eSPI *tft_;
  int16_t w_, h_;
  const Theme *theme_{&DARK};
  bool last_dark_{true};
  bool begun_{false};

  const uint8_t *font_big_{nullptr};
  const uint8_t *font_medium_{nullptr};
  const uint8_t *font_small_{nullptr};

  // Scaled layout (computed from w_/h_ in constructor)
  int16_t pad_;
  int16_t status_y_;
  int16_t panel_y_, panel_h_, panel_r_, panel_cx_, panel_cy_;
  int16_t stats_line_y_, stats_label_y_, stats_value_y_;
  int16_t col_w_, col1_x_, col2_x_, col3_x_, div1_x_, div2_x_;
  int16_t sess_y_, sess_h_, sess_r_;
  int16_t icon_slot_;  // width reserved for status icon

  void compute_layout();
  void offset_layout_(int16_t dy);
  void draw_frame();
  void draw_static_labels();
  void draw_dynamic(EvseState state, float setpoint_a, float current_a,
                    float voltage_v, float power_kw, float session_kwh,
                    uint32_t elapsed_s);
};

}  // namespace evse_display
