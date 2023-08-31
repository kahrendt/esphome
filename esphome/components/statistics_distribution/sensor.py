import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_ID,
    CONF_SOURCE_ID,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_STATE_CLASS,
    # STATE_CLASS_MEASUREMENT,
)
from esphome.core.entity_helpers import inherit_property_from


CODEOWNERS = ["@kahrendt"]

statistics_distribution_ns = cg.esphome_ns.namespace("statistics_distribution")
StatisticsDistributionComponent = statistics_distribution_ns.class_(
    "StatisticsDistributionComponent", cg.PollingComponent
)

CONF_DIGEST_SIZE = "digest_size"
CONF_SCALE_FUNCTION = "scale_function"

CONF_QUANTILE_SENSORS = "quantile_sensors"

ScaleFunctions = statistics_distribution_ns.enum("ScaleFunctions")
SCALE_FUNCTION_OPTIONS = {
    "K1": ScaleFunctions.K1_SCALE,
    "K2": ScaleFunctions.K2_SCALE,
    "K3": ScaleFunctions.K3_SCALE,
}

###################
# Inherit Schemas #
###################

PROPERTIES_TO_INHERIT_FOR_QUANTILE_SENSORS = [
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_ACCURACY_DECIMALS,
    CONF_STATE_CLASS,
]


inherit_schema_for_quantile_sensors = [
    inherit_property_from(["quantile", -1, property], CONF_SOURCE_ID)
    # inherit_property_from([sensor_config, property], CONF_SOURCE_ID)
    for property in PROPERTIES_TO_INHERIT_FOR_QUANTILE_SENSORS
    # for i in range(config.get(CONF_QUANTILE_SENSORS).len())
    # for sensor_config in ["quantile", -1, property]
]


#########################
# Configuration Schemas #
#########################

QUANTILE_SENSOR_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required("quantile"): cv.float_range(min=0, max=1),
    }
)
CDF_SENSOR_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required("value"): cv.float_,
    }
)
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(StatisticsDistributionComponent),
            cv.Required(CONF_SOURCE_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DIGEST_SIZE, default=100): cv.int_range(min=20, max=511),
            cv.Optional(CONF_SCALE_FUNCTION, default="K3"): cv.enum(
                SCALE_FUNCTION_OPTIONS, upper=True
            ),
            cv.Optional("total_weight"): sensor.sensor_schema(),
            cv.Optional(CONF_QUANTILE_SENSORS): cv.All(
                cv.ensure_list(QUANTILE_SENSOR_SCHEMA),
                cv.Length(min=1),
            ),
            cv.Optional("cdf_sensors"): cv.All(
                cv.ensure_list(CDF_SENSOR_SCHEMA),
                cv.Length(min=1),
            ),
        },
    ).extend(cv.polling_component_schema("60s"))
)


# Handles inheriting properties from the source sensor
FINAL_VALIDATE_SCHEMA = cv.All(
    *inherit_schema_for_quantile_sensors,
)

###################
# Code Generation #
###################


async def to_code(config):
    template_args = cg.TemplateArguments(config[CONF_DIGEST_SIZE])

    var = cg.new_Pvariable(config[CONF_ID], template_args)
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_SOURCE_ID])

    cg.add(var.set_source_sensor(source))
    cg.add(var.set_scale_function(config.get(CONF_SCALE_FUNCTION)))

    if weight_sensor := config.get("total_weight"):
        weight_ = await cg.templatable(weight_sensor, template_args, cg.uint8)
        sens = await sensor.new_sensor(weight_)
        # sens = await sensor.new_sensor(weight_sensor)
        cg.add(var.set_total_weight_sensor(sens))

    if quantile_sensor_list := config.get(CONF_QUANTILE_SENSORS):
        for quantile_sensor in quantile_sensor_list:
            quantile_ = await cg.templatable(quantile_sensor, template_args, cg.uint8)
            sens = await sensor.new_sensor(quantile_)
            # sens = await sensor.new_sensor(quantile_sensor)
            cg.add(var.add_quantile_sensor(sens, quantile_sensor.get("quantile")))

    if cdf_sensor_list := config.get("cdf_sensors"):
        for cdf_sensor in cdf_sensor_list:
            cdf_ = await cg.templatable(cdf_sensor, template_args, cg.uint8)
            sens = await sensor.new_sensor(cdf_)
            # sens = await sensor.new_sensor(cdf_sensor)
            cg.add(var.add_cdf_sensor(sens, cdf_sensor.get("value")))
