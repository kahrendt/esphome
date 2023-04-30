import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_LIGHT,
)
from . import VCNL4040, CONF_VCNL4040_ID

DEPENDENCIES = ["vcnl4040"]

CONF_BRIGHT_EVENT = "bright_event"
CONF_DARK_EVENT = "dark_event"
CONF_CLOSE_EVENT = "close_event"
CONF_FAR_EVENT = "far_event"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VCNL4040_ID): cv.use_id(VCNL4040),
        cv.Optional(CONF_BRIGHT_EVENT): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_DARK_EVENT): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_CLOSE_EVENT): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_FAR_EVENT): binary_sensor.binary_sensor_schema(),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VCNL4040_ID])
    if CONF_BRIGHT_EVENT in config:
        conf = config[CONF_BRIGHT_EVENT]
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(getattr(hub, "set_bright_event_binary_sensor")(sens))

    if CONF_DARK_EVENT in config:
        conf = config[CONF_DARK_EVENT]
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(getattr(hub, "set_dark_event_binary_sensor")(sens))

    if CONF_CLOSE_EVENT in config:
        conf = config[CONF_CLOSE_EVENT]
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(getattr(hub, "set_close_event_binary_sensor")(sens))

    if CONF_FAR_EVENT in config:
        conf = config[CONF_FAR_EVENT]
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(getattr(hub, "set_far_event_binary_sensor")(sens))
