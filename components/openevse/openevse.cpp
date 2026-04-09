#include <algorithm>
#include <cctype>
#include <string>
#include <sstream>
#include <cmath>
#include <new>
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "openevse.h"
#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_WEBSERVER_IDF
#include "esphome/components/web_server_idf/web_server_idf.h"
#endif
#include <sys/socket.h>
#include "font_big.h"
#include "font_medium.h"
#include "font_small.h"

namespace esphome {
namespace openevse {

static const char *TAG = "openevse";
static const uint32_t COMMAND_TIMEOUT = 2000;

std::string trim_copy(std::string value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
  if (first >= last) {
    return {};
  }

  return std::string(first, last);
}

// OpenEVSE implementation
void OpenEVSE::setup() {
  // Reserve space for response
  this->response_.reserve(128);
  this->pending_raw_command_.reserve(64);
  this->controls_ready_ = false;
  this->publish_rapi_status_("idle");
  if (this->raw_command_input_ != nullptr) {
    this->raw_command_input_->publish_state("");
  }

  // Log initial memory usage information
  // We'll use the debug component instead
  //this->log_memory_usage("initial");
  
  // Initialize UART
  if (this->uart_parent_ == nullptr) {
    ESP_LOGE(TAG, "UART parent is null");
    this->mark_failed();
    return;
  }
  
  // Initialize LED strip
  this->led_strip_.Begin();
  this->led_strip_.Show();

  // Initialize TFT display with new renderer
  this->tft.init();
  this->tft.setRotation(1);

  this->display_ = new (std::nothrow) evse_display::EvseDisplay(&this->tft, 480, 320);
  if (this->display_) {
    this->display_->set_fonts(font_big, font_medium, font_small);
    this->display_->begin(this->dark_mode_);
    this->display_->update(evse_display::STATE_STARTING, 0, 0, 0, 0, 0, 0, this->dark_mode_);
  } else {
    this->tft.fillScreen(TFT_BLACK);
    this->tft.setTextColor(TFT_WHITE);
    this->tft.setTextFont(1);
    this->tft.drawString("Display OOM!", 10, 10);
  }


  
  // Publish initial backlight brightness
  if (this->backlight_control_) {
    this->backlight_control_->publish_state(100);
  }
  analogWrite(BACKLIGHT_PIN, 255);

  // Set initial startup phase
  this->startup_phase_ = StartupPhase::INIT;
  
  // Schedule first update
  this->last_update_ = millis() - this->update_interval_;

  // Initialize heartbeat timer
  this->last_heartbeat_ = millis();
}

void OpenEVSE::loop() {
  // One-shot: register screenshot endpoint on ESPHome's web server
  if (!this->screenshot_registered_) {
    ESP_LOGW(TAG, "Starting screenshot server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;  // different from ESPHome's httpd (32768)
    config.stack_size = 8192;  // VLW font rendering needs extra stack
    config.max_open_sockets = 2;
    config.lru_purge_enable = false;
    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &config);
    if (err == ESP_OK) {
      httpd_uri_t uri = {};
      uri.uri = "/screenshot.bmp";
      uri.method = HTTP_GET;
      uri.handler = &OpenEVSE::handle_screenshot_static;
      uri.user_ctx = this;
      httpd_register_uri_handler(server, &uri);
      this->screenshot_registered_ = true;
      ESP_LOGI(TAG, "Screenshot server on port 8080");
    } else {
      ESP_LOGE(TAG, "Screenshot httpd_start failed: %d", err);
      this->screenshot_registered_ = true;  // don't retry
    }
  }

  uint32_t now = millis();

  // Clear queue if no responses for too long
  if (!this->command_.empty() && now - this->last_command_time_ > COMMAND_TIMEOUT) {
    ESP_LOGW(TAG, "Command timed out after %d ms, clearing command queue", COMMAND_TIMEOUT);
    if (this->current_request_.capture_response) {
      if (this->rapi_response_sensor_ != nullptr) {
        this->rapi_response_sensor_->publish_state("Timeout");
      }
      this->publish_rapi_status_("timeout");
    }
    std::deque<QueuedCommand> empty;
    std::swap(this->command_queue_, empty);
    this->command_.clear();
    this->current_request_ = {};
  }

  // Check for incoming messages
  while (this->read_line_()) {
    std::string response = this->parse_response_();
    if (response.empty()) {
      continue;
    }
    
    // Check if this is an asynchronous notification (starts with $A)
    if (response.length() > 2 && response[0] == '$' && response[1] == 'A') {
      // Handle asynchronous notification
      this->handle_async_notification_(response);
    } else {
      // Handle the response to a command in progress
      this->handle_response_(response);
      // Refresh display after each response (values may have changed)
      this->update_display();
      // Clear current command and process the next one in the queue
      this->command_.clear();
      this->current_request_ = {};
      this->send_next_command_in_queue_();
    }
  }

  // Check if it's time to send a heartbeat
  if (now - this->last_heartbeat_ >= this->heartbeat_interval_ && this->startup_phase_ == StartupPhase::RUN) {
    this->last_heartbeat_ = now;
    this->queue_command_("SY");
  }

  // Check if it's time to update
  if (now - this->last_update_ >= this->update_interval_) {
    this->last_update_ = now;
    this->update();
  }

  // Update LED animation every loop iteration.
  this->update_leds_();
}

void OpenEVSE::update_leds_() {
  auto &s = this->led_state_;

  float brightness = 0.0f;
  switch (s.anim) {
    case evse_display::LED_OFF:
      brightness = 0.0f;
      break;
    case evse_display::LED_SOLID:
      brightness = 1.0f;
      break;
    case evse_display::LED_BREATHE: {
      // Smooth sine wave: 0 → 1 → 0
      float phase = (millis() % s.period_ms) / (float)s.period_ms;
      brightness = (sinf(phase * 2.0f * M_PI - M_PI / 2.0f) + 1.0f) * 0.5f;
      // Square it for more natural perceived brightness curve
      brightness *= brightness;
      break;
    }
    case evse_display::LED_BLINK_SLOW:
    case evse_display::LED_BLINK_FAST: {
      // Square wave: on for first half, off for second half
      brightness = ((millis() % s.period_ms) < (uint32_t)(s.period_ms / 2)) ? 1.0f : 0.0f;
      break;
    }
  }

  uint8_t r = (uint8_t)(s.r * brightness);
  uint8_t g = (uint8_t)(s.g * brightness);
  uint8_t b = (uint8_t)(s.b * brightness);

  RgbColor color(r, g, b);
  for (int i = 0; i < LED_COUNT; i++) {
    this->led_strip_.SetPixelColor(i, color);
  }
  this->led_strip_.Show();
}

std::string OpenEVSE::parse_response_() {
  std::string response = this->response_;
  this->response_.clear();
  size_t pos = response.find('^');
  if (pos == std::string::npos) {
    ESP_LOGE(TAG, "Checksum not found for message: %s", response.c_str());
    return std::string();
  }
  std::string payload = response.substr(0, pos);
  std::string checksum = response.substr(pos + 1);

  // Calculate checksum of payload
  uint8_t calculated_checksum = 0;
  for (char c : payload) {
    calculated_checksum ^= c;
  }

  // Convert checksum to integer
  uint8_t checksum_int = strtol(checksum.c_str(), nullptr, 16);

  // Compare calculated checksum with received checksum
  if (calculated_checksum == checksum_int) {
    return payload;
  } else {
    ESP_LOGE(TAG, "Checksum verification failed for message: %s", response.c_str());
    return std::string();
  }
}

bool OpenEVSE::read_line_() {
  uint8_t c;

  while (this->uart_parent_->available()) {
    this->uart_parent_->read_byte(&c);
    if (c == '\r' || c == '\n') {
      if (this->response_.empty()) {
        continue;
      } else {  
        ESP_LOGD(TAG, "<<< %s", this->response_.c_str());
        return true;
      }
    } else {
      if (this->response_.length() < 128) {
        this->response_.push_back(c);
      }
    }
  }

  return false;
}

void OpenEVSE::update() {
  // State machine for the three startup phases
  switch (this->startup_phase_) {
    case StartupPhase::INIT:
      // Initial phase - keep querying version until we get a valid response
      ESP_LOGI(TAG, "Startup phase: INIT - Querying version");
      this->get_version();
      break;
      
    case StartupPhase::SETUP:
      // Setup phase - one-time configuration
      ESP_LOGI(TAG, "Startup phase: SETUP - Performing one-time configuration");
      
      // Initialize supervision
      this->queue_command_("SY 60 6");
      
      // Get one-time configuration settings
      this->get_ammeter_settings();  // $GA: Get ammeter settings
      this->get_settings();          // $GE: Get persisted settings and flags
      this->get_current_capacity();  // $GC: Sync the actual pilot/current setpoint after an ESP32-only reboot

      // Mark setup phase as complete and move to RUN phase
      this->startup_phase_ = StartupPhase::RUN;
      ESP_LOGI(TAG, "Moving to RUN phase");
      break;
      
    case StartupPhase::RUN:
      // Get other status updates
      this->get_state(); // $GS: Get EVSE state
      this->get_settings(); // $GE: Get persisted settings and service-level flags
      this->get_current_capacity(); // $GC: Get current capacity
      this->get_charging_current_voltage(); // $GG: Get charging current and voltage
      this->get_temperature(); // $GP: Get temperature
      this->get_energy_usage(); // $GU: Get energy usage
      
      // Update display with real-time data
      this->update_display();
      break;
  }
}

// Send command and add to queue if needed
bool OpenEVSE::queue_command_(const std::string &command, bool capture_response, bool priority) {
  // Add command to queue
  if (this->command_queue_.size() >= MAX_QUEUE_SIZE) {
    ESP_LOGW(TAG, "Command queue full, dropping command: %s", command.c_str());
    return false;
  }

  QueuedCommand queued_command{command, capture_response};
  if (priority) {
    this->command_queue_.push_front(queued_command);
  } else {
    this->command_queue_.push_back(queued_command);
  }
  ESP_LOGD(TAG, "Command queued: %s (queue size: %d)", command.c_str(), this->command_queue_.size());

  // If no command is in progress, send the next command in the queue
  if (this->command_.empty()) {
    this->send_next_command_in_queue_();
  }

  return true;
}

// Send the next command in the queue
void OpenEVSE::send_next_command_in_queue_() {
  if (this->command_queue_.empty()) {
    return;  // No commands to process
  }
  
  if (!this->command_.empty()) {
    ESP_LOGW(TAG, "Command already in progress: %s", this->command_.c_str());
    return;  // Command already in progress
  }
  
  // Get the next command from the queue
  this->current_request_ = this->command_queue_.front();
  this->command_queue_.pop_front();
  
  // Format: $command^checksum
  std::string payload = "$" + this->current_request_.command;
  std::string checksum = this->calculate_checksum_(payload);
  this->command_ = payload + "^" + checksum + "\r";

  // Send command  
  ESP_LOGD(TAG, ">>> %s", this->command_.c_str());
  this->uart_parent_->write_str(this->command_.c_str());
  this->last_command_time_ = millis();
  if (this->current_request_.capture_response) {
    this->publish_rapi_status_("sent");
  }
}

std::string OpenEVSE::calculate_checksum_(const std::string &data) {
  uint8_t checksum = 0;
  for (char c : data) {
    checksum ^= c;
  }
  
  char checksum_str[3];
  sprintf(checksum_str, "%02X", checksum);
  return std::string(checksum_str);
}

std::string OpenEVSE::parse_state_text_(uint8_t state) {
  switch (state) {
    case 0x00: return "Starting";
    case 0x01: return "Not Connected";
    case 0x02: return "Connected";
    case 0x03: return "Charging";
    case 0x04: return "Vent Required";
    case 0x05: return "Diode Check Failed";
    case 0x06: return "GFI Fault";
    case 0x07: return "No Earth Ground";
    case 0x08: return "Stuck Relay";
    case 0x09: return "GFI Self Test Failed";
    case 0x0A: return "Over Temperature";
    case 0x0B: return "Over Current";
    case 0xFE: return "Sleeping";
    case 0xFF: return "Disabled";
    default: return "Unknown";
  }
}

int OpenEVSE::encode_ammeter_offset_for_rapi_(int offset) {
  if (offset < 0) {
    // Convert negative values to 2's complement representation (uint16_t range).
    // This works around a bug in the OpenEVSE controller firmware: its AVR RAPI
    // handler parses SA arguments as uint32_t before storing the offset as int16_t.
    uint16_t encoded = static_cast<uint16_t>(offset);
    ESP_LOGD(TAG, "Converting negative offset %d to 2's complement: %u", offset, encoded);
    return encoded;
  }

  return offset;
}

std::string OpenEVSE::parse_service_level_(uint16_t flags) {
  if ((flags & ECF_AUTO_SVC_LEVEL_DISABLED) == 0) {
    return "Auto";
  }

  return (flags & ECF_L2) ? "L2" : "L1";
}

void OpenEVSE::update_feature_switches_(uint16_t flags) {
  if (this->front_button_switch_ != nullptr) {
    this->front_button_switch_->publish_state((flags & ECF_BUTTON_DISABLED) == 0);
  }
  if (this->diode_check_switch_ != nullptr) {
    this->diode_check_switch_->publish_state((flags & ECF_DIODE_CHK_DISABLED) == 0);
  }
  if (this->vent_required_switch_ != nullptr) {
    this->vent_required_switch_->publish_state((flags & ECF_VENT_REQ_DISABLED) == 0);
  }
  if (this->ground_check_switch_ != nullptr) {
    this->ground_check_switch_->publish_state((flags & ECF_GND_CHK_DISABLED) == 0);
  }
  if (this->stuck_relay_check_switch_ != nullptr) {
    this->stuck_relay_check_switch_->publish_state((flags & ECF_STUCK_RELAY_CHK_DISABLED) == 0);
  }
  if (this->gfi_self_test_switch_ != nullptr) {
    this->gfi_self_test_switch_->publish_state((flags & ECF_GFI_TEST_DISABLED) == 0);
  }
  if (this->temperature_monitoring_switch_ != nullptr) {
    this->temperature_monitoring_switch_->publish_state((flags & ECF_TEMP_CHK_DISABLED) == 0);
  }
}

void OpenEVSE::publish_rapi_status_(const std::string &status) {
  if (this->rapi_status_sensor_ != nullptr) {
    this->rapi_status_sensor_->publish_state(status);
  }
}

bool OpenEVSE::control_writes_ready_() const {
  if (this->controls_ready_) {
    return true;
  }

  ESP_LOGD(TAG, "Ignoring control write before initial settings sync");
  return false;
}

std::string OpenEVSE::normalize_raw_command_(const std::string &command) const {
  std::string normalized = trim_copy(command);
  normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
  normalized.erase(std::remove(normalized.begin(), normalized.end(), '\n'), normalized.end());
  normalized = trim_copy(normalized);

  if (!normalized.empty() && normalized.front() == '$') {
    normalized.erase(0, 1);
  }

  size_t checksum_pos = normalized.find('^');
  if (checksum_pos != std::string::npos) {
    normalized.erase(checksum_pos);
  }

  return trim_copy(normalized);
}

void OpenEVSE::set_raw_command(const std::string &command) {
  this->pending_raw_command_ = this->normalize_raw_command_(command);
}

void OpenEVSE::send_raw_command() {
  const std::string command = this->normalize_raw_command_(this->pending_raw_command_);
  if (command.empty()) {
    if (this->rapi_response_sensor_ != nullptr) {
      this->rapi_response_sensor_->publish_state("Empty command");
    }
    this->publish_rapi_status_("invalid");
    return;
  }

  this->pending_raw_command_ = command;
  if (this->rapi_response_sensor_ != nullptr) {
    this->rapi_response_sensor_->publish_state("");
  }
  this->publish_rapi_status_("queued");

  if (!this->queue_command_(command, true, true)) {
    if (this->rapi_response_sensor_ != nullptr) {
      this->rapi_response_sensor_->publish_state("Queue full");
    }
    this->publish_rapi_status_("queue_full");
  }
}

void OpenEVSE::set_current_capacity(float amps) {
  if (!this->control_writes_ready_()) {
    return;
  }

  int amps_int = std::lround(amps);

  // Format command: SC amps V
  std::string command = "SC " + std::to_string(amps_int) + " V";
  this->queue_command_(command);
}

void OpenEVSE::set_max_conf_capacity(float amps) {
  if (!this->control_writes_ready_()) {
    return;
  }

  int amps_int = std::lround(amps);
  
  // Format command: SC amps
  std::string command = "SC " + std::to_string(amps_int);
  this->queue_command_(command);
}

void OpenEVSE::set_max_hw_capacity(float amps) {
  if (!this->control_writes_ready_()) {
    return;
  }

  int amps_int = std::lround(amps);
  
  // Format command: SC amps M
  std::string command = "SC " + std::to_string(amps_int) + " M";
  this->queue_command_(command);
}

void OpenEVSE::set_current_scale_factor(float scale_factor) {
  if (!this->control_writes_ready_()) {
    return;
  }

  int scale_factor_int = std::lround(scale_factor);
  
  // Get the current offset to include in the command
  float current_offset = 0;
  if (this->current_offset_control_ != nullptr) {
    current_offset = this->current_offset_control_->state;
  }
  int offset_int = this->encode_ammeter_offset_for_rapi_(std::lround(current_offset));
  
  // Format command: SA scale_factor offset
  std::string command = "SA " + std::to_string(scale_factor_int) + " " + std::to_string(offset_int);
  this->queue_command_(command);
}

void OpenEVSE::set_current_offset(float offset) {
  if (!this->control_writes_ready_()) {
    return;
  }

  int offset_int = this->encode_ammeter_offset_for_rapi_(std::lround(offset));
  
  // Get the current scale factor to include in the command
  float scale_factor = 0;
  if (this->current_scale_factor_control_ != nullptr) {
    scale_factor = this->current_scale_factor_control_->state;
  }
  int scale_factor_int = std::lround(scale_factor);
  
  // Format command: SA scale_factor offset
  std::string command = "SA " + std::to_string(scale_factor_int) + " " + std::to_string(offset_int);
  this->queue_command_(command);
}

void OpenEVSE::set_voltage(float volts) {
  if (!this->control_writes_ready_()) {
    return;
  }

  // Convert to integer millivolts
  int mv = std::lround(volts * 1000.0);
  
  // Format command: SV millivolts
  std::string command = "SV " + std::to_string(mv);
  this->queue_command_(command);
  
  // Update the voltage sensor if available
  if (this->voltage_control_ != nullptr) {
    this->voltage_control_->publish_state(volts);
  }
}

void OpenEVSE::set_service_level(const std::string &service_level) {
  if (!this->control_writes_ready_()) {
    return;
  }

  char service_level_code;

  if (service_level == "L1") {
    service_level_code = '1';
  } else if (service_level == "L2") {
    service_level_code = '2';
  } else if (service_level == "Auto") {
    service_level_code = 'A';
  } else {
    ESP_LOGW(TAG, "Ignoring unsupported service level option: %s", service_level.c_str());
    return;
  }

  std::string command = "SL ";
  command.push_back(service_level_code);
  this->queue_command_(command);
}

void OpenEVSE::set_feature_enabled(char feature_id, bool enabled) {
  if (!this->control_writes_ready_()) {
    return;
  }

  std::string command = "FF ";
  command.push_back(feature_id);
  command += enabled ? " 1" : " 0";
  this->queue_command_(command);
}

void OpenEVSE::enable_evse(bool enable) {
  if (!this->control_writes_ready_()) {
    return;
  }

  if (enable) {
    this->queue_command_("FE"); // Enable EVSE
  } else {
    this->queue_command_("FS"); // Sleep EVSE
  }
}

void OpenEVSE::get_state() {
  this->queue_command_("GS");
}

void OpenEVSE::get_current_capacity() {
  this->queue_command_("GC");
}

void OpenEVSE::get_ammeter_settings() {
  this->queue_command_("GA");
}

void OpenEVSE::get_charging_current_voltage() {
  this->queue_command_("GG");
}

void OpenEVSE::get_temperature() {
  this->queue_command_("GP");
}

// Add get_energy_usage method
void OpenEVSE::get_energy_usage() {
  this->queue_command_("GU");
}

void OpenEVSE::get_settings() {
  this->queue_command_("GE");
}

// Add get_version method
void OpenEVSE::get_version() {
  this->queue_command_("GV");
}

// Add a new helper method after the parse_state_text_ method
void OpenEVSE::update_state_sensors_(uint8_t evse_state, uint8_t pilot_state, uint16_t vflags) {
  this->evse_state_code_ = evse_state;
  // Update EVSE state sensor
  if (this->evse_state_sensor_ != nullptr) {
    this->evse_state_sensor_->publish_state(this->parse_state_text_(evse_state));
  }
  
  // Update pilot state sensor
  if (this->pilot_state_sensor_ != nullptr) {
    this->pilot_state_sensor_->publish_state(this->parse_state_text_(pilot_state));
  }
  
  // Update enable switch state based on EVSE state
  if (this->enable_switch_ != nullptr) {
    // State 0xFE is "Sleeping"
    this->enable_switch_->publish_state(evse_state != 0xFE);
  }

  // Update vehicle connected and charging sensors
  if (this->vehicle_connected_sensor_ != nullptr) {
    this->vehicle_connected_sensor_->publish_state(vflags & ECVF_EV_CONNECTED);
  }
  if (this->charging_sensor_ != nullptr) {
    this->charging_sensor_->publish_state(vflags & ECVF_CHARGING_ON);
  }
}

void OpenEVSE::handle_response_(const std::string &response) {
  const std::string current_command = this->current_request_.command;
  const std::string cmd = current_command.substr(0, std::min<size_t>(2, current_command.size()));

  // Tokenize the response by splitting on spaces
  std::vector<std::string> tokens;
  std::istringstream ss(response);
  std::string token;
  
  while (ss >> token) {
    tokens.push_back(token);
  }

  if (this->current_request_.capture_response) {
    if (this->rapi_response_sensor_ != nullptr) {
      this->rapi_response_sensor_->publish_state(response);
    }

    if (!tokens.empty() && tokens[0] == "$OK") {
      this->publish_rapi_status_("ok");
    } else if (!tokens.empty() && tokens[0] == "$NK") {
      this->publish_rapi_status_("nk");
    } else {
      this->publish_rapi_status_("response");
    }
  }
  
  if (cmd == "GS") {
    // Format: $OK evsestate elapsed pilotstate vflags
    if (tokens.size() != 5) {
      ESP_LOGE(TAG, "Invalid response length for $GS: %s", response.c_str());
      return;
    }
    
    uint8_t evse_state = strtoul(tokens[1].c_str(), nullptr, 16);
    uint32_t elapsed = strtoul(tokens[2].c_str(), nullptr, 10);
    uint8_t pilot_state = strtoul(tokens[3].c_str(), nullptr, 16);
    uint16_t vflags = strtoul(tokens[4].c_str(), nullptr, 16);
    
    // Update state sensors using the helper method
    this->update_state_sensors_(evse_state, pilot_state, vflags);
    
    // Update elapsed time sensor (specific to GS response)
    if (this->elapsed_sensor_ != nullptr) {
      this->elapsed_sensor_->publish_state(elapsed);
    }
      
    ESP_LOGD(TAG, "EVSE State: %s, Elapsed: %d seconds, Pilot State: %s", 
              this->parse_state_text_(evse_state).c_str(), elapsed, 
              this->parse_state_text_(pilot_state).c_str());

  } else if (cmd == "GC") {
    // Format: $OK minamps hmaxamps pilotamps cmaxamps
    int min_amps, hmax_amps, pilot_amps, cmax_amps;
    
    if (tokens.size() != 5) {
      ESP_LOGE(TAG, "Invalid response length for $GC: %s", response.c_str());
      return;
    }
    
    min_amps = strtoul(tokens[1].c_str(), nullptr, 10);
    hmax_amps = strtoul(tokens[2].c_str(), nullptr, 10);
    pilot_amps = strtoul(tokens[3].c_str(), nullptr, 10);
    cmax_amps = strtoul(tokens[4].c_str(), nullptr, 10);
    
    // Update sensors
    if (this->current_capacity_sensor_ != nullptr) {
      this->current_capacity_sensor_->publish_state(pilot_amps);
      this->current_capacity_control_->publish_state(pilot_amps);
    }
    
    // Update max capacity controls with actual values from hardware
    if (this->max_conf_capacity_control_ != nullptr) {
      this->max_conf_capacity_control_->publish_state(cmax_amps);
    }
    
    if (this->max_hw_capacity_control_ != nullptr) {
      this->max_hw_capacity_control_->publish_state(hmax_amps);
    }
    
    // Initialize current capacity control to match the current capacity sensor
    if (this->current_capacity_control_ != nullptr) {
      this->current_capacity_control_->publish_state(pilot_amps);
    }
    
    ESP_LOGD(TAG, "Current Capacity: Min=%d, HMax=%d, Pilot=%d, CMax=%d", 
              min_amps, hmax_amps, pilot_amps, cmax_amps);

  } else if (cmd == "SC") {
    // Format: $OK amps or $NK amps
    if (tokens.size() != 2) {
      ESP_LOGE(TAG, "Invalid response length for $SC: %s", response.c_str());
      return;
    }

    std::vector<std::string> command_tokens;
    std::istringstream cmd_ss(current_command);
    while (cmd_ss >> token) {
      command_tokens.push_back(token);
    }

    const int applied_amps = strtol(tokens[1].c_str(), nullptr, 10);
    const bool is_max_hw_write = command_tokens.size() >= 3 && command_tokens[2] == "M";
    const bool is_volatile_write = command_tokens.size() >= 3 && command_tokens[2] != "M";

    if (is_max_hw_write) {
      if (tokens[0] == "$OK") {
        ESP_LOGI(TAG, "Hardware max current stored in EEPROM: %d A", applied_amps);
      } else {
        ESP_LOGW(TAG, "$SC M rejected. Hardware max current is already fixed at %d A; this AVR firmware only allows writing it once via RAPI", applied_amps);
      }
    } else if (is_volatile_write) {
      if (tokens[0] == "$OK") {
        ESP_LOGI(TAG, "Volatile charging current set to %d A", applied_amps);
      } else {
        ESP_LOGW(TAG, "Volatile charging current request was clamped/rejected; resulting current is %d A", applied_amps);
      }
    } else {
      if (tokens[0] == "$OK") {
        ESP_LOGI(TAG, "Configured current for the active service level set to %d A", applied_amps);
      } else {
        ESP_LOGW(TAG, "Configured current request was clamped/rejected; resulting current is %d A", applied_amps);
      }
    }
    
    // SC affects current capacity state, and SC M may also clamp the live current.
    this->queue_command_("GC");
  } else if (cmd == "GE") {
    // Format: $OK amps flags(hex)
    if (tokens.size() != 3 || tokens[0] != "$OK") {
      ESP_LOGE(TAG, "Invalid response for $GE: %s", response.c_str());
      return;
    }

    int amps = strtoul(tokens[1].c_str(), nullptr, 10);
    uint16_t flags = strtoul(tokens[2].c_str(), nullptr, 16);
    std::string service_level = this->parse_service_level_(flags);

    if (this->service_level_select_ != nullptr) {
      this->service_level_select_->publish_state(service_level);
    }
    if (this->settings_flags_sensor_ != nullptr) {
      this->settings_flags_sensor_->publish_state(tokens[2]);
    }
    this->update_feature_switches_(flags);
    if (!this->controls_ready_) {
      this->controls_ready_ = true;
      ESP_LOGI(TAG, "Initial settings sync complete; control writes enabled");
    }

    ESP_LOGD(TAG, "Settings: Current=%d A, Flags(raw)=%s, Flags(parsed)=0x%04X, Service Level=%s",
             amps, tokens[2].c_str(), flags, service_level.c_str());
  } else if (cmd == "GA") {
    // Format: $OK currentscalefactor currentoffset
    if (tokens.size() != 3) {
      ESP_LOGE(TAG, "Invalid response length for $GA: %s", response.c_str());
      return;
    }
    
    int scale_factor = strtol(tokens[1].c_str(), nullptr, 10);
    int offset = strtol(tokens[2].c_str(), nullptr, 10);
    
    // Update controls
    if (this->current_scale_factor_control_ != nullptr) {
      this->current_scale_factor_control_->publish_state(scale_factor);
    }
    
    if (this->current_offset_control_ != nullptr) {
      this->current_offset_control_->publish_state(offset);
    }
    
    ESP_LOGD(TAG, "Ammeter Settings: Scale Factor=%d, Offset=%d", scale_factor, offset);
  } else if (cmd == "GG") {
    // Format: $OK milliamps millivolts
    if (tokens.size() != 3) {
      ESP_LOGE(TAG, "Invalid response length for $GG: %s", response.c_str());
      return;
    }
    
    float milliamps = strtol(tokens[1].c_str(), nullptr, 10);
    float millivolts = strtol(tokens[2].c_str(), nullptr, 10);
    
    // Convert to amps and volts
    float amps = milliamps / 1000.0f;
    float volts = millivolts / 1000.0f;
    
    // Update sensors if they exist
    if (this->charging_current_sensor_ != nullptr) {
      this->charging_current_sensor_->publish_state(amps);
    }
    if (this->voltage_control_ != nullptr) {
      this->voltage_control_->publish_state(volts);
    }
    
    ESP_LOGD(TAG, "Charging Current: %.3f A, Voltage: %.3f V", amps, volts);
  } else if (cmd == "GP") {
    // Format: $OK temp1 temp2 temp3
    if (tokens.size() < 2) {
      ESP_LOGE(TAG, "Invalid response length for $GP: %s", response.c_str());
      return;
    }
    
    // Check all temperature sensors in a loop and use the first valid one
    bool found_valid_temp = false;
    for (size_t i = 1; i < tokens.size(); i++) {
      if (tokens[i] != "-1" && tokens[i] != "-2560" && this->temperature_sensor_ != nullptr) {
        float temp = strtol(tokens[i].c_str(), nullptr, 10) / 10.0f; // Convert to degrees (value is in tenths)
        this->temperature_sensor_->publish_state(temp);
        
        // Log which sensor we're using (1-based index for user-friendly output)
        if (i == 1) {
          ESP_LOGD(TAG, "Temperature: %.1f °C", temp);
        } else {
          ESP_LOGD(TAG, "Temperature (sensor %zu): %.1f °C", i, temp);
        }
        
        found_valid_temp = true;
        break; // Found a valid temperature, no need to check more
      }
    }
    
    if (!found_valid_temp && this->temperature_sensor_ != nullptr) {
      ESP_LOGW(TAG, "No valid temperature readings found");
      this->temperature_sensor_->publish_state(NAN);
    }
  } else if (cmd == "SY") {
    // Format: $OK heartbeatinterval hearbeatcurrentlimit hearbeattrigger
    if (tokens.size() != 4) {
      ESP_LOGE(TAG, "Invalid response length for $SY: %s", response.c_str());
      return;
    }
    
    uint32_t heartbeatinterval = strtoul(tokens[1].c_str(), nullptr, 10);
    uint8_t hearbeatcurrentlimit = strtoul(tokens[2].c_str(), nullptr, 10);
    uint8_t hearbeattrigger = strtoul(tokens[3].c_str(), nullptr, 10);
    
    if (tokens[0] == "$NK") {
      ESP_LOGI(TAG, "Heartbeat missed, ACKing with $SY 165");
      this->queue_command_("SY 165");
    }
  } else if (cmd == "SL") {
    if (tokens.size() != 1) {
      ESP_LOGE(TAG, "Invalid response length for $SL: %s", response.c_str());
      return;
    }

    if (tokens[0] == "$OK") {
      ESP_LOGI(TAG, "Service level change accepted");
    } else {
      ESP_LOGW(TAG, "Service level change rejected");
    }

    this->queue_command_("GE");
    this->queue_command_("GC");
  } else if (cmd == "FF") {
    if (tokens.size() != 1) {
      ESP_LOGE(TAG, "Invalid response length for $FF: %s", response.c_str());
      return;
    }

    if (tokens[0] == "$OK") {
      ESP_LOGI(TAG, "Feature setting accepted: %s", current_command.c_str());
    } else {
      ESP_LOGW(TAG, "Feature setting rejected: %s", current_command.c_str());
    }

    this->queue_command_("GE");
  } else if (cmd == "FE" || cmd == "FS" || cmd == "SV") {
    if (tokens[0] == "$NK") {
      ESP_LOGI(TAG, "Command rejected: %s", response.c_str());
    }
  } else if (cmd == "GU") {
    // Format: $OK wattseconds whacc
    if (tokens.size() != 3) {
      ESP_LOGE(TAG, "Invalid response length for $GU: %s", response.c_str());
      return;
    }
    
    // Parse energy usage values
    uint32_t wattseconds = strtoul(tokens[1].c_str(), nullptr, 10);
    uint32_t whacc = strtoul(tokens[2].c_str(), nullptr, 10);
    
    // Convert to kWh
    float kwh = wattseconds / 3600000.0f;
    this->energy_usage_kwh_ = kwh;
    
    // Update sensor if it exists
    if (this->energy_usage_sensor_ != nullptr) {
      this->energy_usage_sensor_->publish_state(kwh);
    }
    
    ESP_LOGD(TAG, "Energy Usage: Session=%.3f kWh, Total=%.3f kWh", 
             kwh, whacc / 1000.0f);
  } else if (cmd == "GV") {
    // Format: $OK firmware_version protocol_version
    if (tokens.size() >= 2 && tokens[0] == "$OK") {
      // Store firmware version
      this->firmware_version_ = tokens[1];
      
      // Update firmware version sensor if available
      if (this->firmware_version_sensor_ != nullptr) {
        this->firmware_version_sensor_->publish_state(this->firmware_version_);
      }
      
      ESP_LOGI(TAG, "OpenEVSE firmware version: %s", this->firmware_version_.c_str());
      
      // If we're in INIT phase, move to SETUP phase
      if (this->startup_phase_ == StartupPhase::INIT) {
        ESP_LOGI(TAG, "Version received, moving to SETUP phase");
        this->startup_phase_ = StartupPhase::SETUP;
        this->last_update_ = millis() - this->update_interval_;
      }
    } else {
      ESP_LOGW(TAG, "Invalid response for $GV: %s", response.c_str());
    }
  } else {
    if (this->current_request_.capture_response) {
      ESP_LOGD(TAG, "Raw command response: %s -> %s", current_command.c_str(), response.c_str());
    } else {
      ESP_LOGW(TAG, "Unhandled response: %s", response.c_str());
    }
  }
  
}


void OpenEVSE::handle_async_notification_(const std::string &notification) {
  // Check notification type (second character after $A)
  if (notification.length() < 3) {
    ESP_LOGE(TAG, "Invalid async notification: %s", notification.c_str());
    return;
  }
  
  char notification_type = notification[2];
  
  switch (notification_type) {
    case 'T': {  // EVSE state transition
      // Format: $AT evsestate pilotstate currentcapacity vflags
      std::string evse_state_str, pilot_state_str, current_capacity_str, vflags_str;
      std::istringstream nss(notification.substr(3));  // Skip "$AT"
      
      if (!(nss >> evse_state_str >> pilot_state_str >> current_capacity_str >> vflags_str)) {
        ESP_LOGE(TAG, "Failed to parse AT notification: %s", notification.c_str());
        return;
      }
      
      // Convert hex string to integer
      uint8_t evse_state = strtol(evse_state_str.c_str(), nullptr, 16);
      uint8_t pilot_state = strtol(pilot_state_str.c_str(), nullptr, 16);
      int current_capacity = strtol(current_capacity_str.c_str(), nullptr, 10);
      uint16_t vflags = strtol(vflags_str.c_str(), nullptr, 16);

      // Update state sensors using the helper method
      this->update_state_sensors_(evse_state, pilot_state, vflags);
      
      // Update current capacity sensor (specific to AT notification)
      if (this->current_capacity_sensor_ != nullptr) {
        this->current_capacity_sensor_->publish_state(current_capacity);
      }

      ESP_LOGD(TAG, "Async EVSE State Change: EVSE=%s, Pilot=%s, Current=%d",
               this->parse_state_text_(evse_state).c_str(),
               this->parse_state_text_(pilot_state).c_str(),
               current_capacity);

      // Immediately refresh display on state change
      this->update_display();
      break;
    }
    
    case 'B': {  // Boot notification
      // Format: $AB postcode fwrev
      std::string postcode_str, fwrev;
      std::istringstream nss(notification.substr(3));  // Skip "$AB"
      
      if (!(nss >> postcode_str >> fwrev)) {
        ESP_LOGE(TAG, "Failed to parse AB notification: %s", notification.c_str());
        return;
      }
      
      uint8_t postcode = strtol(postcode_str.c_str(), nullptr, 16);
      
      if (postcode == 0x00) {
        ESP_LOGI(TAG, "OpenEVSE booted successfully, firmware: %s", fwrev.c_str());
      } else {
        ESP_LOGW(TAG, "OpenEVSE boot error, code: 0x%02X, firmware: %s", postcode, fwrev.c_str());
      }
      break;
    }
    
    default:
      ESP_LOGD(TAG, "Unhandled async notification: %s", notification.c_str());
      break;
  }
}


// Memory debugging method
void OpenEVSE::log_memory_usage(const char* location) {
  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  
  // Get IRAM memory info
  size_t free_iram = heap_caps_get_free_size(MALLOC_CAP_32BIT);
  size_t total_iram = heap_caps_get_total_size(MALLOC_CAP_32BIT);
  
  // Get DMA-capable memory info
  size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
  size_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);
  
  ESP_LOGI(TAG, "Memory at %s:", location);
  ESP_LOGI(TAG, "  DRAM - Free: %u bytes, Total: %u bytes, Min Free: %u bytes, Largest Block: %u bytes",
           free_heap, total_heap, min_free_heap, largest_free_block);
  ESP_LOGI(TAG, "  IRAM - Free: %u bytes, Total: %u bytes", free_iram, total_iram);
  ESP_LOGI(TAG, "  DMA  - Free: %u bytes, Total: %u bytes", free_dma, total_dma);
}

void OpenEVSE::set_dark_mode(bool dark) {
  this->dark_mode_ = dark;
  this->update_display();
}

void OpenEVSE::set_backlight_brightness(float pct) {
  this->backlight_brightness_ = pct / 100.0f;
  // Apply immediately — don't wait for next update_display cycle
  analogWrite(BACKLIGHT_PIN, (int)(this->backlight_brightness_ * 255));
}

// Update display with real-time sensor data
void OpenEVSE::update_display() {
  if (this->display_ == nullptr) return;
  if (this->screenshot_in_progress_) return;  // avoid SPI contention

  // Raw RAPI state code maps directly to EvseState enum
  auto state = static_cast<evse_display::EvseState>(this->evse_state_code_);

  float setpoint = 0, current = 0, voltage = 0, power_kw = 0, session_kwh = 0;
  uint32_t elapsed = 0;

  if (this->current_capacity_sensor_ && !std::isnan(this->current_capacity_sensor_->get_state()))
    setpoint = this->current_capacity_sensor_->get_state();
  if (this->charging_current_sensor_ && !std::isnan(this->charging_current_sensor_->get_state()))
    current = this->charging_current_sensor_->get_state();
  if (this->voltage_control_ && !std::isnan(this->voltage_control_->state))
    voltage = this->voltage_control_->state;
  if (voltage > 0 && current > 0)
    power_kw = (voltage * current) / 1000.0f;
  if (!std::isnan(this->energy_usage_kwh_))
    session_kwh = this->energy_usage_kwh_;
  if (this->elapsed_sensor_ && !std::isnan(this->elapsed_sensor_->get_state()))
    elapsed = (uint32_t)this->elapsed_sensor_->get_state();

  uint32_t dt = this->display_->update(state, setpoint, current, voltage,
                                        power_kw, session_kwh, elapsed,
                                        this->dark_mode_);

  // Compute LED state from combined flags
  bool evse_enabled = (this->enable_switch_ && this->enable_switch_->state);
  bool vehicle_connected = (this->vehicle_connected_sensor_ && this->vehicle_connected_sensor_->state);
  bool charging = (this->charging_sensor_ && this->charging_sensor_->state);
  this->led_state_ = evse_display::EvseDisplay::compute_led(state, evse_enabled, vehicle_connected, charging);

  // Backlight: HA-set brightness when active, off when sleeping/disabled + no car
  bool backlight_on = !((state == evse_display::STATE_SLEEPING || state == evse_display::STATE_DISABLED)
                        && !vehicle_connected);
  analogWrite(BACKLIGHT_PIN, backlight_on ? (int)(this->backlight_brightness_ * 255) : 0);

  if (state != this->last_evse_state_) {
    ESP_LOGI(TAG, "Display: %s (%uus)", evse_display::EvseDisplay::state_text(state), dt);
    this->last_evse_state_ = (uint8_t)state;
  }
}

// --- Screenshot HTTP handler ---

esp_err_t OpenEVSE::handle_screenshot_static(httpd_req_t *req) {
  return static_cast<OpenEVSE *>(req->user_ctx)->handle_screenshot(req);
}

esp_err_t OpenEVSE::handle_screenshot(httpd_req_t *req) {
  constexpr int16_t W = 480, H = 320;
  constexpr int16_t BAND_H = evse_display::EvseDisplay::SCREENSHOT_BAND_H;
  constexpr uint32_t ROW_BYTES = W * 3;
  constexpr uint32_t FILE_SIZE = 54 + ROW_BYTES * H;

  // Gather current display state
  auto state = static_cast<evse_display::EvseState>(this->evse_state_code_);
  float setpoint = this->current_capacity_sensor_ ? this->current_capacity_sensor_->state : 0;
  float current = this->charging_current_sensor_ ? this->charging_current_sensor_->state : 0;
  float voltage = this->voltage_control_ ? this->voltage_control_->state : 0;
  float power = (current * voltage) / 1000.0f;
  float session_kwh = std::isnan(this->energy_usage_kwh_) ? 0.0f : this->energy_usage_kwh_;
  uint32_t elapsed = this->elapsed_sensor_ ? (uint32_t)this->elapsed_sensor_->state : 0;
  bool dark = this->dark_mode_;

  // BMP header (54 bytes): 14-byte file header + 40-byte info header
  uint8_t hdr[54] = {};
  // File header
  hdr[0] = 'B'; hdr[1] = 'M';
  memcpy(&hdr[2], &FILE_SIZE, 4);        // file size
  uint32_t offset = 54;
  memcpy(&hdr[10], &offset, 4);          // pixel data offset
  // Info header
  uint32_t info_size = 40;
  memcpy(&hdr[14], &info_size, 4);
  int32_t bmp_w = W;
  int32_t bmp_h = -H;                    // negative = top-down
  memcpy(&hdr[18], &bmp_w, 4);
  memcpy(&hdr[22], &bmp_h, 4);
  uint16_t planes = 1;
  memcpy(&hdr[26], &planes, 2);
  uint16_t bpp = 24;
  memcpy(&hdr[28], &bpp, 2);
  uint32_t img_size = ROW_BYTES * H;
  memcpy(&hdr[34], &img_size, 4);

  // Increase socket send timeout (default 5s is too short for 460KB)
  int fd = httpd_req_to_sockfd(req);
  struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_send_chunk(req, (const char *)hdr, 54);

  ESP_LOGI(TAG, "Screenshot: streaming %dx%d BMP", W, H);
  this->screenshot_in_progress_ = true;
  vTaskDelay(pdMS_TO_TICKS(200));  // let main loop finish any in-flight SPI

  // Stream row by row — render each band, convert, send
  TFT_eSprite sprite(&this->tft);
  uint8_t rgb_row[ROW_BYTES];

  for (int16_t band_y = 0; band_y < H; band_y += BAND_H) {
    int16_t band_h = std::min((int16_t)BAND_H, (int16_t)(H - band_y));

    bool ok = this->display_->render_band(sprite, band_y, state, setpoint,
                                          current, voltage, power, session_kwh,
                                          elapsed, dark);

    uint16_t *px = ok ? (uint16_t *)sprite.getPointer() : nullptr;
    for (int row = 0; row < band_h; row++) {
      if (px) {
        for (int x = 0; x < W; x++) {
          uint16_t c = __builtin_bswap16(px[row * W + x]);  // sprite stores big-endian
          rgb_row[x * 3 + 0] = (c & 0x1F) << 3;         // B
          rgb_row[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;   // G
          rgb_row[x * 3 + 2] = (c >> 11) << 3;            // R
        }
      } else {
        // Magenta fill for failed bands
        for (int x = 0; x < W; x++) {
          rgb_row[x * 3 + 0] = 255;
          rgb_row[x * 3 + 1] = 0;
          rgb_row[x * 3 + 2] = 255;
        }
      }
      if (httpd_resp_send_chunk(req, (const char *)rgb_row, ROW_BYTES) != ESP_OK) {
        if (ok) sprite.deleteSprite();
        return ESP_FAIL;
      }
      if ((row & 7) == 7) vTaskDelay(1);  // yield every 8 rows for TCP processing
    }
    if (ok) sprite.deleteSprite();
  }

  this->screenshot_in_progress_ = false;
  httpd_resp_send_chunk(req, NULL, 0);
  ESP_LOGI(TAG, "Screenshot served (%u bytes)", FILE_SIZE);
  return ESP_OK;
}

}  // namespace openevse
}  // namespace esphome
