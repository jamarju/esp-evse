# CLAUDE.md

## Project Overview

ESPHome-based firmware for OpenEVSE electric vehicle chargers with a TFT touchscreen UI, NeoPixel LED status strip, and Home Assistant integration. Communicates with the OpenEVSE controller via the RAPI serial protocol over UART.

## Rules

- Use `uv` for project management
  - `uv run esphome run ev1.yaml` to compile + flash ev1.yaml
  - `uv add` to add dependencies
- Do not ever commit `secrets.yaml`.
- PlatformIO isolation: set `export PLATFORMIO_CORE_DIR=$(pwd)/.platformio` to avoid conflicts with system-wide PlatformIO installations if the user so desires.

## Architecture

1. **YAML config layer** — `openevse-tft.yaml` declares the platform (ESP32, esp32dev, 16MB flash), TFT_eSPI display driver (ILI9488 320×480), UART pins, NeoPixel LED strip, sensors, and controls. `ev1.yaml`/`ev2.yaml` are per-device overrides.

2. **Python bridge layer** — `components/openevse/__init__.py` defines the ESPHome component schema (sensors, binary sensors, number controls, switches) and generates the C++ glue code via `cg.add()` calls.

3. **C++ core layer** — `components/openevse/openevse.{h,cpp}` is the main component class (`OpenEVSE`, inherits `PollingComponent` + `UARTDevice`). Handles RAPI command/response framing, XOR checksums, and a phased startup (INIT → SETUP → RUN).

### RAPI Protocol (Serial Communication)

- UART1 at 115200 baud (TX=GPIO1, RX=GPIO3)
- Commands: `$XX params^checksum\r` — Responses: `$OK [data]^checksum\r` or `$NK`
- Async notifications from OpenEVSE: `$AT evsestate pilotstate currentcapacity vflags`
- Heartbeat supervision via `$SY` to detect ESP32↔OpenEVSE link loss
- Full protocol reference: `docs/rapi_doc.txt`

### Display & UI

- `evse_display.{h,cpp}` — TFT_eSPI-based rendering with dark/light themes, RGB565 colors, sprite-based text to avoid flicker
- Precompiled VLW fonts in `font_big.h`, `font_medium.h`, `font_small.h` (MonaspiceNe Nerd Font)
- Font conversion: `uv run python scripts/ttf2vlw.py <input.ttf> <size> <output.h>` (custom converter with baseline fix)

### Sensor/Control Components

Each control (current capacity, voltage, ammeter calibration, backlight, enable switch) is wired through `components/openevse/__init__.py` and delegated to the main OpenEVSE component.

### NeoPixel LED Strip

4 WS2812 LEDs on GPIO26 (RMT channel 0). Driven directly from C++ with state-aware color animations reflecting EVSE state.
