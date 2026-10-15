# smalltv-companion

Composes the SmallTV's screen on your computer and pushes it to the device: music, notifications, Steam, system stats, or anything a plugin can draw. The device keeps the clock ticking by itself.

```sh
pip install ./companion            # or pipx install ./companion
smalltv-companion init             # ~/.config/smalltv-companion/config.toml
smalltv-companion preview -o out.png
smalltv-companion run --open      # settings + live preview at http://127.0.0.1:8765/
```

On the device, choose **Companion app** in the Display tab.

The full guide is in [docs/…/features/companion.md](../docs/src/content/docs/features/companion.md). It covers configuration, the built-in plugins, writing your own plugin, and the protocol.

Development:

```sh
uv venv .venv && uv pip install --python .venv/bin/python -e '.[test]'
.venv/bin/pytest
```
