"""CPU, memory and temperature of the computer running the companion.

    [plugins.system]
    interval = 3
    sensor = "k10temp"        # optional: psutil sensor name for the temperature
"""
from __future__ import annotations

import psutil

from ..render import Ctx
from .base import Plugin


class SystemPlugin(Plugin):
    name = "system"
    interval = 3.0

    def __init__(self, config=None):
        super().__init__(config)
        self.cpu = self.mem = 0.0
        self.temp: float | None = None
        psutil.cpu_percent()      # first call primes the counter

    def _temperature(self) -> float | None:
        try:
            temps = psutil.sensors_temperatures()
        except (AttributeError, OSError):
            return None
        wanted = self.config.get("sensor")
        for name in ([wanted] if wanted else ["k10temp", "coretemp", "zenpower", "cpu_thermal", "acpitz"]):
            if temps.get(name):
                return temps[name][0].current
        return None

    async def poll(self) -> bool:
        old = (round(self.cpu), round(self.mem), self.temp and round(self.temp))
        self.cpu = psutil.cpu_percent()
        self.mem = psutil.virtual_memory().percent
        self.temp = self._temperature()
        return old != (round(self.cpu), round(self.mem), self.temp and round(self.temp))

    def render(self, ctx: Ctx) -> None:
        rows = [("CPU", self.cpu, f"{self.cpu:.0f}%"), ("RAM", self.mem, f"{self.mem:.0f}%")]
        if self.temp is not None:
            rows.append(("TEMP", min(100.0, self.temp), f"{self.temp:.0f}°C"))
        pad = 14
        step = (ctx.h - 2 * pad) // len(rows)
        for i, (label, pct, value) in enumerate(rows):
            y = pad + i * step
            ctx.text((pad, y), label, 16, fill=ctx.theme.muted, bold=True)
            ctx.text((ctx.w - pad, y), value, 22, bold=True, anchor="ra")
            hot = pct >= 85
            ctx.bar((pad, y + 28, ctx.w - pad, y + 38), pct / 100,
                    fill=(230, 80, 60) if hot else None)
