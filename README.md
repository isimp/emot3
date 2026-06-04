# emot3

A clickable emote panel for Guild Wars 2, with a separate Quickbar you can keep
on your HUD. Built as a [Nexus](https://raidcore.gg/Nexus) addon.

## AI notice

Most of emot3 was written by Claude Code (Anthropic), which did the heavy
lifting on implementation and design. Heads-up so you can judge for yourself.

The bundled AI-generated emote icons are an opt-in fallback (off by default,
under *Options > General > Icons*), marked with a gold frame so you can tell
them from the official ArenaNet artwork.

## Features

- **Library** - search by name, command, or alias; filter by Core / Unlocked /
  Locked; group favorites into your own categories and drag them into order.
- **Built-in categories** - show all Core emotes, everything you've Unlocked, or
  the seasonal Mad King set, alongside your own favorites.
- **Quickbar** - a slim window showing one category at a time, built for your
  HUD: scroll over it to switch categories, let clicks fall through to the game,
  and snap it to a clean grid.
- **Quickbar presets** - save a look plus the bar's size and position, then swap
  between them from the addon's icon.
- **Your own catalog** - the emote list is a file you own, not hard-coded. Add,
  edit, or remove any emote without waiting for an update.
- **Multilingual** - emote commands in all four GW2 languages; UI in English and
  German, with more easy to add.

## The catalog

emot3's emotes live in `addons/emot3/emotes.json` and are fully editable under
*Options > Catalog*. Each emote has a slash command (sent on click), a display
name, a stable id, optional **aliases** (extra commands that also match it in
search and help the unlock sync; never sent), and an optional custom icon.

emot3 ships the full GW2 emote set in four languages plus the official artwork
and seeds it on first run. "Restore built-in emotes" re-adds only what's
missing; your edits are never overwritten. Icons resolve in order: a custom
path, a PNG you drop in `addons/emot3/icons/`, the bundled official art, the
optional AI fallback, then a plain letter - so every emote shows something.

## Install

1. Install [Nexus](https://raidcore.gg/Nexus) into your Guild Wars 2 folder.
2. Put `emot3.dll` (from the latest release) in `<GW2>/addons/`.
3. Launch. emot3 creates `<GW2>/addons/emot3/` and asks which language to seed.

## Notes

- **Sending** opens chat with your GW2 command-chat keybind, types the command,
  and presses Enter. It skips with a short note if chat's open with text, you're
  holding a key, or the keybind isn't set (bind it under *GW2 Options > Control
  Options > User Interface > Chat Command*).
- **Unlock tracking** is manual by default - right-click an unlockable emote to
  toggle. The Unlocks tab can also read your account from the GW2 API (via Hoard
  & Seek or your own key) to fill these in; it only ever adds unlocks.

## Translations

To add a language, copy `resources/i18n/en.json` to `<code>.json`, set the
`_lang` field, translate the values (leave the keys alone), and rebuild. It's
auto-discovered and appears under *Options > General > Interface language*;
missing keys fall back to English, so partial translations are fine. Use the ISO
639-1 two-letter code (`fr`, `es`, `it`, ...). Full steps in
[`resources/i18n/README.md`](resources/i18n/README.md). Emote names and commands
live in `resources/emote_data/emotes_i18n.json`.

## Contributing to the emote catalog

If a slash command is wrong, a display name differs from what the game shows, an emote is missing, or you know a useful alias — open an issue or pull request. The bundled emote data (commands, names, aliases, and flags for all four GW2 languages) lives in [`resources/emote_data/emotes_i18n.json`](resources/emote_data/emotes_i18n.json). The [GW2 Wiki emote page](https://wiki.guildwars2.com/wiki/Emote) is the reference for per-language commands.

## Build

Requirements: Visual Studio 2022 (C++ desktop workload, Win10 SDK), Python 3 for the resource codegen step. Submodules must be cloned.

```sh
git submodule update --init --recursive
py -3 tools/gen_rc.py
msbuild emot3.sln /p:Configuration=Dev /p:Platform=x64
```

Output: `build\Dev\x64\emot3_dev.dll`. Each configuration writes to its own `build\<Configuration>\x64\` folder, so builds never clobber each other and switching configs stays incremental. The MSBuild pre-build target also runs `gen_rc.py` automatically, so the explicit step above is only needed for a clean first build.

### Build configurations

| Configuration | Output DLL | Input-swallows | Dev tools | Use |
|---|---|:---:|:---:|---|
| **Distribution** | `emot3.dll` | — | — | The clean public build. CI publishes this. |
| **Plus** | `emot3_plus.dll` | ✓ | — | Convenience features (mid-movement send, click-through wheel), no dev clutter. |
| **Dev** | `emot3_dev.dll` | ✓ | ✓ | Local diagnostic build — the default for development. |
| **DistributionDevTools** | `emot3_distdevtools.dll` | — | ✓ | Dev tools on a dist-shaped binary (swallows stripped) — diagnose what the public build does. |
| **Debug** | `emot3.dll` | ✓ | ✓ | Unoptimized + debug CRT, for stepping through code. |

CI builds the three shipping configs on every push and pull request, uploads `emot3.dll` + `emot3_plus.dll` as artifacts, and on a `v*` tag publishes `emot3.dll` as a release asset (Nexus auto-updates from a single release DLL; grab `emot3_plus.dll` from the build artifacts). See [`.github/workflows/build.yml`](.github/workflows/build.yml).

Dev builds include diagnostic overlays (performance, Quickbar sizing, runtime state inspector); see [`src/dev/`](src/dev/). Plus/Dev builds include input-swallow features stripped from the public build for AV compatibility; see [`src/dev/QuickbarWheel.cpp`](src/dev/QuickbarWheel.cpp) and [`src/dev/SendSuppress.cpp`](src/dev/SendSuppress.cpp).

## Credits

- [Guild Wars 2](https://www.guildwars2.com/) and the bundled official emote artwork are property of [ArenaNet, LLC](https://www.arena.net/). emot3 is a fan-made tool, not affiliated with or endorsed by ArenaNet.
- The [Guild Wars 2 Wiki](https://wiki.guildwars2.com/wiki/Emote) — source of the built-in emote catalog: the emotes, their per-language slash commands, and display names.
- [Kiroho](https://github.com/Kiroho) and [EmoteTome](https://github.com/Kiroho/EmoteTome) — a great inspiration for this addon.
- [Nexus](https://raidcore.gg/Nexus) by [Raidcore](https://raidcore.gg/) — the addon framework, ImGui host, render hook, keybind plumbing.
- [Dear ImGui](https://github.com/ocornut/imgui) by [Omar Cornut](https://github.com/ocornut) — the immediate-mode UI library every pixel of the panel and Quickbar runs through.
- [nlohmann/json](https://github.com/nlohmann/json) by [Niels Lohmann](https://github.com/nlohmann) — JSON parsing for settings and emote catalogue.
- [GW2 RealTime API (RTAPI)](https://github.com/TyrianDeveloperCollective/GW2-RealTime-API-Releases) — the optional addon emot3 reads for precise can't-emote states (swimming, gliding, downed, and so on); without it emot3 degrades to mounted-only.
- [Hoard & Seek](https://github.com/PieOrCake/hoard_and_seek) — the GW2-API proxy emot3 can use to read your unlocks without ever handling your API key.

## License

[MIT](LICENSE) for emot3's code. ArenaNet retains copyright on the bundled official emote artwork — see the LICENSE file for the breakdown of which assets fall under MIT and which don't.
