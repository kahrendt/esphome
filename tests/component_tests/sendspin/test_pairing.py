"""Validation tests for the sendspin hub's pairing options.

These cover the rejection branches, which a compile test cannot reach: a
`test*.yaml` can only assert that a configuration is accepted.
"""

import base64
from typing import Any

import pytest

from esphome import config_validation as cv
from esphome.components.sendspin import (
    CONF_INITIAL_PAIRING_PSK,
    CONF_INITIAL_STATIC_PIN,
    CONF_INITIAL_UNPAIRED_ACCESS_ENABLED,
    CONFIG_SCHEMA,
    _pairing_psk_id,
)
from esphome.const import PlatformFramework
from esphome.types import ConfigType
from tests.component_tests.types import SetCoreConfigCallable

# Bytes 0x00..0x1f, the same placeholder the compile tests use.
TEST_PSK = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="


def _hub_config(**overrides: Any) -> ConfigType:
    """Build a minimal valid hub config, allowing field overrides."""
    config: ConfigType = {"id": "sendspin_hub_id"}
    config.update(overrides)
    return config


def test_minimal_config_is_accepted(set_core_config: SetCoreConfigCallable) -> None:
    """The baseline the rejection tests vary is itself valid."""
    set_core_config(PlatformFramework.ESP32_IDF)

    config = CONFIG_SCHEMA(_hub_config())

    assert CONF_INITIAL_STATIC_PIN not in config
    assert CONF_INITIAL_PAIRING_PSK not in config


def test_unpaired_access_defaults_on(set_core_config: SetCoreConfigCallable) -> None:
    """Devices ship accepting unpaired (Sentinel) access unless told otherwise.

    Pinned deliberately: this is the one pairing default that decides what an
    unconfigured device allows, so a change to it should fail a test rather than
    ride along silently.
    """
    set_core_config(PlatformFramework.ESP32_IDF)

    assert CONFIG_SCHEMA(_hub_config())[CONF_INITIAL_UNPAIRED_ACCESS_ENABLED] is True
    assert (
        CONFIG_SCHEMA(_hub_config(initial_unpaired_access_enabled=False))[
            CONF_INITIAL_UNPAIRED_ACCESS_ENABLED
        ]
        is False
    )


@pytest.mark.parametrize("pin", ["01234567", "00000000", "99999999"])
def test_static_pin_accepted(set_core_config: SetCoreConfigCallable, pin: str) -> None:
    """Eight decimal digits, leading zeros included, pass through unchanged."""
    set_core_config(PlatformFramework.ESP32_IDF)

    assert (
        CONFIG_SCHEMA(_hub_config(initial_static_pin=pin))[CONF_INITIAL_STATIC_PIN]
        == pin
    )


@pytest.mark.parametrize(
    "pin",
    [
        "0123456",  # too short
        "012345678",  # too long
        "0123456a",  # not all decimal digits
        "0123 567",  # whitespace is not a digit
        "",
    ],
)
def test_static_pin_rejected(set_core_config: SetCoreConfigCallable, pin: str) -> None:
    """Anything that is not exactly eight decimal digits is refused, because the
    library feeds the PIN straight into the PAKE and a malformed one can only ever
    produce pin_mismatch."""
    set_core_config(PlatformFramework.ESP32_IDF)

    with pytest.raises(cv.Invalid, match="exactly 8 decimal digits"):
        CONFIG_SCHEMA(_hub_config(initial_static_pin=pin))


def test_unquoted_static_pin_rejected(set_core_config: SetCoreConfigCallable) -> None:
    """An unquoted YAML PIN arrives as an int, having already lost its leading
    zeros, so it is refused rather than silently pairing with the wrong value."""
    set_core_config(PlatformFramework.ESP32_IDF)

    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA(_hub_config(initial_static_pin=1234567))


def test_pairing_psk_accepted(set_core_config: SetCoreConfigCallable) -> None:
    """A base64 32-byte key passes through as the original base64 text."""
    set_core_config(PlatformFramework.ESP32_IDF)

    config = CONFIG_SCHEMA(_hub_config(initial_pairing_psk=TEST_PSK))

    assert config[CONF_INITIAL_PAIRING_PSK] == TEST_PSK


@pytest.mark.parametrize(
    ("psk", "error"),
    [
        ("not base64!", "using base64"),
        ("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==", "32 bytes long"),  # 31 bytes
        (
            "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g",
            "32 bytes long",
        ),  # 33 bytes
        ("", "32 bytes long"),
    ],
)
def test_pairing_psk_rejected(
    set_core_config: SetCoreConfigCallable, psk: str, error: str
) -> None:
    """Only a base64 32-byte secret is accepted; the length is what the Noise PSK
    slot requires, so a wrong-sized key cannot be caught any later than here."""
    set_core_config(PlatformFramework.ESP32_IDF)

    with pytest.raises(cv.Invalid, match=error):
        CONFIG_SCHEMA(_hub_config(initial_pairing_psk=psk))


def test_pairing_psk_id_derivation() -> None:
    """psk_id is base64url(SHA-256("sendspin-psk-id-v1" || psk)), unpadded.

    The value is pinned rather than recomputed: the library derives it independently
    (RecordStore::psk_id_for) and overrides a stored id that disagrees, so a drift here
    would silently discard the configured id instead of failing.
    """
    assert (
        _pairing_psk_id(base64.b64decode(TEST_PSK))
        == "4Ffu00KzaQRUQSo5glG4-C081HIWzmpBBxLBuP3KJxQ"
    )
