import esphome.codegen as cg
from esphome.components import sensor
from esphome.components.esp32 import CONF_CPU_FREQUENCY
from esphome.components.psram import DOMAIN as PSRAM_DOMAIN
import esphome.config_validation as cv
from esphome.const import (
    CONF_BLOCK,
    CONF_FRAGMENTATION,
    CONF_FREE,
    CONF_LOOP_TIME,
    DEVICE_CLASS_FREQUENCY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_COUNTER,
    ICON_TIMER,
    PLATFORM_BK72XX,
    PLATFORM_LN882X,
    PLATFORM_RTL87XX,
    STATE_CLASS_MEASUREMENT,
    UNIT_BYTES,
    UNIT_HERTZ,
    UNIT_MILLISECOND,
    UNIT_PERCENT,
)

from . import (  # noqa: F401  pylint: disable=unused-import
    CONF_DEBUG_ID,
    FILTER_SOURCE_FILES,
    DebugComponent,
)

DEPENDENCIES = ["debug"]

CONF_CPU_IDLE = "cpu_idle"
CONF_CORES = "cores"
CONF_CORE = "core"
CONF_MIN_FREE = "min_free"
CONF_PSRAM = "psram"
CONF_TASKS = "tasks"
CONF_TASK_NAME = "task_name"

# FreeRTOS task names are limited to configMAX_TASK_NAME_LEN (16 by default in ESP-IDF,
# allowing 15 characters plus a null terminator). The name must match a task's name EXACTLY:
# it is resolved once with xTaskGetHandle() and the handle is then reused, which is what keeps
# sampling cheap enough to leave interrupts alone. Use `log_cpu_usage: true` on the `debug`
# component to print the real names if you are not sure what a task is called.
TASK_NAME_MAX_LEN = 15

# Dual-core is the widest any current ESP32 variant goes; single-core variants only have core 0.
# An out-of-range core would read a idle-task handle that does not exist.
CORE_ID_MAX = 1

TASK_CPU_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_PERCENT,
    icon="mdi:cpu-32-bit",
    accuracy_decimals=1,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.Required(CONF_TASK_NAME): cv.All(
            cv.string_strict, cv.Length(min=1, max=TASK_NAME_MAX_LEN)
        ),
    }
)

CORE_CPU_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_PERCENT,
    icon="mdi:cpu-32-bit",
    accuracy_decimals=1,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.Required(CONF_CORE): cv.int_range(min=0, max=CORE_ID_MAX),
    }
)

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_DEBUG_ID): cv.use_id(DebugComponent),
    cv.Optional(CONF_FREE): sensor.sensor_schema(
        unit_of_measurement=UNIT_BYTES,
        icon=ICON_COUNTER,
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_BLOCK): sensor.sensor_schema(
        unit_of_measurement=UNIT_BYTES,
        icon=ICON_COUNTER,
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_FRAGMENTATION): cv.All(
        cv.Any(
            cv.All(
                cv.only_on_esp8266,
                cv.require_framework_version(esp8266_arduino=cv.Version(2, 5, 2)),
            ),
            cv.only_on_esp32,
            msg="This feature is only available on ESP8266 (Arduino 2.5.2+) and ESP32",
        ),
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon=ICON_COUNTER,
            accuracy_decimals=1,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    cv.Optional(CONF_MIN_FREE): cv.All(
        cv.Any(
            cv.only_on_esp32,
            cv.only_on([PLATFORM_BK72XX, PLATFORM_LN882X, PLATFORM_RTL87XX]),
            msg="This feature is only available on ESP32 and LibreTiny (BK72xx, LN882x, RTL87xx)",
        ),
        sensor.sensor_schema(
            unit_of_measurement=UNIT_BYTES,
            icon=ICON_COUNTER,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    cv.Optional(CONF_LOOP_TIME): sensor.sensor_schema(
        unit_of_measurement=UNIT_MILLISECOND,
        icon=ICON_TIMER,
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_PSRAM): cv.All(
        cv.only_on_esp32,
        cv.requires_component(PSRAM_DOMAIN),
        sensor.sensor_schema(
            unit_of_measurement=UNIT_BYTES,
            icon=ICON_COUNTER,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    cv.Optional(CONF_CPU_FREQUENCY): cv.All(
        sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            icon="mdi:speedometer",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_FREQUENCY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    cv.Optional(CONF_CPU_IDLE): cv.All(
        cv.only_on_esp32,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon="mdi:cpu-32-bit",
            accuracy_decimals=1,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    cv.Optional(CONF_CORES): cv.All(
        cv.only_on_esp32,
        cv.ensure_list(CORE_CPU_SCHEMA),
    ),
    cv.Optional(CONF_TASKS): cv.All(
        cv.only_on_esp32,
        cv.ensure_list(TASK_CPU_SCHEMA),
    ),
}


def _enable_run_time_stats():
    """Turn on the FreeRTOS bookkeeping every CPU sensor here reads.

    GENERATE_RUN_TIME_STATS is what maintains each task's ulRunTimeCounter, and TRACE_FACILITY is
    what makes vTaskGetInfo() available to read one out. Both cost a timestamp on every context
    switch, which is why they are only enabled when a CPU sensor actually asks for them.
    """
    from esphome.components import esp32

    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_USE_TRACE_FACILITY", True)
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS", True)


async def to_code(config):
    debug_component = await cg.get_variable(config[CONF_DEBUG_ID])

    if free_conf := config.get(CONF_FREE):
        sens = await sensor.new_sensor(free_conf)
        cg.add(debug_component.set_free_sensor(sens))

    if block_conf := config.get(CONF_BLOCK):
        sens = await sensor.new_sensor(block_conf)
        cg.add(debug_component.set_block_sensor(sens))

    if fragmentation_conf := config.get(CONF_FRAGMENTATION):
        sens = await sensor.new_sensor(fragmentation_conf)
        cg.add(debug_component.set_fragmentation_sensor(sens))

    if min_free_conf := config.get(CONF_MIN_FREE):
        sens = await sensor.new_sensor(min_free_conf)
        cg.add(debug_component.set_min_free_sensor(sens))

    if loop_time_conf := config.get(CONF_LOOP_TIME):
        sens = await sensor.new_sensor(loop_time_conf)
        cg.add(debug_component.set_loop_time_sensor(sens))

    if psram_conf := config.get(CONF_PSRAM):
        sens = await sensor.new_sensor(psram_conf)
        cg.add(debug_component.set_psram_sensor(sens))

    if cpu_freq_conf := config.get(CONF_CPU_FREQUENCY):
        sens = await sensor.new_sensor(cpu_freq_conf)
        cg.add(debug_component.set_cpu_frequency_sensor(sens))

    if cpu_idle_conf := config.get(CONF_CPU_IDLE):
        _enable_run_time_stats()
        sens = await sensor.new_sensor(cpu_idle_conf)
        cg.add(debug_component.set_cpu_idle_sensor(sens))

    if cores_conf := config.get(CONF_CORES):
        _enable_run_time_stats()
        cg.add(debug_component.init_core_cpu_sensors(len(cores_conf)))
        for core_conf in cores_conf:
            sens = await sensor.new_sensor(core_conf)
            cg.add(debug_component.add_core_cpu_sensor(core_conf[CONF_CORE], sens))

    if tasks_conf := config.get(CONF_TASKS):
        _enable_run_time_stats()
        cg.add(debug_component.init_task_cpu_sensors(len(tasks_conf)))
        for task_conf in tasks_conf:
            sens = await sensor.new_sensor(task_conf)
            cg.add(debug_component.add_task_cpu_sensor(task_conf[CONF_TASK_NAME], sens))
