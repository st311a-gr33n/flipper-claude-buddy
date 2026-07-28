# Plan: Linux Wayland + BLE Support for Flipper Claude Buddy

## Context

The project supports macOS well but Linux support is incomplete:
1. **Keystroke forwarding** only works on X11 via `xdotool`. Wayland (default on Zorin OS 18, Ubuntu 24.04, Fedora, etc.) is documented as "not yet supported."
2. **BLE transport** works but is labeled "experimental" on Linux with poor error messages.
3. **Session target detection** (`session-target.py`) relies on X11's `WINDOWID` env var.

The user is on Zorin OS 18 (GNOME/Mutter Wayland compositor) and needs full functionality.

## Approach: Multi-backend input with auto-detection

### Decision: ydotool as primary Wayland backend, wtype as fallback

- **`ydotool`** — Works with ANY Wayland compositor (GNOME, KDE, Sway, Hyprland) via `/dev/uinput`. Requires `ydotoold` daemon. Available in Ubuntu/Zorin repos (`apt install ydotool ydotoold`).
- **`wtype`** — Only works with wlroots-based compositors (Sway, Hyprland). Available via `apt install wtype`. Useful as a lighter-weight alternative for wlroots users.
- **Order**: Try ydotool first (widest compat), then wtype, then xdotool (X11 fallback). Give clear install instructions when none found.

### Key design constraints
- Wayland has NO programmatic window focusing — we skip `_focus()` and document that the terminal must be focused.
- All three bridge packages share identical `input.py` — change once, copy to all three.
- `ydotool` uses Linux input event codes (NOT X11 keysyms) — need a separate key mapping table.
- The `ydotoold` daemon must be running (typically as root or user with uinput permissions).

## Files to Modify

### 1. `plugin/host-bridge/bridge/input.py` (primary change, then copy)

**Add Wayland detection helper:**
```python
def _is_wayland() -> bool:
    """Return True if running under a Wayland session."""
    return os.environ.get("XDG_SESSION_TYPE", "").lower() == "wayland" \
           or bool(os.environ.get("WAYLAND_DISPLAY", ""))
```

**Add `YdotoolInputBackend` class:**
- Maps abstract key names → Linux input event codes (not X11 keysyms)
- Maps macOS keycodes → Linux input codes (for `send_modified_keystroke`)
- Maps macOS modifier phrases → Linux modifier keycodes
- `_run_ydotool(args, context)` — async subprocess wrapper (same pattern as `_run_xdotool`)
- `send_ctrl_c()` — press ctrl, press c, release c, release ctrl
- `send_keystroke(key)` — press + release sequence
- `send_text(text)` — `ydotool type` then Enter key
- `send_chars(text)` — `ydotool type` (no Return)
- `send_modified_keystroke(code, mods)` — modifier + key + release
- No `_focus()` or `_window_args()` — Wayland doesn't allow programmatic focus

**Add `WtypeInputBackend` class (wlroots only):**
- Uses XKB key names like `wtype -k Return`
- Include install warning for non-wlroots users

**Update `create_backend()` factory:**
- On Linux + Wayland: try ydotool → wtype → NullBackend
- On Linux + X11: existing xdotool path
- macOS: unchanged

**Update `NullInputBackend._warn()`** to mention Wayland tools.

### 2. `plugin/scripts/session-target.py`

- On Wayland: skip `window_id` (X11-only env var)
- Update check to not reject targets missing only `window_id` on Wayland

### 3. Copy changes to sibling bridge packages

- `input.py`: identical across all three — copy verbatim
- `session-target.py`: same functional changes to each variant

### 4. Documentation updates

- `README.md`: Wayland setup instructions, troubleshooting
- `CLAUDE.md`: Update platform notes table

## Linux Input Event Codes Reference (for ydotool)

| Abstract key | Linux code | Name |
|---|---|---|
| escape | 1 | KEY_ESC |
| backspace | 14 | KEY_BACKSPACE |
| tab | 15 | KEY_TAB |
| return | 28 | KEY_ENTER |
| leftctrl | 29 | KEY_LEFTCTRL |
| leftshift | 42 | KEY_LEFTSHIFT |
| c | 46 | KEY_C |
| leftalt | 56 | KEY_LEFTALT |
| space | 57 | KEY_SPACE |
| up | 103 | KEY_UP |
| page_up | 104 | KEY_PAGEUP |
| left | 105 | KEY_LEFT |
| right | 106 | KEY_RIGHT |
| down | 108 | KEY_DOWN |
| page_down | 109 | KEY_PAGEDOWN |
| leftmeta | 125 | KEY_LEFTMETA |

## Verification Plan

1. Run existing tests: `python3 -m pytest tests/` in each bridge package
2. Test detection logic with environment variable overrides
3. Manual smoke test on Wayland session

**Claude Sessions**
claude --resume 53106784-65b3-4ae5-9045-fb7b5bf36ec4
claude --resume 8e7903d3-bc4b-40c4-804b-592b177d2732
claude --resume 7d2e03e9-6d00-4402-b187-e63c2a945aad

**Opencode Sessions**
opencode -s ses_05ea09482ffexasQaas67eEy44
