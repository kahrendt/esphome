import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import (
    CONF_ID,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["esp32"]

CONF_WIFI_CSI_ID = "wifi_csi_id"
CONF_PRESENCE = "presence"

wifi_csi_ns = cg.esphome_ns.namespace("wifi_csi")

WiFiCSIComponent = wifi_csi_ns.class_(
    "WiFiCSIComponent",
    cg.PollingComponent,
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WiFiCSIComponent),
        }
    ).extend(cv.polling_component_schema("1s"))
    # cv.only_with_esp_idf,
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

    esp32.add_idf_component(
        name="esp-radar",
        repo="https://github.com/espressif/esp-csi/",
        path="components",
        components=["esp-radar"],
    )

    add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_CSI_ENABLED", True)
