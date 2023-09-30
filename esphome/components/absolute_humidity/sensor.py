import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_EQUATION,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_TEMPERATURE,
    ICON_WATER,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_GRAMS_PER_CUBIC_METER,
)

CONF_ABSOLUTE_HUMIDITY = "absolute_humidity"
CONF_DEWPOINT = "dewpoint"
CONF_FROSTPOINT = "frostpoint"

CODEOWNERS = ["@DAVe3283", "@kahrendt"]

absolute_humidity_ns = cg.esphome_ns.namespace("absolute_humidity")
AbsoluteHumidityComponent = absolute_humidity_ns.class_(
    "AbsoluteHumidityComponent", sensor.Sensor, cg.Component
)

SaturationVaporPressureEquation = absolute_humidity_ns.enum(
    "SaturationVaporPressureEquation"
)
EQUATION = {
    "BUCK": SaturationVaporPressureEquation.BUCK,
    "TETENS": SaturationVaporPressureEquation.TETENS,
    "WOBUS": SaturationVaporPressureEquation.WOBUS,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AbsoluteHumidityComponent),
        cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Required(CONF_HUMIDITY): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_EQUATION, default="WOBUS"): cv.enum(EQUATION, upper=True),
        cv.Optional(CONF_ABSOLUTE_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_GRAMS_PER_CUBIC_METER,
            icon=ICON_WATER,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_DEWPOINT): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FROSTPOINT): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    temperature_sensor = await cg.get_variable(config[CONF_TEMPERATURE])
    cg.add(var.set_temperature_sensor(temperature_sensor))

    humidity_sensor = await cg.get_variable(config[CONF_HUMIDITY])
    cg.add(var.set_humidity_sensor(humidity_sensor))

    cg.add(var.set_equation(config[CONF_EQUATION]))

    if absolute_humidity_config := config.get(CONF_ABSOLUTE_HUMIDITY):
        sens = await sensor.new_sensor(absolute_humidity_config)
        cg.add(var.set_absolute_humidity_sensor(sens))

    if dewpoint_config := config.get(CONF_DEWPOINT):
        sens = await sensor.new_sensor(dewpoint_config)
        cg.add(var.set_dewpoint_sensor(sens))

    if frostpoint_config := config.get(CONF_FROSTPOINT):
        sens = await sensor.new_sensor(frostpoint_config)
        cg.add(var.set_frostpoint_sensor(sens))
