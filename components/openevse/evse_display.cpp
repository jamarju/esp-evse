// Self-contained EVSE display renderer — dark/light theme.
// No OpenEVSE dependencies; takes raw values only.

#include "evse_display.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "esphome/core/log.h"
static const char *DTAG = "evse_display";

namespace evse_display {

// --- State helpers ---

const char *EvseDisplay::state_text(EvseState s) {
  switch (s) {
    case STATE_STARTING:         return "STARTING";
    case STATE_NOT_CONNECTED:    return "NOT CONNECTED";
    case STATE_CONNECTED:        return "CONNECTED";
    case STATE_CHARGING:         return "CHARGING";
    case STATE_VENT_REQUIRED:    return "VENT REQUIRED";
    case STATE_DIODE_CHECK:      return "DIODE CHECK";
    case STATE_GFI_FAULT:        return "GFI FAULT";
    case STATE_NO_GROUND:        return "NO GROUND";
    case STATE_STUCK_RELAY:      return "STUCK RELAY";
    case STATE_GFI_SELF_TEST:    return "GFI TEST FAIL";
    case STATE_OVER_TEMPERATURE: return "OVER TEMP";
    case STATE_OVER_CURRENT:     return "OVER CURRENT";
    case STATE_SLEEPING:         return "SLEEPING";
    case STATE_DISABLED:         return "DISABLED";
    default:                     return "UNKNOWN";
  }
}

const char *EvseDisplay::state_icon(EvseState s) {
  switch (s) {
    case STATE_STARTING:         return ICON_REFRESH;
    case STATE_NOT_CONNECTED:    return ICON_PLUG;
    case STATE_CONNECTED:        return ICON_PLUG;
    case STATE_CHARGING:         return ICON_BOLT;
    case STATE_OVER_TEMPERATURE: return ICON_THERMO;
    case STATE_SLEEPING:         return ICON_MOON;
    case STATE_DISABLED:         return ICON_BAN;
    default:                     return ICON_WARN;
  }
}

uint16_t EvseDisplay::state_color(EvseState s) {
  switch (s) {
    case STATE_STARTING:         return SC_BLUE;
    case STATE_NOT_CONNECTED:    return SC_GRAY;
    case STATE_CONNECTED:        return SC_CYAN;
    case STATE_CHARGING:         return SC_GREEN;
    case STATE_OVER_TEMPERATURE: return SC_ORANGE;
    case STATE_OVER_CURRENT:     return SC_ORANGE;
    case STATE_SLEEPING:         return SC_BLUE;
    case STATE_DISABLED:         return SC_DGRAY;
    default:                     return SC_RED;
  }
}

LedColor EvseDisplay::state_led(EvseState s) {
  switch (s) {
    case STATE_STARTING:         return {0, 0, 180};
    case STATE_NOT_CONNECTED:    return {15, 15, 15};
    case STATE_CONNECTED:        return {0, 200, 200};
    case STATE_CHARGING:         return {0, 255, 0};
    case STATE_OVER_TEMPERATURE: return {255, 80, 0};
    case STATE_OVER_CURRENT:     return {255, 80, 0};
    case STATE_SLEEPING:         return {0, 0, 40};
    case STATE_DISABLED:         return {0, 0, 0};
    default:                     return {255, 0, 0};
  }
}

LedState EvseDisplay::compute_led(EvseState state, bool evse_enabled,
                                  bool vehicle_connected, bool charging) {
  // Error states take priority
  switch (state) {
    case STATE_VENT_REQUIRED:
    case STATE_DIODE_CHECK:
    case STATE_GFI_FAULT:
    case STATE_NO_GROUND:
    case STATE_STUCK_RELAY:
    case STATE_GFI_SELF_TEST:
      return {255, 0, 0, LED_BLINK_FAST, 333};        // Red fast blink
    case STATE_OVER_TEMPERATURE:
    case STATE_OVER_CURRENT:
      return {255, 80, 0, LED_BLINK_SLOW, 1000};      // Orange slow blink
    case STATE_STARTING:
      return {0, 0, 200, LED_BREATHE, 1500};           // Blue fast breathe
    case STATE_SLEEPING:
      return {0, 0, 80, LED_BREATHE, 6000};            // Blue very slow breathe
    case STATE_DISABLED:
      break;  // fall through to connected/enabled logic below
    default:
      break;
  }

  // Normal operation — combine state flags
  if (vehicle_connected && charging) {
    return {0, 255, 0, LED_BREATHE, 3000};             // Green breathing
  }
  if (vehicle_connected && !evse_enabled) {
    return {0, 0, 150, LED_SOLID, 0};                  // Blue solid dim
  }
  if (vehicle_connected && evse_enabled && !charging) {
    return {0, 200, 0, LED_SOLID, 0};                  // Green solid (charge done)
  }
  if (!vehicle_connected && !evse_enabled) {
    return {0, 0, 0, LED_OFF, 0};                      // Off
  }
  if (!vehicle_connected && evse_enabled) {
    return {0, 180, 180, LED_BREATHE, 4000};           // Cyan slow breathing
  }

  return {0, 0, 0, LED_OFF, 0};
}

// --- Constructor & layout ---

EvseDisplay::EvseDisplay(TFT_eSPI *tft, int16_t w, int16_t h)
    : tft_(tft), w_(w), h_(h) {
  compute_layout();
}

void EvseDisplay::set_fonts(const uint8_t *big, const uint8_t *medium,
                            const uint8_t *small) {
  font_big_ = big;
  font_medium_ = medium;
  font_small_ = small;
}

void EvseDisplay::compute_layout() {
  // All positions are proportional to screen size.
  // Reference design: 320x240. Scale from there.
  float sx = w_ / 320.0f;
  float sy = h_ / 240.0f;

  pad_         = (int16_t)(6 * sx);
  status_y_    = (int16_t)(4 * sy);
  icon_slot_   = (int16_t)(22 * sx);

  panel_y_     = (int16_t)(28 * sy);
  panel_h_     = (int16_t)(118 * sy);
  panel_r_     = (int16_t)(8 * sx);
  panel_cx_    = w_ / 2;
  panel_cy_    = panel_y_ + panel_h_ / 2;

  stats_line_y_  = panel_y_ + panel_h_ + (int16_t)(4 * sy);
  stats_label_y_ = stats_line_y_ + (int16_t)(3 * sy);
  stats_value_y_ = stats_label_y_ + (int16_t)(20 * sy);

  col_w_  = (w_ - 2 * pad_) / 3;
  col1_x_ = pad_ + (int16_t)(2 * sx);
  col2_x_ = pad_ + col_w_ + (int16_t)(4 * sx);
  col3_x_ = pad_ + 2 * col_w_ + (int16_t)(6 * sx);
  div1_x_ = pad_ + col_w_;
  div2_x_ = pad_ + 2 * col_w_ + (int16_t)(2 * sx);

  sess_h_  = (int16_t)(32 * sy);
  sess_y_  = h_ - sess_h_ - (int16_t)(2 * sy);
  sess_r_  = (int16_t)(6 * sx);
}

void EvseDisplay::offset_layout_(int16_t dy) {
  status_y_      += dy;
  panel_y_       += dy;
  panel_cy_      += dy;
  stats_line_y_  += dy;
  stats_label_y_ += dy;
  stats_value_y_ += dy;
  sess_y_        += dy;
}

// --- Public API ---

void EvseDisplay::begin(bool dark) {
  theme_ = dark ? &DARK : &LIGHT;
  last_dark_ = dark;
  tft_->fillScreen(theme_->bg);
  draw_frame();
  draw_static_labels();
  begun_ = true;
}

uint32_t EvseDisplay::update(EvseState state, float setpoint_a, float current_a,
                             float voltage_v, float power_kw, float session_kwh,
                             uint32_t elapsed_s, MainPanelMode panel_mode,
                             bool dark) {
  bool theme_changed = (dark != last_dark_);
  last_dark_ = dark;
  theme_ = dark ? &DARK : &LIGHT;

  if (theme_changed || !begun_) {
    tft_->fillScreen(theme_->bg);
    draw_frame();
    draw_static_labels();
    begun_ = true;
  }

  uint32_t t0 = micros();
  draw_dynamic(state, setpoint_a, current_a, voltage_v, power_kw,
               session_kwh, elapsed_s, panel_mode);
  return micros() - t0;
}

// --- Screenshot band rendering ---

bool EvseDisplay::render_band(TFT_eSprite &sprite, int16_t band_y,
                              EvseState state, float setpoint_a, float current_a,
                              float voltage_v, float power_kw, float session_kwh,
                              uint32_t elapsed_s, MainPanelMode panel_mode,
                              bool dark) {
  int16_t band_h = std::min((int16_t)SCREENSHOT_BAND_H, (int16_t)(h_ - band_y));
  if (band_h <= 0) return false;

  ESP_LOGW(DTAG, "band y=%d h=%d: createSprite(%d,%d)", band_y, band_h, w_, band_h);
  if (!sprite.createSprite(w_, band_h)) {
    ESP_LOGE(DTAG, "createSprite failed");
    return false;
  }
  ESP_LOGW(DTAG, "band y=%d: sprite OK, swapping target", band_y);

  // Save and swap render target
  TFT_eSPI *orig_tft = tft_;
  const Theme *orig_theme = theme_;
  tft_ = &sprite;
  theme_ = dark ? &DARK : &LIGHT;

  // Offset layout so this band's y-range maps to [0, band_h)
  offset_layout_(-band_y);

  ESP_LOGW(DTAG, "band y=%d: fillSprite", band_y);
  sprite.fillSprite(theme_->bg);

  ESP_LOGW(DTAG, "band y=%d: draw_frame", band_y);
  draw_frame();

  ESP_LOGW(DTAG, "band y=%d: draw_static_labels", band_y);
  draw_static_labels();

  ESP_LOGW(DTAG, "band y=%d: draw_dynamic", band_y);
  draw_dynamic(state, setpoint_a, current_a, voltage_v, power_kw,
               session_kwh, elapsed_s, panel_mode);

  ESP_LOGW(DTAG, "band y=%d: done, restoring", band_y);

  // Restore
  offset_layout_(band_y);
  tft_ = orig_tft;
  theme_ = orig_theme;

  return true;
}

// --- Drawing ---

void EvseDisplay::draw_frame() {
  const auto &t = *theme_;
  // Main panel with 2px border
  tft_->drawRoundRect(pad_, panel_y_, w_ - 2 * pad_, panel_h_, panel_r_, t.border);
  tft_->drawRoundRect(pad_ + 1, panel_y_ + 1, w_ - 2 * pad_ - 2, panel_h_ - 2,
                     panel_r_ - 1, t.border);
  tft_->fillRoundRect(pad_ + 2, panel_y_ + 2, w_ - 2 * pad_ - 4, panel_h_ - 4,
                     panel_r_ - 2, t.panel);

  // Stats dividers
  tft_->drawFastHLine(0, stats_line_y_, w_, t.divider);
  tft_->drawFastVLine(div1_x_, stats_line_y_ + 2, sess_y_ - stats_line_y_ - 6, t.divider);
  tft_->drawFastVLine(div2_x_, stats_line_y_ + 2, sess_y_ - stats_line_y_ - 6, t.divider);

  // Session bar
  tft_->fillRoundRect(pad_, sess_y_, w_ - 2 * pad_, sess_h_, sess_r_, t.sess_bg);
}

void EvseDisplay::draw_static_labels() {
  const auto &t = *theme_;
  if (!font_small_) return;

  tft_->loadFont(font_small_);
  tft_->setTextDatum(TL_DATUM);
  tft_->setTextPadding(0);

  tft_->setTextColor(t.label, t.bg);
  tft_->drawString("CURRENT", col1_x_, stats_label_y_);
  tft_->drawString("VOLTAGE", col2_x_, stats_label_y_);
  tft_->drawString("POWER", col3_x_, stats_label_y_);

  // Session icon + label
  int16_t sess_text_y = sess_y_ + (sess_h_ - 18) / 2;  // roughly center
  tft_->setTextColor(t.sess_text, t.sess_bg);
  tft_->drawString(ICON_BATTERY, pad_ + 6, sess_text_y);
  tft_->drawString("SESSION", pad_ + (int16_t)(29 * w_ / 320.0f), sess_text_y);

  // Clock icon
  tft_->setTextColor(t.label, t.bg);
  tft_->drawString(ICON_CLOCK, w_ - pad_ - (int16_t)(17 * w_ / 320.0f), status_y_);

  tft_->unloadFont();
}

void EvseDisplay::draw_dynamic(EvseState state, float setpoint_a, float current_a,
                               float voltage_v, float power_kw,
                               float session_kwh, uint32_t elapsed_s,
                               MainPanelMode panel_mode) {
  const auto &t = *theme_;
  char buf[32];
  uint16_t sc = state_color(state);

  // --- Status bar (small font) ---
  if (font_small_) {
    tft_->loadFont(font_small_);

    // State icon
    tft_->setTextDatum(TL_DATUM);
    tft_->setTextPadding(icon_slot_);
    tft_->setTextColor(sc, t.bg);
    tft_->drawString(state_icon(state), pad_, status_y_);

    // State text
    tft_->setTextPadding(w_ / 2 - pad_ - icon_slot_);
    tft_->drawString(state_text(state), pad_ + icon_slot_, status_y_);

    // Elapsed time
    sprintf(buf, "%d:%02d", elapsed_s / 3600, (elapsed_s % 3600) / 60);
    tft_->setTextColor(t.label, t.bg);
    tft_->setTextDatum(TR_DATUM);
    tft_->setTextPadding((int16_t)(60 * w_ / 320.0f));
    tft_->drawString(buf, w_ - pad_ - (int16_t)(21 * w_ / 320.0f), status_y_);

    tft_->unloadFont();
  }

  // --- Stats (medium font) ---
  if (font_medium_) {
    tft_->loadFont(font_medium_);
    tft_->setTextDatum(TL_DATUM);
    tft_->setTextPadding(col_w_ - 4);
    tft_->setTextColor(t.text, t.bg);

    sprintf(buf, "%.1fA", current_a);
    tft_->drawString(buf, col1_x_, stats_value_y_);
    sprintf(buf, "%.0fV", voltage_v);
    tft_->drawString(buf, col2_x_, stats_value_y_);
    sprintf(buf, "%.1fkW", power_kw);
    tft_->drawString(buf, col3_x_, stats_value_y_);

    // Session value
    int16_t sess_val_y = sess_y_ + (sess_h_ - 22) / 2;
    tft_->setTextDatum(TR_DATUM);
    tft_->setTextPadding(w_ / 2);
    tft_->setTextColor(t.text, t.sess_bg);
    sprintf(buf, "%.2f kWh", session_kwh);
    tft_->drawString(buf, w_ - pad_ - 6, sess_val_y);

    tft_->unloadFont();
  }

  // --- Main panel (big font) ---
  if (font_big_) {
    tft_->loadFont(font_big_);
    tft_->setTextDatum(MC_DATUM);
    tft_->setTextPadding(w_ - 4 * pad_);
    tft_->setTextColor(t.text, t.panel);
    switch (panel_mode) {
      case PANEL_SURPLUS_WAIT:
        tft_->drawString(ICON_MOON, panel_cx_, panel_cy_);
        break;
      case PANEL_VEHICLE_PAUSED:
        tft_->drawString(ICON_BATTERY, panel_cx_, panel_cy_);
        break;
      case PANEL_SETPOINT:
      default:
        sprintf(buf, "%d A", (int)setpoint_a);
        tft_->drawString(buf, panel_cx_, panel_cy_);
        break;
    }
    tft_->unloadFont();
  }
}

}  // namespace evse_display
