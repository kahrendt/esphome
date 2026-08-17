"""Validation tests for the sendspin text sensor platform.

These cover behavior a compile test cannot observe: which roles a sensor type
requests, and the fact that a `pairing_token` sensor rewrites its own `internal:`.
"""

from typing import Any

import pytest

from esphome import config_validation as cv
from esphome.components.sendspin import _get_data
from esphome.components.sendspin.text_sensor import (
    CONFIG_SCHEMA,
    METADATA_TYPES,
    PAIRING_TYPES,
)
from esphome.const import CONF_INTERNAL, PlatformFramework
from esphome.types import ConfigType
from tests.component_tests.types import SetCoreConfigCallable


def _sensor_config(**overrides: Any) -> ConfigType:
    """Build a minimal valid text sensor config, allowing field overrides."""
    config: ConfigType = {"name": "Sendspin Text", "type": "title"}
    config.update(overrides)
    return config


@pytest.mark.parametrize("sensor_type", sorted(METADATA_TYPES))
def test_metadata_types_request_metadata_role(
    set_core_config: SetCoreConfigCallable, sensor_type: str
) -> None:
    """Metadata sensors are the only reason the platform needs the metadata role;
    without the request, codegen would compile the role out from under them."""
    set_core_config(PlatformFramework.ESP32_IDF)

    CONFIG_SCHEMA(_sensor_config(type=sensor_type))

    assert _get_data().metadata_support is True
    assert _get_data().pin_display_support is False


@pytest.mark.parametrize("sensor_type", sorted(PAIRING_TYPES))
def test_pairing_types_do_not_request_metadata_role(
    set_core_config: SetCoreConfigCallable, sensor_type: str
) -> None:
    """The pairing types work without any role. This is what makes a pairing-only
    config build with USE_SENDSPIN_METADATA undefined."""
    set_core_config(PlatformFramework.ESP32_IDF)

    CONFIG_SCHEMA(_sensor_config(type=sensor_type))

    assert _get_data().metadata_support is False


def test_pairing_pin_requests_pin_display(
    set_core_config: SetCoreConfigCallable,
) -> None:
    """A pairing_pin sensor is a way to show the dynamic PIN, so it alone makes the
    hub advertise the dynamic_pin pair method."""
    set_core_config(PlatformFramework.ESP32_IDF)

    CONFIG_SCHEMA(_sensor_config(type="pairing_pin"))

    assert _get_data().pin_display_support is True


def test_pairing_token_does_not_request_pin_display(
    set_core_config: SetCoreConfigCallable,
) -> None:
    """The token is not a PIN; publishing it says nothing about whether the device
    can display a dynamic PIN."""
    set_core_config(PlatformFramework.ESP32_IDF)

    CONFIG_SCHEMA(_sensor_config(type="pairing_token"))

    assert _get_data().pin_display_support is False


def test_pairing_token_defaults_to_internal(
    set_core_config: SetCoreConfigCallable,
) -> None:
    """The token embeds the Pairing PSK, so it is withheld from the native API unless
    the user opts in explicitly."""
    set_core_config(PlatformFramework.ESP32_IDF)

    config = CONFIG_SCHEMA(_sensor_config(type="pairing_token"))

    assert config[CONF_INTERNAL] is True


@pytest.mark.parametrize("internal", [True, False])
def test_explicit_internal_wins_for_pairing_token(
    set_core_config: SetCoreConfigCallable, internal: bool
) -> None:
    """The forced default must not override a user who asked for either value, or
    exposing the token on purpose would be impossible."""
    set_core_config(PlatformFramework.ESP32_IDF)

    config = CONFIG_SCHEMA(_sensor_config(type="pairing_token", internal=internal))

    assert config[CONF_INTERNAL] is internal


@pytest.mark.parametrize("sensor_type", ["pairing_pin", *sorted(METADATA_TYPES)])
def test_other_types_are_not_forced_internal(
    set_core_config: SetCoreConfigCallable, sensor_type: str
) -> None:
    """Only the token carries a secret; nothing else should be quietly hidden."""
    set_core_config(PlatformFramework.ESP32_IDF)

    config = CONFIG_SCHEMA(_sensor_config(type=sensor_type))

    assert CONF_INTERNAL not in config


def test_unknown_type_rejected(set_core_config: SetCoreConfigCallable) -> None:
    """A misspelled type must fail rather than fall through to a default."""
    set_core_config(PlatformFramework.ESP32_IDF)

    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA(_sensor_config(type="pairing_secret"))
