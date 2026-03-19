import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.cpp_generator import MockObjClass

CODEOWNERS = ["@kahrendt"]

IS_PLATFORM_COMPONENT = True

visualizer_ns = cg.esphome_ns.namespace("visualizer")

Visualizer = visualizer_ns.class_("Visualizer")


async def register_visualizer(var, config):
    if not CORE.has_id(config[CONF_ID]):
        var = cg.Pvariable(config[CONF_ID], var)
    CORE.register_platform_component("visualizer", var)
    return var


_VISUALIZER_SCHEMA = cv.Schema({})


def visualizer_schema(
    class_: MockObjClass,
) -> cv.Schema:
    schema = {cv.GenerateID(CONF_ID): cv.declare_id(class_)}

    return _VISUALIZER_SCHEMA.extend(schema)


@coroutine_with_priority(CoroPriority.CORE)
async def to_code(config):
    cg.add_global(visualizer_ns.using)
    cg.add_define("USE_VISUALIZER")
