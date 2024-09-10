from esphome import pins
import esphome.codegen as cg
from esphome.components import esp32, microphone
from esphome.components.adc import ESP32_VARIANT_ADC1_PIN_TO_CHANNEL, validate_adc_pin
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_NUMBER, CONF_SAMPLE_RATE

from .. import (
    BITS_PER_SAMPLE,
    CONF_BITS_PER_SAMPLE,
    CONF_I2S_AUDIO_ID,
    CONF_I2S_DIN_PIN,
    CONF_I2S_MODE,
    CONF_PRIMARY,
    I2S_MODE_OPTIONS,
    I2SAudioComponent,
    I2SAudioIn,
    _validate_bits,
    i2s_audio_ns,
)

CODEOWNERS = ["@jesserockz"]
DEPENDENCIES = ["i2s_audio"]

CONF_ADC_PIN = "adc_pin"
CONF_ADC_TYPE = "adc_type"
CONF_PDM = "pdm"
CONF_USE_APLL = "use_apll"

I2SAudioMicrophone = i2s_audio_ns.class_(
    "I2SAudioMicrophone", I2SAudioIn, microphone.Microphone, cg.Component
)

i2s_channel_fmt_t = cg.global_ns.enum("i2s_channel_fmt_t")
CHANNELS = {
    "left": i2s_channel_fmt_t.I2S_CHANNEL_FMT_ONLY_LEFT,
    "right": i2s_channel_fmt_t.I2S_CHANNEL_FMT_ONLY_RIGHT,
}

INTERNAL_ADC_VARIANTS = [esp32.const.VARIANT_ESP32]
PDM_VARIANTS = [esp32.const.VARIANT_ESP32, esp32.const.VARIANT_ESP32S3]


def validate_esp32_variant(config):
    variant = esp32.get_esp32_variant()
    if config[CONF_ADC_TYPE] == "external":
        if config[CONF_PDM]:
            if variant not in PDM_VARIANTS:
                raise cv.Invalid(f"{variant} does not support PDM")
        return config
    if config[CONF_ADC_TYPE] == "internal":
        if variant not in INTERNAL_ADC_VARIANTS:
            raise cv.Invalid(f"{variant} does not have an internal ADC")
        return config
    raise NotImplementedError


BASE_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(I2SAudioMicrophone),
        cv.GenerateID(CONF_I2S_AUDIO_ID): cv.use_id(I2SAudioComponent),
        cv.Optional(CONF_CHANNEL, default="right"): cv.enum(CHANNELS),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=1),
        cv.Optional(CONF_BITS_PER_SAMPLE, default="32bit"): cv.All(
            _validate_bits, cv.enum(BITS_PER_SAMPLE)
        ),
        cv.Optional(CONF_USE_APLL, default=False): cv.boolean,
        cv.Optional(CONF_I2S_MODE, default=CONF_PRIMARY): cv.enum(
            I2S_MODE_OPTIONS, lower=True
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            "internal": BASE_SCHEMA.extend(
                {
                    cv.Required(CONF_ADC_PIN): validate_adc_pin,
                }
            ),
            "external": BASE_SCHEMA.extend(
                {
                    cv.Required(CONF_I2S_DIN_PIN): pins.internal_gpio_input_pin_number,
                    cv.Required(CONF_PDM): cv.boolean,
                }
            ),
        },
        key=CONF_ADC_TYPE,
    ),
    validate_esp32_variant,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    await cg.register_parented(var, config[CONF_I2S_AUDIO_ID])

    if config[CONF_ADC_TYPE] == "internal":
        variant = esp32.get_esp32_variant()
        pin_num = config[CONF_ADC_PIN][CONF_NUMBER]
        channel = ESP32_VARIANT_ADC1_PIN_TO_CHANNEL[variant][pin_num]
        cg.add(var.set_adc_channel(channel))
    else:
        cg.add(var.set_din_pin(config[CONF_I2S_DIN_PIN]))
        cg.add(var.set_pdm(config[CONF_PDM]))

    cg.add(var.set_i2s_mode(config[CONF_I2S_MODE]))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_use_apll(config[CONF_USE_APLL]))

    await microphone.register_microphone(var, config)
