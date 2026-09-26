# AI Usage

See **Cursor, Claude Code, and Codex** usage without opening three apps. AI Usage provides a GNOME Shell panel menu on Linux and a floating desktop widget with a tray icon on Windows. It reads existing local sign-ins and requests usage from each provider.

[Download the latest release](https://github.com/byte4day/AI-Usage-Extension/releases/latest) · [Report a problem](https://github.com/byte4day/AI-Usage-Extension/issues) · [MIT license](LICENSE)

## Compatibility and features

| | Linux | Windows |
| --- | --- | --- |
| Interface | GNOME Shell top-panel menu | Floating widget and tray icon |
| Supported desktop | GNOME Shell 46–50 | Windows 10/11 |
| Usage | Cursor Auto/API, Claude 5h/7d, Codex primary/weekly | Same |
| Settings | GNOME Extensions preferences | Widget or tray Settings |
| Release file | `cursor-usage@byte4day.github.io.shell-extension.zip` | `CursorUsage.exe` |

The percentage is **used** by default; you can switch to **remaining**. Missing pools appear as unavailable rather than 0%. A reset countdown appears when the provider returns a reset time. The ring and warning colors use 75% and 90% thresholds. Optional billing or credits lines appear only when the provider returns those fields.

## Install on Linux

This is a **GNOME Shell extension**, not a general Linux desktop app. Sign in to the provider CLIs you want to show, then download the GNOME ZIP from the [latest release](https://github.com/byte4day/AI-Usage-Extension/releases/latest). In its download directory, run:

```bash
gnome-extensions install -f cursor-usage@byte4day.github.io.shell-extension.zip
gnome-extensions enable cursor-usage@byte4day.github.io
```

On Wayland, log out and back in if the indicator does not appear. On X11, press `Alt+F2`, type `r`, and press Enter. Open **Extensions → AI Usage → Preferences** to choose providers and display options.

To update, install a newer ZIP with `-f` and reload GNOME Shell. To remove it, run `gnome-extensions uninstall cursor-usage@byte4day.github.io`.

To build from source, install `gnome-extensions`, `glib-compile-schemas`, and `zip`, then run `./pack` from the repository root. For a local source install, run `./update` and enable the extension.

## Install on Windows

Sign in to the provider CLIs you want to show. Download `CursorUsage.exe` from the [latest release](https://github.com/byte4day/AI-Usage-Extension/releases/latest) and run it. No installer is required. Open **Settings** from the widget or tray icon to choose providers, compact readout, opacity, always-on-top, proxy, and used or remaining percentages.

Preferences are saved at `%APPDATA%\CursorUsage\config.ini`. To reset them, exit the widget and delete that file. To uninstall, exit and delete the executable and, if desired, the configuration directory. Windows may warn about an unsigned downloadable executable; inspect the source or build it yourself if you prefer.

To build, install CMake and Visual Studio 2022 with the C++ desktop workload:

```powershell
cmake -S windows -B windows/build -G "Visual Studio 17 2022" -A x64
cmake --build windows/build --config Release
```

The output is `windows/build/Release/CursorUsage.exe`. MinGW cross-build instructions are in [windows/README.md](windows/README.md).

## Authentication and privacy

AI Usage uses credentials already saved by the provider tools. You may enable any combination of providers.

| Provider | Linux sources | Windows sources |
| --- | --- | --- |
| Cursor | `CURSOR_SESSION_TOKEN`, `~/.config/cursor/auth.json`, or Cursor desktop `state.vscdb` (requires `sqlite3`) | `CURSOR_SESSION_TOKEN`, Cursor auth files, or desktop `state.vscdb` |
| Claude Code | `CLAUDE_CODE_OAUTH_TOKEN` or `~/.claude/.credentials.json` | `CLAUDE_CODE_OAUTH_TOKEN`, Claude credentials files, or Windows Credential Manager |
| Codex | `~/.codex/auth.json` | `%USERPROFILE%\.codex\auth.json` |

Environment variable tokens take precedence over files. A bare Claude environment token cannot be renewed because it has no stored refresh token. When a stored Claude token expires, AI Usage may renew it and write rotated credentials back. If the write fails after the server rotates a token, you may need to sign in to Claude Code again; keep the credential store writable.

Usage requests go directly to provider HTTPS endpoints or your configured proxy. This project has no separate telemetry service. Provider APIs and local credential formats can change, which may require an app update.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Login required, No auth, or Session expired | Sign in through the relevant CLI, then refresh AI Usage. |
| Claude asks to reauthenticate | Run `claude` and complete sign-in. Check that its credential store is writable. |
| Cursor desktop session is not found on Linux | Install `sqlite3`, then refresh. |
| All providers show errors | Check connectivity and proxy URL. A `429` response means wait before refreshing. |
| A pool is missing | The provider may not return that window for your plan. |
| GNOME indicator is absent | Check that the extension is enabled and reload GNOME Shell. |
| Windows widget is hidden | Check the system tray; Settings can enable **Start hidden in the tray**. |

For bug reports, include your OS version, GNOME Shell version if relevant, release, provider, and visible error. **Do not attach credential files or tokens.**

## Development and releases

The GNOME code is in `extension.js`, `prefs.js`, and `stylesheet.css`. The Windows C++17 Win32 code is in `windows/`. GitHub Actions checks JavaScript syntax and the GNOME schema, builds both packages, and publishes assets when a `v*` tag is pushed.

## Disclaimer and license

AI Usage is independent of Cursor, Anthropic, and OpenAI. Their names identify the services whose usage is displayed. Licensed under [MIT](LICENSE).
