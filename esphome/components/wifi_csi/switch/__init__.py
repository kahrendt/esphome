import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import (
    DEVICE_CLASS_SWITCH,
    ENTITY_CATEGORY_CONFIG,
)
from .. import (
    CONF_WIFI_CSI_ID,
    wifi_csi_ns,
    WiFiCSIComponent,
)

DEPENDENCIES = ["wifi_csi"]

CONF_TRAINING = "training"

TrainingSwitch = wifi_csi_ns.class_("TrainingSwitch", switch.Switch)


CONFIG_SCHEMA = (
    switch.switch_schema(
        TrainingSwitch,
        device_class=DEVICE_CLASS_SWITCH,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend(
        {
            cv.GenerateID(CONF_WIFI_CSI_ID): cv.use_id(WiFiCSIComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_WIFI_CSI_ID])

    s = await switch.new_switch(config)
    await cg.register_parented(s, config[CONF_WIFI_CSI_ID])
    cg.add(hub.set_training_switch(s))
