# Flipper Claude Code Buddy

Turn your Flipper Zero into a physical companion for Claude Code. Get tactile feedback for every AI action — feel the difference between a completed task, an error, and an approval request — and control Claude with real buttons instead of typing.

> Supports macOS and Linux. Windows is not tested.

## What it does

**You feel what Claude is doing.**
Every significant event triggers a distinct sound and vibration pattern on your Flipper, so you always know Claude's status even when you're not looking at the screen.

**You control Claude with physical buttons.**
Interrupt a runaway task, submit a prompt, trigger voice dictation, or open the slash command menu — all without touching the keyboard.

## Two modes

Claude Buddy runs in one of two modes, switchable from the on-device menu (long-press **Right → MENU**, then the top row):

- **Claude Code (USB/BLE)** (default) — talks to Claude Code in the terminal via the companion plugin and Python host bridge. USB or BLE. Full keystroke forwarding: Enter, Esc, voice dictation, slash-command menu, etc.
- **Claude Desktop (BLE)** — talks directly to the Claude Desktop app over BLE using Anthropic's [Hardware Buddy](https://github.com/anthropics/claude-desktop-buddy) protocol (Nordic UART Service). No plugin, no host bridge. The Flipper shows live status from Claude Desktop (running sessions, token counts, recent transcript) and lets you Allow / Deny permission prompts right from the device.

### Enabling Hardware Buddy mode in Claude Desktop

1. On the Flipper, switch to **Claude Desktop (BLE)** in the info menu.
2. In the Claude Desktop app: **Help → Troubleshooting → Enable Developer Mode**.
3. Open **Developer → Open Hardware Buddy** and pick your Flipper from the scan list. macOS will prompt for Bluetooth permission the first time.

Once paired, Claude Desktop auto-reconnects whenever both sides are online.

## Buttons

| Button | Action |
|--------|--------|
| UP | Start / stop voice dictation |
| UP (hold) | Hold Space for voice input |
| LEFT | Interrupt Claude (Esc) |
| LEFT (hold) | Send Ctrl+C |
| RIGHT | Open slash command menu |
| RIGHT (hold) | Open menu |
| OK | Submit Enter (⏎) |
| OK (hold) | Type "yes" and submit |
| DOWN | Send Down arrow (↓) |
| DOWN (hold) | Toggle mute |
| BACK | Send Backspace (⌫) |
| BACK (hold) | Exit |

## Setup

### 1. Install the Flipper app

Download `claude_buddy.fap` from the [latest release](../../releases/latest) and copy it to your Flipper Zero:

- **Via qFlipper:** SD Card → `apps/USB/` → drag and drop
- **Via SD card:** copy to `apps/USB/` directly

### 2. Install the Claude Code plugin

> **Requires Python 3.10 or higher.** If you're on an older system Python, upgrade first (e.g. via [pyenv](https://github.com/pyenv/pyenv) or [python.org](https://www.python.org/downloads/)), then reinstall the plugin.

```bash
claude plugin marketplace add jxw1102/flipper-claude-buddy
claude plugin install flipper-claude-buddy@flipper-claude-buddy
```

<<<<<<< HEAD
The plugin starts the host bridge automatically on each Claude Code session. Configure transport in `~/.claude/settings.json` under `pluginConfigs → flipper-claude-buddy → options`:

```json
{
  "transport": "ble",           // "auto", "usb", or "ble"
  "bluetoothName": "Flipper-name"   // your Flipper's BLE name (required for BLE)
}
```

**Running the bridge manually** (for testing or standalone use):

```bash
cd plugin/host-bridge
pip3 install -e ".[bt]"
python3 -m bridge --transport ble --flipper "Flipper-name" --log-level info
```

Use `--flipper` or the `FLIPPER_BT_NAME` env var when your Flipper has a custom BLE name.
=======
Claude Code will ask for your connection preference (`auto`, `usb`, or `ble`). Leave everything else empty for auto-detect.
>>>>>>> fd484a53b8e6590cf9c9f679511e116bab1468b7

The plugin starts automatically with every Claude Code session and stops when you close it.

### 3. Launch Claude Buddy on your Flipper

Go to **Applications → USB → Claude Buddy**. You'll hear the startup fanfare when the connection is established.

## Connection

Connects over USB (plug-and-play) or Bluetooth LE — whichever is available. USB is preferred when the cable is plugged in; it falls back to BLE automatically.

<<<<<<< HEAD
**Bridge CLI arguments:**

```text
python3 -m bridge [--transport auto|usb|ble] [--flipper NAME] [--log-level debug|info|warning|error]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--transport` | `auto` | Transport to use: `auto` (USB first, fallback BLE), `usb`, or `ble` |
| `--flipper` | *(env: `FLIPPER_BT_NAME`)* | BLE device name — overrides the scan prefix from "Flipper" to your device's name |
| `--log-level` | `info` | Log verbosity. Use `debug` for diagnosing BLE connection issues |

**macOS — Bluetooth pairing:** accept the pairing prompt on first BLE connect. If pairing fails after a firmware flash, remove the Flipper from System Settings → Bluetooth and re-pair.
=======
**macOS — First-time Bluetooth pairing:** on first BLE connection macOS will pair with the Flipper. Accept the pairing prompt on both sides. If the connection fails after a firmware flash or factory reset, remove the Flipper from System Settings → Bluetooth and let it re-pair.
>>>>>>> fd484a53b8e6590cf9c9f679511e116bab1468b7

**macOS — Bluetooth permission:** Terminal (or your terminal app) must have Bluetooth access. Grant it in System Settings → Privacy & Security → Bluetooth.

**macOS — Accessibility permission (required for keystroke forwarding):** Flipper button presses are delivered to your terminal via AppleScript (`osascript`), which needs Accessibility permission. Grant it in System Settings → Privacy & Security → Accessibility and toggle on your terminal app (Terminal, iTerm2, WezTerm, Alacritty, Ghostty, etc.). Without this, the Flipper will see Claude's status (e.g. "thinking...") but pressing OK, LEFT, RIGHT, etc. will do nothing. If your terminal doesn't prompt automatically, add it manually. The bridge log will show `osascript is not allowed to send keystrokes` when this permission is missing. Voice dictation (UP) also depends on this.

**Linux — USB:** Flipper appears as `/dev/ttyACM*`. No additional drivers needed. If you get a permission error, add your user to the `dialout` group:
```bash
sudo usermod -aG dialout $USER  # log out and back in to apply
```

<<<<<<< HEAD
**Linux — Keystroke forwarding:**

| Display server | Tool | Install |
|---|---|---|
| Wayland (GNOME, KDE, Sway, …) | `ydotool` | `sudo apt install ydotool ydotoold` |
| Wayland (wlroots: Sway, Hyprland) | `wtype` | `sudo apt install wtype` |
| X11 | `xdotool` | `sudo apt install xdotool` |

**Wayland users:** start the `ydotoold` daemon before running the bridge:
```bash
sudo ydotoold &
```
The daemon must stay running while the bridge is active. On Wayland the terminal window must have keyboard focus — programmatic window focusing is not available.

**Linux — BLE:** functional via BlueZ (the `bleak` library). USB is preferred for reliability.

**Requirements:**
- **BlueZ ≥ 5.72** (check with `bluetoothctl --version`). Older versions don't support the `--agent` flag needed for automated pairing.
- **Bluetooth powered on:** `bluetoothctl power on`
- **Flipper advertising:** launch Claude Buddy on the Flipper; the app advertises the bridge serial service (UUID `0x3082`).

**BLUETOOTH PAIRING IS NOT REQUIRED.** The Flipper app uses a non-authenticated BLE serial profile that allows GATT access without pairing. This avoids complex BlueZ agent management.

**Custom device name:** If your Flipper has been renamed from the default "Flipper", specify the name explicitly:

```bash
# Via CLI flag:
python3 -m bridge --transport ble --flipper "Flipper-name"

# Or via environment variable:
FLIPPER_BT_NAME="Flipper-name" python3 -m bridge --transport ble
```

When running through the Claude Code plugin, set the name in `~/.claude/settings.json` (`bluetoothName` option under `flipper-claude-buddy`).

## Host identification protocol

Bridges report which agent is connected via the `host` field on `state` messages:

```json
{"v":1,"t":"state","d":{"claude":true,"host":"cursor"}}
=======
**Linux — Keystroke forwarding:** Flipper button presses are forwarded to your terminal via `xdotool` (X11 only). Install it if needed:
```bash
sudo apt install xdotool
>>>>>>> fd484a53b8e6590cf9c9f679511e116bab1468b7
```
Wayland is not yet supported for keystroke forwarding.

**Linux — BLE:** BLE transport works via BlueZ. Make sure BlueZ is installed and running:
```bash
sudo apt install bluetooth bluez
sudo systemctl enable --now bluetooth
sudo usermod -aG bluetooth $USER  # log out and back in to apply
```

## Troubleshooting

| Problem | Fix |
<<<<<<< HEAD
|---------|-----|
| Flipper not found over USB | Stop other bridges and apps using the serial port (qFlipper, another agent bridge). Set `FLIPPER_SERIAL_PORT` explicitly. |
| Flipper not found over BLE | Ensure Bluetooth is on (`bluetoothctl power on`). The Flipper must be running Claude Buddy and in Bridge mode. Check the BLE device name matches `--flipper` / `FLIPPER_BT_NAME`. |
| BLE connection fails with `AuthenticationFailed` | You are running an older version of the Flipper app. Rebuild with `ufbt build && ufbt launch` — the latest `bridge_profile.c` uses non-authenticated characteristics. |
| BLE connection fails with `org.bluez.Error` | Ensure BlueZ ≥ 5.72. If pairing is required, run `bluetoothctl --agent NoInputNoOutput` and `pair <MAC>` manually, then restart the bridge. |
| Bridge connects but Flipper buttons do nothing (BLE) | Update the Flipper app — an older build may have a race in `transport_bt.c` where `bt_set_status_changed_callback` was registered after advertising, causing `bt->connected` to stay `false`. Rebuild with `ufbt build && ufbt launch`. |
| Bridge manual session: Flipper connects but no Claude interaction | Send `claude_connect` via IPC: `echo '{"action":"claude_connect","project_dir":"'$(pwd)'"}' \| nc -U /tmp/<agent>-flipper-bridge.sock`. When running through the plugin, this is handled automatically by `on-session-start.sh`. |
| Wrong agent label / no label | Restart the bridge after flashing a new FAP. Ensure the bridge sends `host` in `state` messages. |
| No sound on task complete | Check bridge log: `tail -f /tmp/<agent>-flipper-bridge.log` |
| Buttons do nothing (macOS) | Grant Accessibility permission to your terminal app. |
| Buttons do nothing (Linux X11) | Install `xdotool`; focus the terminal window. |
| Buttons do nothing (Linux Wayland) | Install `ydotool` + `ydotoold`; start the daemon with `sudo ydotoold &`; keep the terminal focused. |
| Parse errors / CLI banner in log | The Flipper is running the CLI app, not Claude Buddy — open the buddy FAP and restart the bridge. |

Bridge logs:

| Agent | Log file |
|-------|----------|
| Claude Code | `/tmp/claude-flipper-bridge.log` |
| Codex | `/tmp/codex-flipper-bridge.log` |
| Cursor | `/tmp/cursor-flipper-bridge.log` |

## Repository layout

```
flipper-app/              Flipper Zero FAP (C)
plugin/                   Claude Code plugin + host bridge
flipper-codex-buddy/      Codex plugin + host bridge
flipper-cursor-buddy/     Cursor hooks + host bridge
```

## Development

See [CLAUDE.md](CLAUDE.md) for build commands, architecture, protocol details, and release checklist.
=======
|---|---|
| Flipper not found over USB (macOS) | Make sure no other app (qFlipper, Chrome serial, etc.) is using the port. If it still fails, set `FLIPPER_SERIAL_PORT=/dev/cu.usbmodemXXX` explicitly. |
| Flipper not found over USB (Linux) | Check `ls /dev/ttyACM*` — if empty, try a different USB cable. If the port exists but access is denied, run `sudo usermod -aG dialout $USER` and log out/in. Set `FLIPPER_SERIAL_PORT=/dev/ttyACMX` explicitly if needed. |
| Flipper not found over BLE | Make sure Bluetooth is on and the app is running on the Flipper |
| No sound on task complete | Check that the bridge is running: `cat /tmp/claude-flipper-bridge.log` |
| Buttons do nothing / `osascript is not allowed to send keystrokes` in log (macOS) | Grant your terminal app Accessibility permission in System Settings → Privacy & Security → Accessibility. Terminals like WezTerm, Alacritty, or Ghostty often don't prompt automatically — add them manually. |
>>>>>>> fd484a53b8e6590cf9c9f679511e116bab1468b7

### Updating the host bridge plugin during development

When you modify `plugin/host-bridge/` in this repo, Claude Code does **not** automatically pick up the changes — it runs the bridge from its own plugin copy at `~/.claude/plugins/`. To test your changes, you need to update that copy.

#### One-time setup: editable install (recommended)

Symlink your repo into the marketplace so code changes take effect immediately without copying each time:

```bash
# Remove the marketplace copy and symlink to your repo
rm -rf ~/.claude/plugins/marketplaces/flipper-claude-buddy/plugin/host-bridge
ln -s "$(pwd)/plugin/host-bridge" ~/.claude/plugins/marketplaces/flipper-claude-buddy/plugin/host-bridge

# Install as editable in the plugin's venv
~/.claude/plugins/data/flipper-claude-buddy-flipper-claude-buddy/venv/bin/pip install -e \
  ~/.claude/plugins/marketplaces/flipper-claude-buddy/plugin/host-bridge/

# Restart the bridge so it loads the new code
kill $(cat /tmp/claude-flipper-bridge.pid)
```

After this, any code change you make is live — just restart the bridge.

#### One-off update (no symlink)

If you prefer not to symlink, copy and reinstall each time:

```bash
cp -r plugin/host-bridge/* ~/.claude/plugins/marketplaces/flipper-claude-buddy/plugin/host-bridge/
~/.claude/plugins/data/flipper-claude-buddy-flipper-claude-buddy/venv/bin/pip install \
  ~/.claude/plugins/marketplaces/flipper-claude-buddy/plugin/host-bridge/
kill $(cat /tmp/claude-flipper-bridge.pid)
```

Claude Code restarts the bridge automatically on the next session.

#### Restart shortcut

```bash
kill $(cat /tmp/claude-flipper-bridge.pid)
# Claude Code auto-restarts the bridge when needed
```

Check the bridge loaded correctly: `tail -f /tmp/claude-flipper-bridge.log`

## Support

If you find this useful, consider [buying me a coffee](https://ko-fi.com/jxw1102).

## License

MIT — see [LICENSE](LICENSE).
