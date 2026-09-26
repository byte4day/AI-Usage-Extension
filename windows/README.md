# AI Usage (Windows widget)

Floating desktop widget for Windows showing **Cursor**, **Claude**, and **Codex** usage.

Supports Windows 10 and Windows 11. The release executable is `AI-Usage-Extension.exe`.

## Features

- Compact pill (ring + %) that expands to all enabled providers
- Cursor Auto/API, Claude 5h/7d, Codex primary/weekly
- Rounded, shadowed card drawn with per-pixel alpha; hover chrome
- Tray icon, drag + edge snap, always-on-top, opacity
- DPI-scaled preferences window that follows the system light/dark theme

## Auth

No separate sign-in: the widget reads whatever the CLIs already stored.

| Provider | Source |
| --- | --- |
| Cursor | `%USERPROFILE%\.cursor\auth.json`, `CURSOR_SESSION_TOKEN`, or `%APPDATA%\Cursor\...\state.vscdb` |
| Claude | `CLAUDE_CODE_OAUTH_TOKEN`, any `.credentials.json` Claude Code writes (`%USERPROFILE%\.claude`, `%APPDATA%\Claude`, `%LOCALAPPDATA%\Claude`), or the Windows Credential Manager |
| Codex | `%USERPROFILE%\.codex\auth.json` |

### Claude token renewal

Claude Code's OAuth access token expires, which used to leave the widget stuck
on `HTTP 401` until you next ran `claude` by hand. It now renews the token with
the stored refresh token and writes the rotated pair back to the same store it
came from, preserving every other field in the file.

The server may invalidate the old refresh token when it answers. If the widget
cannot save the rotated token, you may need to sign in to Claude Code again.
Keep the credential store writable.

## Build

```powershell
cd windows
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

MinGW cross-compile from Linux:

```bash
cd windows
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

## License

MIT
