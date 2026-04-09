import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, sensor, text_sensor, number, switch, binary_sensor, select, text
from esphome.const import (
    CONF_ID,
    CONF_UPDATE_INTERVAL,
    CONF_UART_ID,
    UNIT_SECOND,
    UNIT_AMPERE,
    UNIT_VOLT,
    UNIT_CELSIUS,
    UNIT_KILOWATT_HOURS,
    ICON_TIMER,
    ICON_CURRENT_AC,
    ICON_THERMOMETER,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_ENERGY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "select"]

openevse_ns = cg.esphome_ns.namespace("openevse")
OpenEVSE = openevse_ns.class_("OpenEVSE", cg.Component)

CONF_EVSE_STATE = "evse_state"
CONF_PILOT_STATE = "pilot_state"
CONF_ELAPSED_TIME = "elapsed_time"
CONF_CURRENT_CAPACITY = "current_capacity"
CONF_CHARGING_CURRENT = "charging_current"
CONF_TEMPERATURE = "temperature"
CONF_ENERGY_USAGE = "energy_usage"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_SETTINGS_FLAGS = "settings_flags"
CONF_RAPI_RESPONSE = "rapi_response"
CONF_RAPI_STATUS = "rapi_status"
CONF_RAW_COMMAND_TEXT_ID = "raw_command_text_id"
CONF_VEHICLE_CONNECTED = "vehicle_connected"
CONF_CHARGING = "charging"

# ID references to template entities (defined in YAML, wired here)
CONF_CURRENT_CONTROL_ID = "current_control_id"
CONF_MAX_CONF_CONTROL_ID = "max_conf_control_id"
CONF_MAX_HW_CONTROL_ID = "max_hw_control_id"
CONF_ENABLE_SWITCH_ID = "enable_switch_id"
CONF_CURRENT_SCALE_FACTOR_ID = "current_scale_factor_id"
CONF_CURRENT_OFFSET_ID = "current_offset_id"
CONF_VOLTAGE_CONTROL_ID = "voltage_control_id"
CONF_BACKLIGHT_ID = "backlight_id"
CONF_SERVICE_LEVEL_SELECT_ID = "service_level_select_id"
CONF_FRONT_BUTTON_SWITCH_ID = "front_button_switch_id"
CONF_DIODE_CHECK_SWITCH_ID = "diode_check_switch_id"
CONF_VENT_REQUIRED_SWITCH_ID = "vent_required_switch_id"
CONF_GROUND_CHECK_SWITCH_ID = "ground_check_switch_id"
CONF_STUCK_RELAY_CHECK_SWITCH_ID = "stuck_relay_check_switch_id"
CONF_GFI_SELF_TEST_SWITCH_ID = "gfi_self_test_switch_id"
CONF_TEMPERATURE_MONITORING_SWITCH_ID = "temperature_monitoring_switch_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenEVSE),
            cv.GenerateID(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_UPDATE_INTERVAL, default="15s"): cv.update_interval,
            # Sensors
            cv.Optional(CONF_EVSE_STATE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_PILOT_STATE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_ELAPSED_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                icon=ICON_TIMER,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION
            ),
            cv.Optional(CONF_CURRENT_CAPACITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_AMPERE,
                icon=ICON_CURRENT_AC,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT
            ),
            cv.Optional(CONF_CHARGING_CURRENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_AMPERE,
                icon=ICON_CURRENT_AC,
                accuracy_decimals=3,
                state_class=STATE_CLASS_MEASUREMENT
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                icon=ICON_THERMOMETER,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT
            ),
            cv.Optional(CONF_ENERGY_USAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_KILOWATT_HOURS,
                icon="mdi:lightning-bolt",
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING
            ),
            cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:information-outline"
            ),
            cv.Optional(CONF_SETTINGS_FLAGS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:form-textbox"
            ),
            cv.Optional(CONF_RAPI_RESPONSE): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:console-line"
            ),
            cv.Optional(CONF_RAPI_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:list-status"
            ),
            cv.Optional(CONF_VEHICLE_CONNECTED): binary_sensor.binary_sensor_schema(
                icon="mdi:ev-plug-type2"
            ),
            cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
                icon="mdi:battery-charging"
            ),
            # ID references to template entities
            cv.Optional(CONF_CURRENT_CONTROL_ID): cv.use_id(number.Number),
            cv.Optional(CONF_MAX_CONF_CONTROL_ID): cv.use_id(number.Number),
            cv.Optional(CONF_MAX_HW_CONTROL_ID): cv.use_id(number.Number),
            cv.Optional(CONF_ENABLE_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_CURRENT_SCALE_FACTOR_ID): cv.use_id(number.Number),
            cv.Optional(CONF_CURRENT_OFFSET_ID): cv.use_id(number.Number),
            cv.Optional(CONF_VOLTAGE_CONTROL_ID): cv.use_id(number.Number),
            cv.Optional(CONF_BACKLIGHT_ID): cv.use_id(number.Number),
            cv.Optional(CONF_SERVICE_LEVEL_SELECT_ID): cv.use_id(select.Select),
            cv.Optional(CONF_FRONT_BUTTON_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_DIODE_CHECK_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_VENT_REQUIRED_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_GROUND_CHECK_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_STUCK_RELAY_CHECK_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_GFI_SELF_TEST_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_TEMPERATURE_MONITORING_SWITCH_ID): cv.use_id(switch.Switch),
            cv.Optional(CONF_RAW_COMMAND_TEXT_ID): cv.use_id(text.Text),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_UART_ID in config:
        uart_parent = await cg.get_variable(config[CONF_UART_ID])
        cg.add(var.set_uart_parent(uart_parent))

    if CONF_UPDATE_INTERVAL in config:
        cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

    # Sensors
    for conf_key, setter in [
        (CONF_EVSE_STATE, "set_evse_state_sensor"),
        (CONF_PILOT_STATE, "set_pilot_state_sensor"),
        (CONF_FIRMWARE_VERSION, "set_firmware_version_sensor"),
        (CONF_SETTINGS_FLAGS, "set_settings_flags_sensor"),
        (CONF_RAPI_RESPONSE, "set_rapi_response_sensor"),
        (CONF_RAPI_STATUS, "set_rapi_status_sensor"),
    ]:
        if conf_key in config:
            sens = await text_sensor.new_text_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    for conf_key, setter in [
        (CONF_ELAPSED_TIME, "set_elapsed_sensor"),
        (CONF_CURRENT_CAPACITY, "set_current_capacity_sensor"),
        (CONF_CHARGING_CURRENT, "set_charging_current_sensor"),
        (CONF_TEMPERATURE, "set_temperature_sensor"),
        (CONF_ENERGY_USAGE, "set_energy_usage_sensor"),
    ]:
        if conf_key in config:
            sens = await sensor.new_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    for conf_key, setter in [
        (CONF_VEHICLE_CONNECTED, "set_vehicle_connected_sensor"),
        (CONF_CHARGING, "set_charging_sensor"),
    ]:
        if conf_key in config:
            sens = await binary_sensor.new_binary_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    # Wire template entity IDs to C++ component
    for conf_key, setter in [
        (CONF_CURRENT_CONTROL_ID, "set_current_capacity_control"),
        (CONF_MAX_CONF_CONTROL_ID, "set_max_conf_capacity_control"),
        (CONF_MAX_HW_CONTROL_ID, "set_max_hw_capacity_control"),
        (CONF_CURRENT_SCALE_FACTOR_ID, "set_current_scale_factor_control"),
        (CONF_CURRENT_OFFSET_ID, "set_current_offset_control"),
        (CONF_VOLTAGE_CONTROL_ID, "set_voltage_control"),
        (CONF_BACKLIGHT_ID, "set_backlight_control"),
        (CONF_RAW_COMMAND_TEXT_ID, "set_raw_command_input"),
        (CONF_SERVICE_LEVEL_SELECT_ID, "set_service_level_select"),
        (CONF_FRONT_BUTTON_SWITCH_ID, "set_front_button_switch"),
        (CONF_DIODE_CHECK_SWITCH_ID, "set_diode_check_switch"),
        (CONF_VENT_REQUIRED_SWITCH_ID, "set_vent_required_switch"),
        (CONF_GROUND_CHECK_SWITCH_ID, "set_ground_check_switch"),
        (CONF_STUCK_RELAY_CHECK_SWITCH_ID, "set_stuck_relay_check_switch"),
        (CONF_GFI_SELF_TEST_SWITCH_ID, "set_gfi_self_test_switch"),
        (CONF_TEMPERATURE_MONITORING_SWITCH_ID, "set_temperature_monitoring_switch"),
    ]:
        if conf_key in config:
            ctrl = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, setter)(ctrl))

    if CONF_ENABLE_SWITCH_ID in config:
        ctrl = await cg.get_variable(config[CONF_ENABLE_SWITCH_ID])
        cg.add(var.set_enable_switch(ctrl))
