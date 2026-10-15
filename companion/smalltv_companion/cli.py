"""smalltv-companion run | preview | plugins | init"""
from __future__ import annotations

import argparse
import asyncio
import logging
import shutil
import sys
from pathlib import Path

from . import __version__, config
from .app import App
from .device import Device
from .plugins import available_plugins

EXAMPLE = Path(__file__).with_name("config.example.toml")


def _load(args) -> config.Config:
    cfg = config.load(args.config)
    if getattr(args, "host", None):
        cfg.host = args.host
    if getattr(args, "password", None):
        cfg.password = args.password
    return cfg


def cmd_run(args) -> int:
    cfg = _load(args)
    if not cfg.host:
        print("no device: pass --host or set [device] host in", args.config or config.DEFAULT_PATH, file=sys.stderr)
        return 2
    app = App(cfg, Device(cfg.host, cfg.user, cfg.password))
    try:
        asyncio.run(app.run(once=args.once))
    except KeyboardInterrupt:
        pass
    return 0


def cmd_preview(args) -> int:
    """Renders the screen locally (plugins polled once), no device needed."""
    cfg = _load(args)
    app = App(cfg)

    async def go():
        for p in app.plugins.values():
            try:
                await p.start()
                await p.poll()
            except Exception as e:
                logging.warning("plugin %s: %s", p.name, e)
        if args.screen:
            names = [s.name for s in cfg.screens]
            if args.screen not in names:
                raise SystemExit(f"no screen {args.screen!r}; have {names}")
            cfg.screens = [s for s in cfg.screens if s.name == args.screen]
        if args.notify:
            n = app.plugins.get("notifications")
            if n is None:
                raise SystemExit("--notify needs the notifications plugin enabled")
            from .plugins.notifications import Note
            n.add(Note("preview", args.notify, "This is what a notification looks like on the SmallTV."))
            import time
            app.collect_interrupts(time.monotonic())
        img = app.preview()
        for p in app.plugins.values():
            await p.stop()
        return img

    img = asyncio.run(go())
    if args.scale > 1:
        img = img.resize((img.width * args.scale, img.height * args.scale), 0)
    img.save(args.out)
    print(args.out)
    return 0


def cmd_plugins(args) -> int:
    for name, target in sorted(available_plugins().items()):
        print(f"{name:15} {target}")
    return 0


def cmd_init(args) -> int:
    dest = Path(args.config) if args.config else config.DEFAULT_PATH
    if dest.exists() and not args.force:
        print(f"{dest} exists (use --force to overwrite)", file=sys.stderr)
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(EXAMPLE, dest)
    print(dest)
    return 0


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(prog="smalltv-companion", description=__doc__)
    ap.add_argument("--version", action="version", version=__version__)
    ap.add_argument("-c", "--config", help=f"config file (default {config.DEFAULT_PATH})")
    ap.add_argument("-v", "--verbose", action="count", default=0)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="push the screen to the device until stopped")
    r.add_argument("--host", help="device address (overrides the config)")
    r.add_argument("--password", help="web UI password, if the device has one")
    r.add_argument("--once", action="store_true", help="push one frame and exit")
    r.set_defaults(fn=cmd_run)

    p = sub.add_parser("preview", help="render the screen to a PNG without a device")
    p.add_argument("-o", "--out", default="smalltv-preview.png")
    p.add_argument("--screen", help="render this screen instead of the first visible one")
    p.add_argument("--notify", metavar="TEXT", help="render a sample notification")
    p.add_argument("--scale", type=int, default=2)
    p.set_defaults(fn=cmd_preview)

    sub.add_parser("plugins", help="list installed plugins").set_defaults(fn=cmd_plugins)
    i = sub.add_parser("init", help="write an example config")
    i.add_argument("--force", action="store_true")
    i.set_defaults(fn=cmd_init)

    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.WARNING - 10 * min(args.verbose, 2),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    sys.exit(args.fn(args))
