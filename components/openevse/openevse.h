#pragma once

#include <queue>
#include <string>
#include <limits>
#include "TFT_eSPI.h"
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "evse_display.h"
#include <NeoPixelBus.h>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include <esp_http_server.h>

#define ECF_L2                       0x0001 // service level 2
#define ECF_DIODE_CHK_DISABLED       0x0002 // diode check disabled
#define ECF_VENT_REQ_DISABLED        0x0004 // vent required check disabled
#define ECF_GND_CHK_DISABLED         0x0008 // ground check disabled
#define ECF_STUCK_RELAY_CHK_DISABLED 0x0010 // stuck relay check disabled
#define ECF_AUTO_SVC_LEVEL_DISABLED  0x0020 // auto detect svc level disabled
#define ECF_GFI_TEST_DISABLED        0x0200 // GFI self-test disabled
#define ECF_TEMP_CHK_DISABLED        0x0400 // temperature monitoring disabled
#define ECF_BUTTON_DISABLED          0x8000 // front button disabled

#define ECVF_AUTOSVCLVL_SKIPPED 0x0001 // auto svc level test skipped during post
#define ECVF_HARD_FAULT         0x0002 // in non-autoresettable fault
#define ECVF_LIMIT_SLEEP        0x0004 // currently sleeping after reaching time/charge limit
#define ECVF_AUTH_LOCKED        0x0008 // locked pending authentication
#define ECVF_TIME_LIMIT         0x0010 // time limit set
#define ECVF_NOGND_TRIPPED      0x0020 // no ground has tripped at least once
#define ECVF_CHARGING_ON        0x0040 // charging relay is closed
#define ECVF_GFI_TRIPPED        0x0080 // gfi has tripped at least once since boot
#define ECVF_EV_CONNECTED       0x0100 // EV connected - valid only when pilot not N12
#define ECVF_SESSION_ENDED      0x0200 // used for charging session time calc
#define ECVF_EV_CONNECTED_PREV  0x0400 // prev EV connected flag
#define ECVF_UI_IN_MENU         0x0800 // onboard UI currently in a menu
#define ECVF_TIMER_ON           0x1000 // delay timer enabled
#define ECVF_CHARGE_LIMIT       0x2000
#define ECVF_BOOT_LOCK          0x4000 // locked at boot
#define ECVF_MENNEKES_MANUAL    0x8000 // Mennekes lock manual mode

namespace esphome {
namespace openevse {

class OpenEVSE : public Component {
 public:
  // Define startup phases
  enum class StartupPhase {
    INIT,    // Initial phase - waiting for version response
    SETUP,   // Setup phase - one-time configuration
    RUN      // Normal operation phase
  };

  void setup() override;
  void loop() override;
  void update();
  void set_update_interval(uint32_t update_interval) { this->update_interval_ = update_interval; }
  void set_uart_parent(uart::UARTComponent *parent) { this->uart_parent_ = parent; }
  void set_dark_mode(bool dark);
  bool get_dark_mode() const { return this->dark_mode_; }
  void set_backlight_brightness(float pct);
  void set_backlight_control(number::Number *n) { this->backlight_control_ = n; }

  // Sensors
  void set_evse_state_sensor(text_sensor::TextSensor *s) { this->evse_state_sensor_ = s; }
  void set_pilot_state_sensor(text_sensor::TextSensor *s) { this->pilot_state_sensor_ = s; }
  void set_elapsed_sensor(sensor::Sensor *s) { this->elapsed_sensor_ = s; }
  void set_current_capacity_sensor(sensor::Sensor *s) { this->current_capacity_sensor_ = s; }
  void set_charging_current_sensor(sensor::Sensor *s) { this->charging_current_sensor_ = s; }
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
  void set_energy_usage_sensor(sensor::Sensor *s) { this->energy_usage_sensor_ = s; }
  void set_firmware_version_sensor(text_sensor::TextSensor *s) { this->firmware_version_sensor_ = s; }

  // Controls (wired from template entities via Python codegen)
  void set_current_capacity_control(number::Number *n) { this->current_capacity_control_ = n; }
  void set_max_conf_capacity_control(number::Number *n) { this->max_conf_capacity_control_ = n; }
  void set_max_hw_capacity_control(number::Number *n) { this->max_hw_capacity_control_ = n; }
  void set_enable_switch(switch_::Switch *s) { this->enable_switch_ = s; }
  void set_current_scale_factor_control(number::Number *n) { this->current_scale_factor_control_ = n; }
  void set_current_offset_control(number::Number *n) { this->current_offset_control_ = n; }
  void set_voltage_control(number::Number *n) { this->voltage_control_ = n; }
  void set_service_level_select(select::Select *s) { this->service_level_select_ = s; }
  void set_front_button_switch(switch_::Switch *s) { this->front_button_switch_ = s; }
  void set_diode_check_switch(switch_::Switch *s) { this->diode_check_switch_ = s; }
  void set_vent_required_switch(switch_::Switch *s) { this->vent_required_switch_ = s; }
  void set_ground_check_switch(switch_::Switch *s) { this->ground_check_switch_ = s; }
  void set_stuck_relay_check_switch(switch_::Switch *s) { this->stuck_relay_check_switch_ = s; }
  void set_gfi_self_test_switch(switch_::Switch *s) { this->gfi_self_test_switch_ = s; }
  void set_temperature_monitoring_switch(switch_::Switch *s) { this->temperature_monitoring_switch_ = s; }

  // Control methods
  void set_current_capacity(float amps);
  void set_max_conf_capacity(float amps);
  void set_max_hw_capacity(float amps);
  void enable_evse(bool enable);
  void set_current_scale_factor(float scale_factor);
  void set_current_offset(float offset);
  void set_voltage(float volts);
  void set_service_level(const std::string &service_level);
  void set_feature_enabled(char feature_id, bool enabled);

  // Query methods
  void get_current_capacity();
  void get_state();
  void get_ammeter_settings();
  void get_charging_current_voltage();
  void get_temperature();
  void get_energy_usage();
  void get_settings();
  void update_display();
  void get_version();

  // Memory debugging
  void log_memory_usage(const char* location);

  // SPI read diagnostic
  void debug_spi_read();

  // Screenshot HTTP endpoint (port 8080)
  static esp_err_t handle_screenshot_static(httpd_req_t *req);
  esp_err_t handle_screenshot(httpd_req_t *req);

  // Binary sensors
  void set_vehicle_connected_sensor(binary_sensor::BinarySensor *s) { this->vehicle_connected_sensor_ = s; }
  void set_charging_sensor(binary_sensor::BinarySensor *s) { this->charging_sensor_ = s; }

 protected:
  // Command types for tracking in-flight commands
  enum class CommandType {
    NONE,
    GET_CURRENT_CAPACITY,  // GC
    GET_STATE,             // GS
    ENABLE_EVSE,           // FE
    DISABLE_EVSE           // FD
  };
  
  void queue_command_(const std::string &command);
  void handle_response_(const std::string &response);
  std::string calculate_checksum_(const std::string &data);
  std::string parse_state_text_(uint8_t state);
  int encode_ammeter_offset_for_rapi_(int offset);
  std::string parse_service_level_(uint16_t flags);
  void update_feature_switches_(uint16_t flags);
  bool read_line_();
  std::string parse_response_();
  text_sensor::TextSensor *evse_state_sensor_{nullptr};
  text_sensor::TextSensor *pilot_state_sensor_{nullptr};
  sensor::Sensor *elapsed_sensor_{nullptr};
  sensor::Sensor *current_capacity_sensor_{nullptr};
  number::Number *current_capacity_control_{nullptr};
  number::Number *max_conf_capacity_control_{nullptr};
  number::Number *max_hw_capacity_control_{nullptr};
  switch_::Switch *enable_switch_{nullptr};
  number::Number *current_scale_factor_control_{nullptr};
  number::Number *current_offset_control_{nullptr};
  sensor::Sensor *charging_current_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  number::Number *voltage_control_{nullptr};
  sensor::Sensor *energy_usage_sensor_{nullptr};
  select::Select *service_level_select_{nullptr};
  switch_::Switch *front_button_switch_{nullptr};
  switch_::Switch *diode_check_switch_{nullptr};
  switch_::Switch *vent_required_switch_{nullptr};
  switch_::Switch *ground_check_switch_{nullptr};
  switch_::Switch *stuck_relay_check_switch_{nullptr};
  switch_::Switch *gfi_self_test_switch_{nullptr};
  switch_::Switch *temperature_monitoring_switch_{nullptr};
  
  uint32_t update_interval_{30000};  // Default update interval: 30 seconds
  uint32_t last_update_{0};
  uint32_t last_heartbeat_{0};      // Track last heartbeat time
  uint32_t heartbeat_interval_{10000}; // Heartbeat interval: 10 seconds
  std::string command_;
  std::string response_;
  uart::UARTComponent *uart_parent_{nullptr};
  
  void handle_async_notification_(const std::string &notification);
  
  // Track the currently in-flight command
  CommandType in_flight_command_{CommandType::NONE};
  
  // Add to the protected section of the OpenEVSE class
  std::queue<std::string> command_queue_;
  static const size_t MAX_QUEUE_SIZE = 16;
  
  void send_next_command_in_queue_();
  float energy_usage_kwh_{std::numeric_limits<float>::quiet_NaN()};
  TFT_eSPI tft;
  evse_display::EvseDisplay *display_{nullptr};
  number::Number *backlight_control_{nullptr};
  static constexpr uint8_t BACKLIGHT_PIN = 27;
  static constexpr uint8_t LED_COUNT = 4;
  float backlight_brightness_{1.0f};  // 0.0-1.0, set from HA
  NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2811Method> led_strip_{LED_COUNT, 26};
  evse_display::LedState led_state_{0, 0, 0, evse_display::LED_OFF, 0};
  void update_leds_();

  bool screenshot_registered_{false};
  volatile bool screenshot_in_progress_{false};
  bool dark_mode_{true};
  uint8_t evse_state_code_{0x00};
  uint8_t last_evse_state_{0x00};
  uint32_t last_command_time_{0};
  
  // Track startup phase
  StartupPhase startup_phase_{StartupPhase::INIT};
  std::string firmware_version_;
  text_sensor::TextSensor *firmware_version_sensor_{nullptr};

  // Binary sensors
  binary_sensor::BinarySensor *vehicle_connected_sensor_{nullptr};
  binary_sensor::BinarySensor *charging_sensor_{nullptr};
  
  // Helper method to update state sensors
  void update_state_sensors_(uint8_t evse_state, uint8_t pilot_state, uint16_t vflags);
};

}  // namespace openevse
}  // namespace esphome
