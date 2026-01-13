import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.audio_dac import AudioDac
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["i2c"]

es9018k2m_ns = cg.esphome_ns.namespace("es9018k2m")
ES9018K2M = es9018k2m_ns.class_("ES9018K2M", AudioDac, cg.Component, i2c.I2CDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ES9018K2M),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x92))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
