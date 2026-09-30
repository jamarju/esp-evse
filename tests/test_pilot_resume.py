"""Host tests compile the actual control methods, with UART/sensors stubbed."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index(f"void OpenEVSE::{name}(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PilotResumeTest(unittest.TestCase):
    def test_actual_control_methods(self):
        source = (ROOT / "components/openevse/openevse.cpp").read_text()
        methods = "\n".join(method(source, name) for name in (
            "enable_evse", "cancel_pilot_resume_", "process_pilot_resume_",
            "handle_pilot_resume_response_", "update_state_sensors_",
        ))
        # Fields and phase definitions come from the real component header.
        header = (ROOT / "components/openevse/openevse.h").read_text()
        phase_start = header.index("  enum class PilotResumePhase")
        phase_end = header.index(";", header.index("uint32_t pilot_resume_at_", phase_start)) + 1
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ECVF_EV_CONNECTED 0x0100
#define ECVF_CHARGING_ON 0x0040
uint32_t clock_ms = 100;
uint32_t millis() { return clock_ms; }
struct Sensor {
  bool state = false;
  void publish_state(bool x) { state = x; }
  void publish_state(const std::string &) {}
};
class OpenEVSE {
 public:
  struct QueuedCommand { std::string command; };
  std::deque<QueuedCommand> command_queue_;
  std::vector<std::string> trace;
  bool accept = true, ready = true;
  uint8_t evse_state_code_ = 0x02;
  bool charging_ = false, vehicle_connected_ = true, evse_enabled_ = true;
  Sensor sensor;
  Sensor *enable_switch_ = &sensor;
  Sensor *evse_state_sensor_ = nullptr, *pilot_state_sensor_ = nullptr;
  Sensor *vehicle_connected_sensor_ = nullptr, *charging_sensor_ = nullptr;
  bool control_writes_ready_() const { return ready; }
  std::string parse_state_text_(uint8_t) { return ""; }
  bool queue_command_(const std::string &cmd) {
    if (!accept) return false;
    trace.push_back(cmd);
    command_queue_.push_back({cmd});
    return true;
  }
  void get_state() { queue_command_("GS"); }
  void enable_evse(bool);
  void cancel_pilot_resume_();
  void process_pilot_resume_(uint32_t);
  void handle_pilot_resume_response_(const std::string &, bool);
  void update_state_sensors_(uint8_t, uint8_t, uint16_t);
PHASE_FIELDS
};
METHODS
void begin(OpenEVSE &ev) {
  ev.enable_evse(true);
  assert(ev.trace == std::vector<std::string>{"FS"});
  ev.handle_pilot_resume_response_("FS", true);
  ev.update_state_sensors_(0xFE, 2, 0x0100);
  assert(ev.sensor.state); // Intent stays on during internal B1 dwell.
}
int main() {
  using Phase = OpenEVSE::PilotResumePhase;
  // Connected: FE alone would be a no-op. No FE before FS acknowledgement.
  OpenEVSE ev;
  ev.enable_evse(true);
  ev.process_pilot_resume_(clock_ms + 10000);
  assert(ev.trace == std::vector<std::string>{"FS"});
  ev.handle_pilot_resume_response_("FS", true);
  ev.update_state_sensors_(0xFE, 2, 0x0100);
  ev.enable_evse(true); // Duplicate must not extend the dwell.
  ev.process_pilot_resume_(clock_ms + 2999);
  assert(ev.trace.size() == 1);
  ev.process_pilot_resume_(clock_ms + 3000);
  assert(ev.trace.back() == "GS");
  ev.handle_pilot_resume_response_("GS", true);
  assert(ev.trace.back() == "FE");
  ev.process_pilot_resume_(clock_ms + 100000);
  assert(ev.trace.size() == 3); // One pulse, never periodic retries.
  ev.handle_pilot_resume_response_("FE", true);
  assert(ev.sensor.state && ev.pilot_resume_phase_ == Phase::IDLE);
  // Sleeping and Disabled both get the same positive-pilot transition.
  for (uint8_t state : {0xFE, 0xFF}) {
    OpenEVSE paused; paused.evse_state_code_ = state; begin(paused);
  }
  // Existing charge, disconnected EV, and faults must not be interrupted.
  for (uint8_t state : {0x01, 0x03, 0x05, 0x06, 0x08, 0x0A}) {
    OpenEVSE running; running.evse_state_code_ = state;
    running.charging_ = state == 3;
    running.enable_evse(true);
    assert(running.trace == std::vector<std::string>{"FE"});
  }
  // Off cancels the timer and removes a queued FE, even after it was queued.
  OpenEVSE stopped; begin(stopped);
  stopped.process_pilot_resume_(clock_ms + 3000);
  stopped.handle_pilot_resume_response_("GS", true);
  stopped.enable_evse(false);
  assert(stopped.trace.back() == "FS");
  for (auto &cmd : stopped.command_queue_) assert(cmd.command != "FE");
  auto count = stopped.trace.size();
  stopped.handle_pilot_resume_response_("FS", true);
  stopped.process_pilot_resume_(clock_ms + 100000);
  assert(stopped.trace.size() == count);
  // Graceful Sleep may still have its relay closed: wait, don't force FE.
  OpenEVSE relay; begin(relay);
  relay.process_pilot_resume_(clock_ms + 3000);
  relay.charging_ = true;
  relay.handle_pilot_resume_response_("GS", true);
  assert(relay.trace.back() == "GS");
  relay.charging_ = false;
  relay.process_pilot_resume_(clock_ms + 1000);
  relay.handle_pilot_resume_response_("GS", true);
  assert(relay.trace.back() == "FE");
  // Rejected commands, queue full and UART timeout cannot enable later.
  for (const std::string cmd : {"FS", "GS", "FE"}) {
    OpenEVSE rejected; begin(rejected);
    if (cmd == "FS") rejected.pilot_resume_phase_ = Phase::WAIT_SLEEP;
    if (cmd != "FS") rejected.process_pilot_resume_(clock_ms + 3000);
    if (cmd == "FE") rejected.handle_pilot_resume_response_("GS", true);
    rejected.handle_pilot_resume_response_(cmd, false);
    assert(rejected.pilot_resume_phase_ == Phase::IDLE);
  }
  OpenEVSE full; full.accept = false; full.enable_evse(true);
  assert(full.pilot_resume_phase_ == Phase::IDLE && full.trace.empty());
  OpenEVSE timeout; begin(timeout); timeout.cancel_pilot_resume_();
  timeout.process_pilot_resume_(clock_ms + 10000);
  assert(timeout.trace.size() == 1);
  // A new hardware fault overrides the resume, keeping AVR protections.
  OpenEVSE fault; begin(fault); fault.update_state_sensors_(6, 2, 0x0100);
  fault.process_pilot_resume_(clock_ms + 10000);
  assert(fault.trace.size() == 1 && fault.pilot_resume_phase_ == Phase::IDLE);
  // Disabled is reported as off; no F is ever generated by this sequence.
  OpenEVSE disabled; disabled.update_state_sensors_(0xFF, 0, 0);
  assert(!disabled.sensor.state);
  // millis rollover retains the full three-second dwell.
  clock_ms = UINT32_MAX - 1000;
  OpenEVSE wrap; begin(wrap);
  wrap.process_pilot_resume_(clock_ms + 2999);
  assert(wrap.trace.size() == 1);
  wrap.process_pilot_resume_(clock_ms + 3000);
  assert(wrap.trace.back() == "GS");
}
'''.replace("PHASE_FIELDS", header[phase_start:phase_end]).replace("METHODS", methods)
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "pilot_resume.cpp"
            binary = Path(directory) / "pilot_resume"
            cpp.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
