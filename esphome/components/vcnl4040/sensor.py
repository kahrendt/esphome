import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ILLUMINANCE,
    UNIT_LUX,
    UNIT_PERCENT,
    DEVICE_CLASS_ILLUMINANCE,
    STATE_CLASS_MEASUREMENT,
)
from . import VCNL4040, CONF_VCNL4040_ID

DEPENDENCIES = ["vcnl4040"]

CONF_PROXIMITY = "proximity"
CONF_IRED_DUTY = "ired_duty"
CONF_WHITE_CHANNEL = "white_channel"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(CONF_VCNL4040_ID): cv.use_id(VCNL4040),
            cv.Optional(CONF_ILLUMINANCE): sensor.sensor_schema(
                unit_of_measurement=UNIT_LUX,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_ILLUMINANCE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PROXIMITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=3,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_WHITE_CHANNEL): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VCNL4040_ID])
    if CONF_ILLUMINANCE in config:
        conf = config[CONF_ILLUMINANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(getattr(hub, "set_lux_sensor")(sens))

    if CONF_PROXIMITY in config:
        conf = config[CONF_PROXIMITY]
        sens = await sensor.new_sensor(conf)
        cg.add(getattr(hub, "set_proximity_sensor")(sens))
        
    if CONF_WHITE_CHANNEL in config:
        cg.add(getattr(hub, "set_white_channel_sensor")(sens))
