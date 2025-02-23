"""Snapcast Player Setup."""

import esphome.codegen as cg
from esphome.components import esp32, speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_SPEAKER, PLATFORM_ESP32
from esphome.core import CORE

AUTO_LOAD = ["audio", "bytebuffer", "json", "psram", "socket"]

CODEOWNERS = ["@kahrendt", "@synesthesiam"]
DOMAIN = "file"

TYPE_LOCAL = "local"
TYPE_WEB = "web"

CONF_SERVER_ADDRESS = "server_address"

snapcast_ns = cg.esphome_ns.namespace("snapcast")
SnapcastPlayer = snapcast_ns.class_(
    "SnapcastPlayer",
    cg.Component,
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SnapcastPlayer),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_SERVER_ADDRESS): cv.ipv4address,
            cv.Optional(CONF_PORT, default=1704): cv.int_range(1, 65535),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    cg.add_define("USE_AUDIO_FLAC_SUPPORT", True)

    if CORE.using_esp_idf:
        # Wifi settings based on https://github.com/espressif/esp-adf/issues/297#issuecomment-783811702
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM", 16)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM", 512)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_STATIC_TX_BUFFER", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_TX_BUFFER_TYPE", 0)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_STATIC_TX_BUFFER_NUM", 8)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_CACHE_TX_BUFFER_NUM", 32)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_AMPDU_TX_ENABLED", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_TX_BA_WIN", 16)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_AMPDU_RX_ENABLED", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP32_WIFI_RX_BA_WIN", 32)
        esp32.add_idf_sdkconfig_option("CONFIG_LWIP_MAX_ACTIVE_TCP", 16)
        esp32.add_idf_sdkconfig_option("CONFIG_LWIP_MAX_LISTENING_TCP", 16)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_MAXRTX", 12)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_SYNMAXRTX", 6)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_MSS", 1436)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_MSL", 60000)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_SND_BUF_DEFAULT", 65535)
        esp32.add_idf_sdkconfig_option(
            "CONFIG_TCP_WND_DEFAULT", 65535
        )  # Adjusted from referenced settings to avoid compilation error
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_RECVMBOX_SIZE", 512)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_QUEUE_OOSEQ", True)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_OVERSIZE_MSS", True)
        esp32.add_idf_sdkconfig_option("CONFIG_LWIP_WND_SCALE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_TCP_RCV_SCALE", 3)
        esp32.add_idf_sdkconfig_option("CONFIG_LWIP_TCPIP_RECVMBOX_SIZE", 512)

        # Allocate wifi buffers in PSRAM
        esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP", True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spkr))
    if server_address := config.get[CONF_SERVER_ADDRESS]:
        cg.add(var.set_server_address(server_address))
    cg.add(var.set_server_port(config[CONF_PORT]))
