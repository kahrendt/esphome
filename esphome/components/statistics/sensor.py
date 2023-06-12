import esphome.codegen as cg
import esphome.config_validation as cv
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
    CONF_STATE_CLASS,
)
from esphome.core.entity_helpers import inherit_property_from

CODEOWNERS = ["@kahrendt"]

CONF_MEAN = "mean"
CONF_MAX = "max"
CONF_MIN = "min"
CONF_STD_DEV = "std_dev"
CONF_VARIANCE = "variance"
CONF_TREND = "trend"
CONF_COVARIANCE = "covariance"

statistics_ns = cg.esphome_ns.namespace("statistics")

StatisticsComponent = statistics_ns.class_("StatisticsComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StatisticsComponent),
        cv.Required(CONF_SOURCE_ID): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_WINDOW_SIZE, default=15): cv.positive_not_null_int,
        cv.Optional(CONF_SEND_EVERY, default=15): cv.positive_not_null_int,
        cv.Optional(CONF_SEND_FIRST_AT, default=15): cv.positive_not_null_int,
        cv.Optional(CONF_MEAN): sensor.sensor_schema(),
        cv.Optional(CONF_MAX): sensor.sensor_schema(),
        cv.Optional(CONF_MIN): sensor.sensor_schema(),
        cv.Optional(CONF_STD_DEV): sensor.sensor_schema(),
        cv.Optional(CONF_VARIANCE): sensor.sensor_schema(),
        cv.Optional(CONF_COUNT): sensor.sensor_schema(),
        cv.Optional(CONF_TREND): sensor.sensor_schema(),
        cv.Optional(CONF_COVARIANCE): sensor.sensor_schema(),
    }
).extend(cv.COMPONENT_SCHEMA)

# copied from kalman sensor component
# Inherit some sensor values from the first source, for both the state and the error value
properties_to_inherit = [
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_STATE_CLASS,
]

properties_to_inherit_new_unit = [
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_STATE_CLASS,
]


inherit_schema_for_mean = [
    inherit_property_from([CONF_MEAN, property], CONF_SOURCE_ID)
    for property in properties_to_inherit
]
inherit_schema_for_max = [
    inherit_property_from([CONF_MAX, property], CONF_SOURCE_ID)
    for property in properties_to_inherit
]
inherit_schema_for_min = [
    inherit_property_from([CONF_MIN, property], CONF_SOURCE_ID)
    for property in properties_to_inherit
]
inherit_schema_for_std_dev = [
    inherit_property_from([CONF_STD_DEV, property], CONF_SOURCE_ID)
    for property in properties_to_inherit
]
inherit_schema_for_var = [
    inherit_property_from([CONF_VARIANCE, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
]
inherit_schema_for_cov = [
    inherit_property_from([CONF_COVARIANCE, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
]
inherit_schema_for_trend = [
    inherit_property_from([CONF_TREND, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
]
inherit_schema_for_trend = [
    inherit_property_from([CONF_VARIANCE, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
]


# borrowed from integration sensor
def covariance_unit(uom, config):
    suffix = "⋅s"
    if uom.endswith("/" + suffix):
        return uom[0 : -len("/" + suffix)]
    return uom + suffix


def variance_unit(uom, config):
    return "(" + uom + ")²"


def trend_unit(uom, config):
    denominator = "s"
    return uom + "/" + denominator


FINAL_VALIDATE_SCHEMA = cv.All(
    CONFIG_SCHEMA.extend(
        {cv.Required(CONF_ID): cv.use_id(StatisticsComponent)},
        extra=cv.ALLOW_EXTRA,
    ),
    *inherit_schema_for_max,
    *inherit_schema_for_min,
    *inherit_schema_for_mean,
    *inherit_schema_for_var,
    inherit_property_from(
        [CONF_VARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=variance_unit,
    ),
    *inherit_schema_for_std_dev,
    *inherit_schema_for_cov,
    inherit_property_from(
        [CONF_COVARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=covariance_unit,
    ),
    *inherit_schema_for_trend,
    inherit_property_from(
        [CONF_TREND, CONF_UNIT_OF_MEASUREMENT], CONF_SOURCE_ID, transform=trend_unit
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_SOURCE_ID])
    cg.add(var.set_source_sensor(source))

    cg.add(var.set_window_size(config[CONF_WINDOW_SIZE]))
    cg.add(var.set_send_every(config[CONF_SEND_EVERY]))
    cg.add(var.set_first_at(config[CONF_SEND_FIRST_AT]))

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
