import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_INTERNAL, CONF_TYPE
from esphome.types import ConfigType

from .. import (
    CONF_SENDSPIN_ID,
    SendspinHub,
    request_metadata_support,
    request_pin_display_support,
    sendspin_ns,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["sendspin"]

SendspinTextSensor = sendspin_ns.class_(
    "SendspinTextSensor",
    text_sensor.TextSensor,
    cg.Component,
)

SendspinTextSensorType = sendspin_ns.enum("SendspinTextSensorType", is_class=True)

METADATA_TYPES = {
    "title": SendspinTextSensorType.TITLE,
    "artist": SendspinTextSensorType.ARTIST,
    "album": SendspinTextSensorType.ALBUM,
    "album_artist": SendspinTextSensorType.ALBUM_ARTIST,
}

# The pairing token embeds the Pairing PSK secret by design (it is what the operator
# transfers into a server); publishing it as an entity exposes it to every API consumer.
PAIRING_TYPES = {
    "pairing_token": SendspinTextSensorType.PAIRING_TOKEN,
    "pairing_pin": SendspinTextSensorType.PAIRING_PIN,
}

SENDSPIN_TEXT_SENSOR_TYPES = {**METADATA_TYPES, **PAIRING_TYPES}


def _request_roles(config: ConfigType) -> ConfigType:
    """Request the necessary Sendspin roles/capabilities for the text sensor."""
    sensor_type = config[CONF_TYPE]
    if sensor_type in METADATA_TYPES:
        request_metadata_support()
    elif sensor_type == "pairing_pin":
        # A pairing_pin text sensor is a way to show the dynamic PIN, so advertise
        # the dynamic_pin pair method even without an on_display_pairing_pin automation.
        request_pin_display_support()

    if sensor_type == "pairing_token" and CONF_INTERNAL not in config:
        # The token IS the pairing secret. A non-internal entity is broadcast to every
        # native-API consumer and lands in Home Assistant's recorder history, so default
        # to internal and make exposure an explicit `internal: false` decision (for a
        # local display, use the entity's state from a lambda; internal entities remain
        # readable on-device).
        config[CONF_INTERNAL] = True

    return config


CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema().extend(
        {
            cv.GenerateID(): cv.declare_id(SendspinTextSensor),
            cv.GenerateID(CONF_SENDSPIN_ID): cv.use_id(SendspinHub),
            cv.Required(CONF_TYPE): cv.one_of(*SENDSPIN_TEXT_SENSOR_TYPES, lower=True),
        }
    ),
    cv.only_on_esp32,
    _request_roles,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SENDSPIN_ID])
    await text_sensor.register_text_sensor(var, config)

    cg.add(var.set_sensor_type(SENDSPIN_TEXT_SENSOR_TYPES[config[CONF_TYPE]]))
