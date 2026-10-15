"""Plugin discovery: every entry point in the `smalltv_companion.plugins`
group, the built-in ones included (see pyproject.toml). A third-party package
adds a plugin by declaring its own entry point in that group."""
from __future__ import annotations

import importlib
import logging
from importlib.metadata import entry_points

from .base import Interrupt, Plugin

log = logging.getLogger(__name__)

# Used when the package runs from a source checkout without being installed,
# where no entry points exist.
BUILTIN = {
    "text": "smalltv_companion.plugins.text:TextPlugin",
    "system": "smalltv_companion.plugins.system:SystemPlugin",
    "media": "smalltv_companion.plugins.media:MediaPlugin",
    "notifications": "smalltv_companion.plugins.notifications:NotificationsPlugin",
    "steam": "smalltv_companion.plugins.steam:SteamPlugin",
}


def _load(target: str) -> type[Plugin]:
    module, _, attr = target.partition(":")
    return getattr(importlib.import_module(module), attr)


def available_plugins() -> dict[str, str]:
    found = dict(BUILTIN)
    for ep in entry_points(group="smalltv_companion.plugins"):
        found[ep.name] = ep.value
    return found


def plugin_class(name: str) -> type[Plugin]:
    found = available_plugins()
    if name not in found:
        raise KeyError(f"unknown plugin {name!r}; installed: {', '.join(sorted(found))}")
    return _load(found[name])


__all__ = ["Interrupt", "Plugin", "available_plugins", "plugin_class"]
