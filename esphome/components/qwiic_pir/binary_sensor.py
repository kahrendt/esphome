from esphome import core
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, binary_sensor
from esphome.const import (
    CONF_DEBOUNCE,
    CONF_RAW,
    DEVICE_CLASS_MOTION,
)

DEPENDENCIES = ["i2c"]
CODEOWNERS = ["@kahrendt"]

qwiic_pir_ns = cg.esphome_ns.namespace("qwiic_pir")


QwiicPIRComponent = qwiic_pir_ns.class_(
    "QwiicPIRComponent", cg.Component, i2c.I2CDevice, binary_sensor.BinarySensor
)

CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(
        QwiicPIRComponent,
        device_class=DEVICE_CLASS_MOTION,
    )
    .extend(
        {
            cv.Optional(CONF_DEBOUNCE, default="5ms"): cv.All(
                cv.time_period,
                cv.Range(max=core.TimePeriod(milliseconds=65535)),
            ),
            cv.Optional(CONF_RAW): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_MOTION,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x12))
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_debounce_time(config[CONF_DEBOUNCE].total_milliseconds))

    if CONF_RAW in config:
        conf = config[CONF_RAW]
        binary_sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(var.set_raw_binary_sensor(binary_sens))
