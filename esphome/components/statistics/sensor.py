import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    CONF_SEND_EVERY,
    CONF_SEND_FIRST_AT,
    CONF_SOURCE_ID,
    CONF_WINDOW_SIZE,
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_COUNT,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL,
    CONF_TYPE,
)
from esphome.core.entity_helpers import inherit_property_from

CODEOWNERS = ["@kahrendt"]

statistics_ns = cg.esphome_ns.namespace("statistics")
StatisticsComponent = statistics_ns.class_("StatisticsComponent", cg.Component)

CONF_SLIDING_WINDOW = "sliding_window"
CONF_RUNNING = "running"
CONF_RESET_EVERY = "reset_every"

CONF_MEAN = "mean"
CONF_MAX = "max"
CONF_MIN = "min"
CONF_STD_DEV = "std_dev"
CONF_VARIANCE = "variance"
CONF_TREND = "trend"
CONF_COVARIANCE = "covariance"
CONF_TIME_UNIT = "time_unit"

StatisticsType = statistics_ns.enum("StatisticsType")
STATISTICS_TYPES = {
    CONF_SLIDING_WINDOW: StatisticsType.STATISTICS_TYPE_SLIDING_WINDOW,
    CONF_RUNNING: StatisticsType.STATISTICS_TYPE_RUNNING,
}

ResetAction = statistics_ns.class_("ResetAction", automation.Action)


TimeConversionFactor = statistics_ns.enum("TimeConversionFactor")
TIME_CONVERSION_FACTORS = {
    "ms": TimeConversionFactor.FACTOR_MS,
    "s": TimeConversionFactor.FACTOR_S,
    "min": TimeConversionFactor.FACTOR_MIN,
    "h": TimeConversionFactor.FACTOR_HOUR,
    "d": TimeConversionFactor.FACTOR_DAY,
}


# covarance's unit is original unit of measurement multiplied by time unit of measurement
# borrowed from sensor/ntegration/sensor.py
def transform_covariance_unit_of_measurement(uom, config):
    suffix = config[CONF_TIME_UNIT]
    if uom.endswith("/" + suffix):
        return uom[0 : -len("/" + suffix)]
    return uom + "⋅" + suffix


# variance's unit is original unit of measurement squared
def transform_variance_unit_of_measurement(uom, config):
    return "(" + uom + ")²"


# trend's unit is in original unit of measurement divides by time unit of measurement
def transform_trend_unit_of_measurement(uom, config):
    denominator = config[CONF_TIME_UNIT]
    return uom + "/" + denominator


# borrowed from sensor/__init__.py
def validate_send_first_at(value):
    send_first_at = value.get(CONF_SEND_FIRST_AT)
    send_every = value[CONF_SEND_EVERY]
    if send_first_at is not None and send_first_at > send_every:
        raise cv.Invalid(
            f"send_first_at must be smaller than or equal to send_every! {send_first_at} <= {send_every}"
        )
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StatisticsComponent),
        cv.Required(CONF_SOURCE_ID): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_WINDOW_SIZE, default=15): cv.positive_not_null_int,
        cv.Optional(CONF_RESET_EVERY, default=1000): cv.positive_int,
        cv.Optional(CONF_SEND_EVERY, default=15): cv.positive_not_null_int,
        cv.Optional(CONF_SEND_FIRST_AT, default=1): cv.positive_not_null_int,
        cv.Optional(CONF_TYPE, default=CONF_SLIDING_WINDOW): cv.enum(
            STATISTICS_TYPES, lower=True
        ),
        cv.Optional(CONF_TIME_UNIT, default="s"): cv.enum(
            TIME_CONVERSION_FACTORS, lower=True
        ),
        cv.Optional(CONF_MEAN): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MAX): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MIN): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_STD_DEV): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VARIANCE): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_COUNT): sensor.sensor_schema(
            state_class=STATE_CLASS_TOTAL,
        ),
        cv.Optional(CONF_TREND): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_COVARIANCE): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    },
).extend(cv.COMPONENT_SCHEMA)

# approach orrowed from kalman sensor component
properties_to_inherit_original_unit = [
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
]

properties_to_inherit_new_unit = [
    CONF_ACCURACY_DECIMALS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
]

same_unit_sensor_list = [CONF_MEAN, CONF_MIN, CONF_MAX, CONF_STD_DEV]
new_unit_sensor_list = [CONF_VARIANCE, CONF_COVARIANCE, CONF_TREND]

inherit_schema_for_same_unit_sensors = [
    inherit_property_from([sensor_config, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_original_unit
    for sensor_config in same_unit_sensor_list
]
inherit_schema_for_new_unit_sensors = [
    inherit_property_from([sensor_config, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
    for sensor_config in new_unit_sensor_list
]


FINAL_VALIDATE_SCHEMA = cv.All(
    CONFIG_SCHEMA.extend(
        {cv.Required(CONF_ID): cv.use_id(StatisticsComponent)},
        extra=cv.ALLOW_EXTRA,
    ),
    validate_send_first_at,
    *inherit_schema_for_same_unit_sensors,
    *inherit_schema_for_new_unit_sensors,
    inherit_property_from(
        [CONF_VARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_variance_unit_of_measurement,
    ),
    inherit_property_from(
        [CONF_COVARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_covariance_unit_of_measurement,
    ),
    inherit_property_from(
        [CONF_TREND, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_trend_unit_of_measurement,
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_SOURCE_ID])
    cg.add(var.set_source_sensor(source))

    cg.add(var.set_statistics_type(config[CONF_TYPE]))

    cg.add(var.set_reset_every(config[CONF_RESET_EVERY]))
    cg.add(var.set_window_size(config[CONF_WINDOW_SIZE]))
    cg.add(var.set_send_every(config[CONF_SEND_EVERY]))
    cg.add(var.set_first_at(config[CONF_SEND_FIRST_AT]))

    cg.add(var.set_time_conversion_factor(config[CONF_TIME_UNIT]))

    if CONF_COUNT in config:
        conf = config[CONF_COUNT]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_count_sensor(sens))

    if CONF_MAX in config:
        conf = config[CONF_MAX]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_max_sensor(sens))

    if CONF_MIN in config:
        conf = config[CONF_MIN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_min_sensor(sens))

    if CONF_MEAN in config:
        conf = config[CONF_MEAN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean_sensor(sens))

    if CONF_VARIANCE in config:
        conf = config[CONF_VARIANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_variance_sensor(sens))

    if CONF_STD_DEV in config:
        conf = config[CONF_STD_DEV]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_std_dev_sensor(sens))

    if CONF_COVARIANCE in config:
        conf = config[CONF_COVARIANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_covariance_sensor(sens))

    if CONF_TREND in config:
        conf = config[CONF_TREND]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_trend_sensor(sens))


@automation.register_action(
    "sensor.statistics.reset",
    ResetAction,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(StatisticsComponent),
        }
    ),
)
async def sensor_statistics_reset_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
