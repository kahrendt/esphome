import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_MOTION,
    DEVICE_CLASS_PRESENCE,
    CONF_MOTION,
)
from . import (
    CONF_WIFI_CSI_ID,
    WiFiCSIComponent,
)

DEPENDENCIES = ["wifi_csi"]

CONF_PRESENCE = "presence"


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_WIFI_CSI_ID): cv.use_id(WiFiCSIComponent),
            cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PRESENCE,
            ),
            cv.Optional(CONF_MOTION): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_MOTION,
            ),
        }
    ),
)

# CONFIG_SCHEMA = cv.All(
#     binary_sensor.binary_sensor_schema(
#         WiFiCSIComponent,
#         device_class=DEVICE_CLASS_MOTION,
#     ).extend(cv.COMPONENT_SCHEMA),
#     cv.only_with_esp_idf,
# )


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WIFI_CSI_ID])

    if motion_config := config.get(CONF_MOTION):
        bin_sens = await binary_sensor.new_binary_sensor(motion_config)
        cg.add(hub.set_motion_sensor(bin_sens))
    if presence_config := config.get(CONF_PRESENCE):
        bin_sens = await binary_sensor.new_binary_sensor(presence_config)
        cg.add(hub.set_presence_sensor(bin_sens))
