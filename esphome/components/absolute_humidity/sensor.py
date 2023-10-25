import esphome.config_validation as cv

CONFIG_SCHEMA = CONFIG_SCHEMA = cv.invalid(
    "The absolute_humidity sensor has moved.\nPlease use the thermal_comfort platform instead with the absolute_humidity sensor.\n"
    "See https://esphome.io/components/sensor/thermal_comfort.html"
)
