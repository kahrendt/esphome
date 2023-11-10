import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, sensor

# from esphome.components.esp32 import add_idf_sdkconfig_option

# from esphome.const import (
#     CONF_ID,
# )

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["esp32"]

tflite2_ns = cg.esphome_ns.namespace("tflite2")

TFLiteComponent = tflite2_ns.class_("TFLiteComponent", cg.Component, sensor.Sensor)

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(TFLiteComponent)
#         }
#     )
# )

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(
        TFLiteComponent,
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    esp32.add_idf_component(
        name="esp-tflite-micro",
        repo="https://github.com/espressif/esp-tflite-micro",
        # path="components",
        # components=["esp-radar"],
    )

    cg.add_build_flag("-DTF_LITE_STATIC_MEMORY")
    cg.add_build_flag("-DESP_NN")
    # add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_CSI_ENABLED", True)
