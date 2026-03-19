"""Sendspin visualizer component for ESPHome."""

import esphome.codegen as cg
from esphome.components import visualizer
import esphome.config_validation as cv
from esphome.const import CONF_ID

from .. import CONF_SENDSPIN_ID, SendspinHub, sendspin_ns

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["visualizer"]

CONF_SPECTRUM = "spectrum"
CONF_N_DISP_BINS = "n_disp_bins"
CONF_SCALE = "scale"
CONF_F_MIN = "f_min"
CONF_F_MAX = "f_max"
CONF_RATE_MAX = "rate_max"
CONF_BUFFER_CAPACITY = "buffer_capacity"
CONF_BATCH_MAX = "batch_max"
CONF_TYPES = "types"

SendspinVisualizer = sendspin_ns.class_(
    "SendspinVisualizer",
    cg.Component,
    visualizer.Visualizer,
)

VisualizerDataType = sendspin_ns.enum("VisualizerDataType", is_class=True)

VISUALIZER_DATA_TYPES = {
    "beat": VisualizerDataType.BEAT,
    "loudness": VisualizerDataType.LOUDNESS,
    "f_peak": VisualizerDataType.F_PEAK,
    "spectrum": VisualizerDataType.SPECTRUM,
}

VisualizerSpectrumScale = sendspin_ns.enum("VisualizerSpectrumScale", is_class=True)

SPECTRUM_SCALES = {
    "mel": VisualizerSpectrumScale.MEL,
    "log": VisualizerSpectrumScale.LOG,
    "lin": VisualizerSpectrumScale.LIN,
}

SPECTRUM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_N_DISP_BINS): cv.int_range(min=1, max=128),
        cv.Optional(CONF_SCALE, default="mel"): cv.enum(SPECTRUM_SCALES, lower=True),
        cv.Optional(CONF_F_MIN, default=40): cv.int_range(min=1, max=22000),
        cv.Optional(CONF_F_MAX, default=16000): cv.int_range(min=1, max=22000),
        cv.Optional(CONF_RATE_MAX, default=30): cv.int_range(min=1, max=120),
    }
)


def _validate_spectrum_config(config):
    types = config.get(CONF_TYPES, [])
    has_spectrum = "spectrum" in types
    has_spectrum_config = CONF_SPECTRUM in config

    if has_spectrum and not has_spectrum_config:
        raise cv.Invalid(
            f"'{CONF_SPECTRUM}' configuration is required when 'spectrum' is in '{CONF_TYPES}'"
        )

    if has_spectrum_config and not has_spectrum:
        raise cv.Invalid(
            f"'spectrum' must be in '{CONF_TYPES}' when '{CONF_SPECTRUM}' configuration is provided"
        )

    return config


CONFIG_SCHEMA = cv.All(
    visualizer.visualizer_schema(SendspinVisualizer)
    .extend(
        {
            cv.GenerateID(CONF_SENDSPIN_ID): cv.use_id(SendspinHub),
            cv.Required(CONF_TYPES): cv.ensure_list(
                cv.enum(VISUALIZER_DATA_TYPES, lower=True)
            ),
            cv.Optional(CONF_SPECTRUM): SPECTRUM_SCHEMA,
            cv.Optional(CONF_BUFFER_CAPACITY, default=8192): cv.int_range(
                min=256, max=65536
            ),
            cv.Optional(CONF_BATCH_MAX, default=4): cv.int_range(min=1, max=32),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_spectrum_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await visualizer.register_visualizer(var, config)

    sendspin_hub = await cg.get_variable(config[CONF_SENDSPIN_ID])
    await cg.register_parented(var, sendspin_hub)

    for data_type in config[CONF_TYPES]:
        cg.add(var.add_data_type(data_type))

    cg.add(var.set_buffer_capacity(config[CONF_BUFFER_CAPACITY]))
    cg.add(var.set_batch_max(config[CONF_BATCH_MAX]))

    if CONF_SPECTRUM in config:
        spec = config[CONF_SPECTRUM]
        cg.add(
            var.set_spectrum_config(
                spec[CONF_N_DISP_BINS],
                spec[CONF_SCALE],
                spec[CONF_F_MIN],
                spec[CONF_F_MAX],
                spec[CONF_RATE_MAX],
            )
        )

    cg.add_define("USE_SENDSPIN_VISUALIZER")
