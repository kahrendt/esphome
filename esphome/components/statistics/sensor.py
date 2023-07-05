import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import sensor
from esphome.const import (
    CONF_ACCURACY_DECIMALS,
    CONF_COUNT,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_ID,
    CONF_SEND_EVERY,
    CONF_SEND_FIRST_AT,
    CONF_SOURCE_ID,
    CONF_TYPE,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_WINDOW_SIZE,
    DEVICE_CLASS_DURATION,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL,
    UNIT_MILLISECOND,
)
from esphome.core.entity_helpers import inherit_property_from

CODEOWNERS = ["@kahrendt"]

statistics_ns = cg.esphome_ns.namespace("statistics")
StatisticsComponent = statistics_ns.class_("StatisticsComponent", cg.Component)

# Definable sensors
CONF_COVARIANCE = "covariance"
CONF_DURATION = "duration"
CONF_MAX = "max"
CONF_MEAN = "mean"
CONF_MIN = "min"
CONF_STD_DEV = "std_dev"
CONF_TREND = "trend"
CONF_VARIANCE = "variance"

# TEMPORARY DEBUGGING
CONF_MEAN2 = "mean2"
CONF_MEAN3 = "mean3"
CONF_MEAN4 = "mean4"

# Configuration options for aggregate chunks
CONF_CHUNK_SIZE = "chunk_size"
CONF_CHUNK_DURATION_SIZE = "chunk_duration_size"
CONF_CHUNK_QUANTITY = "chunk_quantity"
CONF_RESET_EVERY_CHUNK = "reset_every_chunk"

# Type of measurement group; i.e., are the observations for a population or sample
CONF_GROUP_TYPE = "group_type"
CONF_SAMPLE_GROUP = "sample"
CONF_POPULATION_GROUP = "population"

GroupType = statistics_ns.enum("GroupType")
GROUP_TYPES = {
    CONF_SAMPLE_GROUP: GroupType.SAMPLE_GROUP_TYPE,
    CONF_POPULATION_GROUP: GroupType.POPULATION_GROUP_TYPE,
}

# Different types of statistics possible
CONF_SLIDING_WINDOW = "sliding_window"
CONF_CHUNKED_SLIDING_WINDOW = "chunked_sliding_window"
CONF_CONTINUOUS = "continuous"
CONF_CHUNKED_CONTINUOUS = "chunked_continuous"

StatisticsType = statistics_ns.enum("StatisticsType")
STATISTICS_TYPES = {
    CONF_SLIDING_WINDOW: StatisticsType.STATISTICS_TYPE_SLIDING_WINDOW,
    CONF_CHUNKED_SLIDING_WINDOW: StatisticsType.STATISTICS_TYPE_CHUNKED_SLIDING_WINDOW,
    CONF_CONTINUOUS: StatisticsType.STATISTICS_TYPE_CONTINUOUS,
    CONF_CHUNKED_CONTINUOUS: StatisticsType.STATISTICS_TYPE_CHUNKED_CONTINUOUS,
}

# Types of average; simple has every observation with the same weight, time_weighted weighs each observation by the duration of the observation
CONF_AVERAGE_TYPE = "average_type"
CONF_SIMPLE_AVERAGE = "simple"
CONF_TIME_WEIGHTED_AVERAGE = "time_weighted"

AverageType = statistics_ns.enum("AverageType")
AVERAGE_TYPES = {
    CONF_SIMPLE_AVERAGE: AverageType.SIMPLE_AVERAGE,
    CONF_TIME_WEIGHTED_AVERAGE: AverageType.TIME_WEIGHTED_AVERAGE,
}

# Level of precision to store in the aggregate queues
CONF_PRECISION = "precision"
CONF_FLOAT = "float"
CONF_DOUBLE = "double"

Precision = statistics_ns.enum("Precision")
PRECISION_TYPES = {
    CONF_FLOAT: Precision.FLOAT_PRECISION,
    CONF_DOUBLE: Precision.DOUBLE_PRECISION,
}

# Time unit used for covariance and trend
CONF_TIME_UNIT = "time_unit"

TimeConversionFactor = statistics_ns.enum("TimeConversionFactor")
TIME_CONVERSION_FACTORS = {
    "ms": TimeConversionFactor.FACTOR_MS,
    "s": TimeConversionFactor.FACTOR_S,
    "min": TimeConversionFactor.FACTOR_MIN,
    "h": TimeConversionFactor.FACTOR_HOUR,
    "d": TimeConversionFactor.FACTOR_DAY,
}

# Reset action that clears all queued aggragates
ResetAction = statistics_ns.class_("ResetAction", automation.Action)


# covarance's unit is original unit of measurement multiplied by time unit of measurement
# borrowed from sensor/ntegration/sensor.py
def transform_covariance_unit_of_measurement(uom, config):
    suffix = config[CONF_TIME_UNIT]
    if uom.endswith("/" + suffix):
        return uom[0 : -len("/" + suffix)]
    return uom + "⋅" + suffix


# variance's unit is original unit of measurement squared
def transform_variance_unit_of_measurement(uom, config):
    return "(" + uom + ")²"


# trend's unit is in original unit of measurement divides by time unit of measurement
def transform_trend_unit_of_measurement(uom, config):
    denominator = config[CONF_TIME_UNIT]
    return uom + "/" + denominator


# borrowed from sensor/__init__.py
def validate_send_first_at(config):
    value = config["window_parameters"]
    send_first_at = value.get(CONF_SEND_FIRST_AT)
    send_every = value[CONF_SEND_EVERY]
    if send_first_at is not None and send_first_at > send_every:
        raise cv.Invalid(
            f"send_first_at must be smaller than or equal to send_every! {send_first_at} <= {send_every}"
        )
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StatisticsComponent),
        cv.Required(CONF_SOURCE_ID): cv.use_id(sensor.Sensor),
        cv.Required("window_parameters"): cv.typed_schema(
            {
                CONF_SLIDING_WINDOW: cv.Schema(
                    {
                        cv.Optional(
                            CONF_WINDOW_SIZE, default=15
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_EVERY, default=15
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_FIRST_AT, default=1
                        ): cv.positive_not_null_int,
                    }
                ),
                CONF_CHUNKED_CONTINUOUS: cv.Schema(
                    {
                        cv.Optional(
                            CONF_RESET_EVERY_CHUNK, default=1000
                        ): cv.positive_int,
                        cv.Optional(
                            CONF_CHUNK_SIZE, default=20
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_EVERY, default=15
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_FIRST_AT, default=1
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_CHUNK_DURATION_SIZE, default="60s"
                        ): cv.time_period,
                    }
                ),
                CONF_CHUNKED_SLIDING_WINDOW: cv.Schema(
                    {
                        cv.Optional(CONF_CHUNK_SIZE, default=20): cv.positive_int,
                        cv.Optional(
                            CONF_CHUNK_DURATION_SIZE, default="60s"
                        ): cv.time_period,
                        cv.Optional(
                            CONF_CHUNK_QUANTITY, default=50
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_EVERY, default=15
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_FIRST_AT, default=1
                        ): cv.positive_not_null_int,
                    }
                ),
                CONF_CONTINUOUS: cv.Schema(
                    {
                        cv.Optional(
                            CONF_SEND_EVERY, default=15
                        ): cv.positive_not_null_int,
                        cv.Optional(
                            CONF_SEND_FIRST_AT, default=1
                        ): cv.positive_not_null_int,
                    }
                ),
            }
        ),
        cv.Optional(CONF_AVERAGE_TYPE, default=CONF_SIMPLE_AVERAGE): cv.enum(
            AVERAGE_TYPES, lower=True
        ),
        cv.Optional(CONF_GROUP_TYPE, default=CONF_SAMPLE_GROUP): cv.enum(
            GROUP_TYPES, lower=True
        ),
        cv.Optional(CONF_PRECISION, default=CONF_FLOAT): cv.enum(
            PRECISION_TYPES, lower=True
        ),
        cv.Optional(CONF_TIME_UNIT, default="s"): cv.enum(
            TIME_CONVERSION_FACTORS, lower=True
        ),
        cv.Optional(CONF_COUNT): sensor.sensor_schema(
            state_class=STATE_CLASS_TOTAL,
        ),
        cv.Optional(CONF_COVARIANCE): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_DURATION): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
            device_class=DEVICE_CLASS_DURATION,
            unit_of_measurement=UNIT_MILLISECOND,
        ),
        cv.Optional(CONF_MAX): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MEAN): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MIN): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_STD_DEV): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TREND): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VARIANCE): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MEAN2): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MEAN3): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MEAN4): sensor.sensor_schema(
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    },
).extend(cv.COMPONENT_SCHEMA)

# approach borrowed from kalman sensor component

properties_to_inherit_original_unit = [
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_UNIT_OF_MEASUREMENT,
]

properties_to_inherit_new_unit = [
    CONF_ACCURACY_DECIMALS,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
]

same_unit_sensor_list = [
    CONF_MEAN,
    CONF_MIN,
    CONF_MAX,
    CONF_STD_DEV,
    CONF_MEAN2,
    CONF_MEAN3,
    CONF_MEAN4,
]

new_unit_sensor_list = [CONF_VARIANCE, CONF_COVARIANCE, CONF_TREND]

inherit_schema_for_same_unit_sensors = [
    inherit_property_from([sensor_config, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_original_unit
    for sensor_config in same_unit_sensor_list
]
inherit_schema_for_new_unit_sensors = [
    inherit_property_from([sensor_config, property], CONF_SOURCE_ID)
    for property in properties_to_inherit_new_unit
    for sensor_config in new_unit_sensor_list
]


FINAL_VALIDATE_SCHEMA = cv.All(
    CONFIG_SCHEMA.extend(
        {cv.Required(CONF_ID): cv.use_id(StatisticsComponent)},
        extra=cv.ALLOW_EXTRA,
    ),
    *inherit_schema_for_new_unit_sensors,
    *inherit_schema_for_same_unit_sensors,
    inherit_property_from(
        [CONF_VARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_variance_unit_of_measurement,
    ),
    inherit_property_from(
        [CONF_COVARIANCE, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_covariance_unit_of_measurement,
    ),
    inherit_property_from(
        [CONF_TREND, CONF_UNIT_OF_MEASUREMENT],
        CONF_SOURCE_ID,
        transform=transform_trend_unit_of_measurement,
    ),
    validate_send_first_at,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_SOURCE_ID])
    cg.add(var.set_source_sensor(source))

    cg.add(var.set_average_type(config[CONF_AVERAGE_TYPE]))
    cg.add(var.set_precision(config[CONF_PRECISION]))
    cg.add(var.set_time_conversion_factor(config[CONF_TIME_UNIT]))
    cg.add(var.set_group_type(config[CONF_GROUP_TYPE]))

    conf = config["window_parameters"]

    constant = STATISTICS_TYPES[conf[CONF_TYPE]]
    cg.add(var.set_statistics_type(constant))

    if conf[CONF_TYPE] == CONF_SLIDING_WINDOW:
        cg.add(var.set_window_size(conf[CONF_WINDOW_SIZE]))
    elif conf[CONF_TYPE] == CONF_CHUNKED_CONTINUOUS:
        cg.add(var.set_window_size(conf[CONF_RESET_EVERY_CHUNK]))
        cg.add(var.set_chunk_size(conf[CONF_CHUNK_SIZE]))
        cg.add(
            var.set_chunk_duration_size(
                conf[CONF_CHUNK_DURATION_SIZE].total_milliseconds
            )
        )
    elif conf[CONF_TYPE] == CONF_CHUNKED_SLIDING_WINDOW:
        cg.add(var.set_chunk_size(conf[CONF_CHUNK_SIZE]))
        cg.add(
            var.set_chunk_duration_size(
                conf[CONF_CHUNK_DURATION_SIZE].total_milliseconds
            )
        )
        cg.add(var.set_window_size(conf[CONF_CHUNK_QUANTITY]))

    cg.add(var.set_send_every(conf[CONF_SEND_EVERY]))
    cg.add(var.set_first_at(conf[CONF_SEND_FIRST_AT]))

    if CONF_COUNT in config:
        conf = config[CONF_COUNT]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_count_sensor(sens))

    if CONF_COVARIANCE in config:
        conf = config[CONF_COVARIANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_covariance_sensor(sens))

    if CONF_DURATION in config:
        conf = config[CONF_DURATION]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_duration_sensor(sens))

    if CONF_MAX in config:
        conf = config[CONF_MAX]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_max_sensor(sens))

    if CONF_MEAN in config:
        conf = config[CONF_MEAN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean_sensor(sens))

    if CONF_MIN in config:
        conf = config[CONF_MIN]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_min_sensor(sens))

    if CONF_STD_DEV in config:
        conf = config[CONF_STD_DEV]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_std_dev_sensor(sens))

    if CONF_TREND in config:
        conf = config[CONF_TREND]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_trend_sensor(sens))

    if CONF_VARIANCE in config:
        conf = config[CONF_VARIANCE]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_variance_sensor(sens))

    if CONF_MEAN2 in config:
        conf = config[CONF_MEAN2]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean2_sensor(sens))
    if CONF_MEAN3 in config:
        conf = config[CONF_MEAN3]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean3_sensor(sens))
    if CONF_MEAN4 in config:
        conf = config[CONF_MEAN4]
        sens = await sensor.new_sensor(conf)
        cg.add(var.set_mean4_sensor(sens))


@automation.register_action(
    "sensor.statistics.reset",
    ResetAction,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(StatisticsComponent),
        }
    ),
)
async def sensor_statistics_reset_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
