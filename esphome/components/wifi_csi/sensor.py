import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    STATE_CLASS_MEASUREMENT,
)
from . import (
    CONF_WIFI_CSI_ID,
    WiFiCSIComponent,
)

DEPENDENCIES = ["wifi_csi"]

CONF_JITTER = "jitter"
CONF_WANDER = "wander"


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_WIFI_CSI_ID): cv.use_id(WiFiCSIComponent),
            cv.Optional(CONF_JITTER): sensor.sensor_schema(
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=4,
            ),
            cv.Optional(CONF_WANDER): sensor.sensor_schema(
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=4,
            ),
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WIFI_CSI_ID])

    if jitter_config := config.get(CONF_JITTER):
        sens = await sensor.new_sensor(jitter_config)
        cg.add(hub.set_jitter_sensor(sens))
    if wander_config := config.get(CONF_WANDER):
        sens = await sensor.new_sensor(wander_config)
        cg.add(hub.set_wander_sensor(sens))
