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
)
from esphome.core.entity_helpers import inherit_property_from

CODEOWNERS = ["@kahrendt"]

CONF_MEAN = "mean"
CONF_MAX = "max"
CONF_MIN = "min"

statistics_ns = cg.esphome_ns.namespace("statistics")


StatisticsComponent = statistics_ns.class_(
    "StatisticsComponent", cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(StatisticsComponent),
            cv.Required(CONF_SOURCE_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_WINDOW_SIZE, default=15): cv.positive_not_null_int,
            cv.Optional(CONF_SEND_EVERY, default=15): cv.positive_not_null_int,
            cv.Optional(CONF_SEND_FIRST_AT, default=1): cv.positive_not_null_int,            
            cv.Optional(CONF_MEAN): sensor.sensor_schema(),
            cv.Optional(CONF_MAX): sensor.sensor_schema(),
            cv.Optional(CONF_MIN): sensor.sensor_schema(),                    
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

## copied from kalman sensor component
# Inherit some sensor values from the first source, for both the state and the error value
properties_to_inherit = [
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
    # CONF_STATE_CLASS could also be inherited, but might lead to unexpected behaviour with "total_increasing"
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

FINAL_VALIDATE_SCHEMA = cv.All(
    CONFIG_SCHEMA.extend(
        {cv.Required(CONF_ID): cv.use_id(StatisticsComponent)},
        extra=cv.ALLOW_EXTRA,
    ),
    *inherit_schema_for_mean,
    *inherit_schema_for_max,
    *inherit_schema_for_min,
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_SOURCE_ID])
    cg.add(var.set_source_sensor(source))
    
    cg.add(var.set_window_size(config[CONF_WINDOW_SIZE]))
    cg.add(var.set_send_every(config[CONF_SEND_EVERY]))
    cg.add(var.set_first_at(config[CONF_SEND_FIRST_AT]))
    
    if CONF_MEAN in config:
        conf = config[CONF_MEAN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean_sensor(sens))
    
    if CONF_MAX in config:
        conf = config[CONF_MAX]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_max_sensor(sens))
        
    if CONF_MIN in config:
        conf = config[CONF_MIN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_min_sensor(sens))
