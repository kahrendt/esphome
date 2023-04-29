import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_ILLUMINANCE,
    CONF_INTEGRATION_TIME,
    CONF_RESOLUTION,
    UNIT_LUX,
    DEVICE_CLASS_ILLUMINANCE,
    STATE_CLASS_MEASUREMENT,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["i2c"]

vcnl4040_ns = cg.esphome_ns.namespace("vcnl4040")

AmbientIntegrationTime = vcnl4040_ns.enum("AmbientIntegrationTime")
AmbientIntegrationTime = {
    "80": AmbientIntegrationTime.ALS_80,
    "160": AmbientIntegrationTime.ALS_160,
    "320": AmbientIntegrationTime.ALS_320,
    "640": AmbientIntegrationTime.ALS_640,
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

VCNL4040Component = vcnl4040_ns.class_(
    "VCNL4040Component", cg.PollingComponent, i2c.I2CDevice
)

CONF_PROXIMITY = "proximity"
CONF_IRED_DUTY = "ired_duty"
CONF_WHITE_CHANNEL = "white_channel"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VCNL4040Component),
            cv.Optional(CONF_ILLUMINANCE): sensor.sensor_schema(
                unit_of_measurement=UNIT_LUX,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_ILLUMINANCE,
                state_class=STATE_CLASS_MEASUREMENT,
            ).extend(
                {
                    cv.Optional(CONF_INTEGRATION_TIME, default="80"): cv.enum(
                        AmbientIntegrationTime, upper=True
                    ),
                }
            ),
            cv.Optional(CONF_PROXIMITY): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ).extend(
                {
                    cv.Optional(CONF_INTEGRATION_TIME, default="1"): cv.enum(
                        ProximityIntegrationTime, upper=True
                    ),
                    cv.Optional(CONF_IRED_DUTY, default="1/40"): cv.enum(
                        IREDDuty, upper=True
                    ),
                    cv.Optional(CONF_RESOLUTION, default="12"): cv.enum(
                        ProximityOutputResolution, upper=True
                    ),
                }
            ),
            cv.Optional(CONF_WHITE_CHANNEL): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
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
    if CONF_ILLUMINANCE in config:
        conf = config[CONF_ILLUMINANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_lux_sensor(sens))
        cg.add(var.set_als_integration_time_config(conf[CONF_INTEGRATION_TIME]))

    if CONF_PROXIMITY in config:
        conf = config[CONF_PROXIMITY]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_proximity_sensor(sens))
        cg.add(var.set_proximity_integration_time_config(conf[CONF_INTEGRATION_TIME]))
        cg.add(var.set_ired_duty_config(conf[CONF_IRED_DUTY]))
        cg.add(var.set_proximity_output_resolution(conf[CONF_RESOLUTION]))

    if CONF_WHITE_CHANNEL in config:
        conf = config[CONF_WHITE_CHANNEL]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_white_channel_sensor(sens))
