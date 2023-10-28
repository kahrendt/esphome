# initially based off of TMP117 component

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, esp32
from esphome.components.esp32 import add_idf_sdkconfig_option

# from esphome.const import (
#     CONF_MODEL,
#     DEVICE_CLASS_WIND_SPEED,
#     STATE_CLASS_MEASUREMENT,
# )

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["esp32"]

wifi_csi_ns = cg.esphome_ns.namespace("wifi_csi")

WiFiCSIComponent = wifi_csi_ns.class_(
    "WiFiCSIComponent", cg.PollingComponent, sensor.Sensor
)

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(
        WiFiCSIComponent,
    ).extend(cv.polling_component_schema("60s")),
    # cv.only_with_esp_idf,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    esp32.add_idf_component(
        name="esp-radar",
        repo="https://github.com/espressif/esp-csi/",
        path="components",
        components=["esp-radar"],
    )

    add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_CSI_ENABLED", True)
