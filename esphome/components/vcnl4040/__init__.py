import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_RESOLUTION,
    CONF_ILLUMINANCE,
    CONF_INTEGRATION_TIME,
    CONF_INTERRUPT,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

CONF_VCNL4040_ID = "vcnl4040_id"
# CONF_ILLUMINANCE_INTEGRATION_TIME = "illuminance_integration_time"
# CONF_PROXIMITY_INTEGRATION_TIME = "proximity_integration_time"
CONF_IRED_DUTY = "ired_duty"
CONF_AMBIENT_INTERRUPT_LOWER_BOUND = "ambient_interrupt_lower_bound"
CONF_AMBIENT_INTERRUPT_UPPER_BOUND = "ambient_interrupt_upper_bound"
CONF_PROXIMITY_INTERRUPT_LOWER_BOUND = "proximity_interrupt_lower_bound"
CONF_PROXIMITY_INTERRUPT_UPPER_BOUND = "proximity_interrupt_upper_bound"

CONF_PROXIMITY = "proximity"
CONF_LOW_THRESHOLD = "low_threshold"
CONF_LOW_THRESHOLD_LUX = "low_threshold_lux"
CONF_LOW_THRESHOLD_PERCENTAGE = "low_threshold_percentage"
CONF_HIGH_THRESHOLD = "high_threshold"
CONF_HIGH_THRESHOLD_LUX = "high_threshold_lux"
CONF_HIGH_THRESHOLD_PERCENTAGE = "high_threshold_percentage"

vcnl4040_ns = cg.esphome_ns.namespace("vcnl4040")

AmbientIntegrationTime = vcnl4040_ns.enum("AmbientIntegrationTime")
AmbientIntegrationTime = {
    "80": AmbientIntegrationTime.ALS_80,  # 6535.5 maximum detectable lux
    "160": AmbientIntegrationTime.ALS_160,  # 3276.8 maximum detectable lux
    "320": AmbientIntegrationTime.ALS_320,  # 1638.4 maximum detectable lux
    "640": AmbientIntegrationTime.ALS_640,  # 819.2 maximum detectable lux
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

VCNL4040 = vcnl4040_ns.class_("VCNL4040", cg.PollingComponent, i2c.I2CDevice)


def validate_bounds(config):
    if (
        config[CONF_PROXIMITY_INTERRUPT_UPPER_BOUND]
        < config[CONF_PROXIMITY_INTERRUPT_LOWER_BOUND]
    ):
        raise cv.Invalid(
            f"{CONF_PROXIMITY_INTERRUPT_UPPER_BOUND} must be greater than or equal to {CONF_PROXIMITY_INTERRUPT_UPPER_BOUND}, please adjust."
        )
    return config


def percentage_to_level(percentage, resolution):
    return int(percentage * (2 ** int(resolution)))


def validate_proximity_bounds(config):
    if CONF_INTERRUPT in config:
        conf = config[CONF_INTERRUPT]
        print(config[CONF_INTEGRATION_TIME])
        resolution = int(config[CONF_RESOLUTION])
        lower_bound = 0
        upper_bound = 0

        if CONF_LOW_THRESHOLD in conf:
            lower_bound = conf[CONF_LOW_THRESHOLD]
        else:
            lower_bound = percentage_to_level(
                conf[CONF_LOW_THRESHOLD_PERCENTAGE], resolution
            )
            conf[CONF_LOW_THRESHOLD] = lower_bound

        if CONF_HIGH_THRESHOLD in conf:
            upper_bound = conf[CONF_HIGH_THRESHOLD]
        else:
            upper_bound = percentage_to_level(
                conf[CONF_HIGH_THRESHOLD_PERCENTAGE], resolution
            )
            conf[CONF_HIGH_THRESHOLD] = upper_bound

        print(upper_bound)
        print(lower_bound)
        if upper_bound < lower_bound:
            raise cv.Invalid(
                f"Lower bound for proximity interrupt must be less than upper bound, please adjust."
            )

    return config


def lux_to_level(lux, integration_time):
    return int(lux / (8 / integration_time))


def validate_ambient_bounds(config):
    if CONF_INTERRUPT in config:
        conf = config[CONF_INTERRUPT]

        integration_time = int(config[CONF_INTEGRATION_TIME])

        lower_bound = 0
        upper_bound = 0

        if CONF_LOW_THRESHOLD in conf:
            lower_bound = conf[CONF_LOW_THRESHOLD]
        else:
            lower_bound = lux_to_level(conf[CONF_LOW_THRESHOLD_LUX], integration_time)
            conf[CONF_LOW_THRESHOLD] = lower_bound

        if CONF_HIGH_THRESHOLD in conf:
            upper_bound = conf[CONF_HIGH_THRESHOLD]
        else:
            upper_bound = lux_to_level(conf[CONF_HIGH_THRESHOLD_LUX], integration_time)
            conf[CONF_HIGH_THRESHOLD] = min(upper_bound, 65535)

        if upper_bound < lower_bound:
            raise cv.Invalid(
                f"Lower bound for ambient interrupt must be less than upper bound, please adjust."
            )

    return config


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VCNL4040),
            cv.Optional(CONF_ILLUMINANCE): cv.All(
                cv.Schema(
                    {
                        cv.Optional(CONF_INTEGRATION_TIME, default="80"): cv.enum(
                            AmbientIntegrationTime, upper=True
                        ),
                        cv.Optional(CONF_INTERRUPT): cv.All(
                            cv.Schema(
                                {
                                    cv.Optional(CONF_LOW_THRESHOLD): cv.uint16_t,
                                    cv.Optional(CONF_LOW_THRESHOLD_LUX): cv.float_range(
                                        min=0, max=6535.5
                                    ),
                                    cv.Optional(CONF_HIGH_THRESHOLD): cv.uint16_t,
                                    cv.Optional(
                                        CONF_HIGH_THRESHOLD_LUX
                                    ): cv.float_range(min=0, max=6535.5),
                                }
                            ),
                            cv.has_exactly_one_key(
                                CONF_LOW_THRESHOLD, CONF_LOW_THRESHOLD_LUX
                            ),
                            cv.has_exactly_one_key(
                                CONF_HIGH_THRESHOLD, CONF_HIGH_THRESHOLD_LUX
                            ),
                        ),
                    }
                ),
                validate_ambient_bounds,
            ),
            cv.Optional(CONF_PROXIMITY): cv.All(
                cv.Schema(
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
                        cv.Optional(CONF_INTERRUPT): cv.All(
                            cv.Schema(
                                {
                                    cv.Optional(CONF_LOW_THRESHOLD): cv.uint16_t,
                                    cv.Optional(
                                        CONF_LOW_THRESHOLD_PERCENTAGE
                                    ): cv.percentage,
                                    cv.Optional(CONF_HIGH_THRESHOLD): cv.uint16_t,
                                    cv.Optional(
                                        CONF_HIGH_THRESHOLD_PERCENTAGE
                                    ): cv.percentage,
                                }
                            ),
                            cv.has_exactly_one_key(
                                CONF_LOW_THRESHOLD, CONF_LOW_THRESHOLD_PERCENTAGE
                            ),
                            cv.has_exactly_one_key(
                                CONF_HIGH_THRESHOLD, CONF_HIGH_THRESHOLD_PERCENTAGE
                            ),
                        ),
                    }
                ),
                validate_proximity_bounds,
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
        cg.add(var.set_als_integration_time_config(conf[CONF_INTEGRATION_TIME]))

        if CONF_INTERRUPT in conf:
            interrupt_config = conf[CONF_INTERRUPT]

            cg.add(
                var.set_ambient_interrupt_lower_bound(
                    interrupt_config[CONF_LOW_THRESHOLD]
                )
            )
            cg.add(
                var.set_ambient_interrupt_upper_bound(
                    interrupt_config[CONF_HIGH_THRESHOLD]
                )
            )

    if CONF_PROXIMITY in config:
        conf = config[CONF_PROXIMITY]
        cg.add(var.set_proximity_integration_time_config(conf[CONF_INTEGRATION_TIME]))
        cg.add(var.set_ired_duty_config(conf[CONF_IRED_DUTY]))
        cg.add(var.set_proximity_output_resolution(conf[CONF_RESOLUTION]))

        if CONF_INTERRUPT in conf:
            interrupt_config = conf[CONF_INTERRUPT]

            cg.add(
                var.set_proximity_close_event_lower_bound(
                    interrupt_config[CONF_LOW_THRESHOLD]
                )
            )
            cg.add(
                var.set_proximity_far_event_upper_bound(
                    interrupt_config[CONF_HIGH_THRESHOLD]
                )
            )
