# AI Usage (GNOME + Windows)

Show **Cursor**, **Claude**, and **Codex** usage from local sessions.

[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![GNOME Shell](https://img.shields.io/badge/GNOME%20Shell-46–50-blue.svg)](https://extensions.gnome.org/)

## GNOME Shell

Minimal top-panel extension for GNOME Shell 46–50.

### Features

- Cursor Auto / API meters
- Claude 5h / 7d OAuth usage
- Codex primary / weekly rate limits
- A minimal menu: one line per provider with its plan tier and overall
  percentage, then a thin bar per pool with its reset countdown
- Panel ring gauge that turns amber past 75% and red past 90%
- Optional billing / credits line
- Prefs for providers, panel target, refresh, proxy

### Auth (reuses what the CLIs already stored — no separate login)

| Provider | Source |
| --- | --- |
| Cursor | `~/.config/cursor/auth.json`, `CURSOR_SESSION_TOKEN`, or desktop `state.vscdb` |
| Claude | `~/.claude/.credentials.json` or `CLAUDE_CODE_OAUTH_TOKEN` |
| Codex | `~/.codex/auth.json` |

Claude OAuth tokens expire. When the stored one has aged out, the extension
renews it with the saved refresh token and writes the rotated pair back to
`.credentials.json`, so the CLI keeps working and you never have to sign in
again just for the meter. If the file cannot be written, the renewal is
abandoned rather than risk invalidating the CLI's login.

### Install

From [releases](https://github.com/byte4day/AI-Usage-Extension/releases):

```bash
gnome-extensions install -f cursor-usage@byte4day.github.io.shell-extension.zip
gnome-extensions enable cursor-usage@byte4day.github.io
```

Reload Shell (Wayland logout, or X11 `Alt+F2` → `r`).

## Windows widget

Floating Win32 card + tray icon. Same providers and auth idea, including the
silent Claude token renewal.

Binary: `CursorUsage.exe` on the release page, or build under [`windows/`](windows/).

See [windows/README.md](windows/README.md) for build steps (CMake + MSVC or MinGW).

## Disclaimer

Not affiliated with Cursor, Anthropic, or OpenAI.

## License

[MIT](LICENSE)
