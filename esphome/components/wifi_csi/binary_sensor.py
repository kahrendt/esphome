import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, binary_sensor
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import (
    DEVICE_CLASS_MOTION,
    DEVICE_CLASS_PRESENCE,
    CONF_MOTION,
    CONF_ID,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["esp32"]

CONF_PRESENCE = "presence"

wifi_csi_ns = cg.esphome_ns.namespace("wifi_csi")

WiFiCSIComponent = wifi_csi_ns.class_(
    "WiFiCSIComponent",
    cg.Component,
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WiFiCSIComponent),
            cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PRESENCE,
            ),
            cv.Optional(CONF_MOTION): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_MOTION,
            ),
        }
    )
)

# CONFIG_SCHEMA = cv.All(
#     binary_sensor.binary_sensor_schema(
#         WiFiCSIComponent,
#         device_class=DEVICE_CLASS_MOTION,
#     ).extend(cv.COMPONENT_SCHEMA),
#     cv.only_with_esp_idf,
# )


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if motion_config := config.get(CONF_MOTION):
        bin_sens = await binary_sensor.new_binary_sensor(motion_config)
        cg.add(var.set_motion_sensor(bin_sens))
    if presence_config := config.get(CONF_PRESENCE):
        bin_sens = await binary_sensor.new_binary_sensor(presence_config)
        cg.add(var.set_presence_sensor(bin_sens))

    esp32.add_idf_component(
        name="esp-radar",
        repo="https://github.com/espressif/esp-csi/",
        path="components",
        components=["esp-radar"],
    )

    add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_CSI_ENABLED", True)
