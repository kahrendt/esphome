import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_RESOLUTION,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

CONF_VCNL4040_ID = "vcnl4040_id"
CONF_ILLUMINANCE_INTEGRATION_TIME = "illuminance_integration_time"
CONF_PROXIMITY_INTEGRATION_TIME = "proximity_integration_time"
CONF_IRED_DUTY = "ired_duty"
CONF_AMBIENT_INTERRUPT_LOWER_BOUND = "ambient_interrupt_lower_bound"
CONF_AMBIENT_INTERRUPT_UPPER_BOUND = "ambient_interrupt_upper_bound"
CONF_PROXIMITY_INTERRUPT_LOWER_BOUND = "proximity_interrupt_lower_bound"
CONF_PROXIMITY_INTERRUPT_UPPER_BOUND = "proximity_interrupt_upper_bound"

vcnl4040_ns = cg.esphome_ns.namespace("vcnl4040")

AmbientIntegrationTime = vcnl4040_ns.enum("AmbientIntegrationTime")
AmbientIntegrationTime = {
    "80": AmbientIntegrationTime.ALS_80,            # 6535.5 maximum detectable lux
    "160": AmbientIntegrationTime.ALS_160,          # 3276.8 maximum detectable lux
    "320": AmbientIntegrationTime.ALS_320,          # 1638.4 maximum detectable lux
    "640": AmbientIntegrationTime.ALS_640,          # 819.2 maximum detectable lux
}

IREDDuty = vcnl4040_ns.enum("IREDDuty")
IREDDuty = {
    "1/40": IREDDuty.IRED_DUTY_40,
    "1/80": IREDDuty.IRED_DUTY_80,
    "1/160": IREDDuty.IRED_DUTY_160,
    "1/320": IREDDuty.IRED_DUTY_160,
}

ProximityIntegrationTime = vcnl4040_ns.enum("ProximityIntegrationTime")
ProximityIntegrationTime = {
    "1": ProximityIntegrationTime.PS_IT_1T,
    "1.5": ProximityIntegrationTime.PS_IT_1T5,
    "2": ProximityIntegrationTime.PS_IT_2T,
    "2.5": ProximityIntegrationTime.PS_IT_2T5,
    "3": ProximityIntegrationTime.PS_IT_3T,
    "3.5": ProximityIntegrationTime.PS_IT_3T5,
    "4": ProximityIntegrationTime.PS_IT_4T,
    "8": ProximityIntegrationTime.PS_IT_8T,
}

ProximityOutputResolution = vcnl4040_ns.enum("ProximityOutputResolution")
ProximityOutputResolution = {
    "12": ProximityOutputResolution.PS_RESOLUTION_12,
    "16": ProximityOutputResolution.PS_RESOLUTION_16,
}

VCNL4040 = vcnl4040_ns.class_(
    "VCNL4040", cg.PollingComponent, i2c.I2CDevice
)

def validate_bounds(config):
    if (config[CONF_PROXIMITY_INTERRUPT_UPPER_BOUND] < config[CONF_PROXIMITY_INTERRUPT_LOWER_BOUND]):
        raise cv.Invalid(
            f"{CONF_PROXIMITY_INTERRUPT_UPPER_BOUND} must be greater than or equal to {CONF_PROXIMITY_INTERRUPT_UPPER_BOUND}, please adjust."
        )
    return config

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VCNL4040),
            cv.Optional(CONF_ILLUMINANCE_INTEGRATION_TIME, default="80"): cv.enum(
                AmbientIntegrationTime, upper=True
            ),
            cv.Optional(CONF_AMBIENT_INTERRUPT_LOWER_BOUND, default="0.0"): cv.float_range(min=0, max=6535.5),
            cv.Optional(CONF_AMBIENT_INTERRUPT_UPPER_BOUND, default="6535.5"): cv.float_range(min=0, max=6535.5),            
            cv.Optional(CONF_PROXIMITY_INTEGRATION_TIME, default="1"): cv.enum(
                ProximityIntegrationTime, upper=True
            ),
            cv.Optional(CONF_PROXIMITY_INTERRUPT_LOWER_BOUND, default="0%"): cv.percentage,
            cv.Optional(CONF_PROXIMITY_INTERRUPT_UPPER_BOUND, default="100%"): cv.percentage or cv.int_range(0,65535),               
            cv.Optional(CONF_IRED_DUTY, default="1/40"): cv.enum(
                IREDDuty, upper=True
            ),
            cv.Optional(CONF_RESOLUTION, default="12"): cv.enum(
                ProximityOutputResolution, upper=True
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x60))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    cg.add(var.set_als_integration_time_config(config[CONF_ILLUMINANCE_INTEGRATION_TIME]))
    cg.add(var.set_ambient_interrupt_lower_bound(config[CONF_AMBIENT_INTERRUPT_LOWER_BOUND]))
    cg.add(var.set_ambient_interrupt_upper_bound(config[CONF_AMBIENT_INTERRUPT_UPPER_BOUND]))    
    cg.add(var.set_proximity_integration_time_config(config[CONF_PROXIMITY_INTEGRATION_TIME]))
    cg.add(var.set_ired_duty_config(config[CONF_IRED_DUTY]))
    cg.add(var.set_proximity_output_resolution(config[CONF_RESOLUTION]))
    cg.add(var.set_proximity_close_event_lower_bound_percentage(config[CONF_PROXIMITY_INTERRUPT_LOWER_BOUND]))
    cg.add(var.set_proximity_far_event_upper_bound_percentage(config[CONF_PROXIMITY_INTERRUPT_UPPER_BOUND]))