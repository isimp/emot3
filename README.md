# emot3

A clickable emote panel for Guild Wars 2, with a separate Quickbar you can keep
on your HUD and a type-to-send palette for your keyboard. Built as a
[Nexus](https://raidcore.gg/Nexus) addon.

## AI notice

Most of emot3 was written by Claude Code (Anthropic), which did the heavy
lifting on implementation and design. Heads-up so you can judge for yourself.

The bundled AI-generated emote icons are an opt-in fallback (off by default,
under *Options > General > Icons*), marked with a gold frame so you can tell
them from the official ArenaNet artwork.

## Features

- **Library** - search by name, command, or alias; filter by Core / Unlocked /
  Locked; group favorites into your own categories and drag them into order.
- **Category options** — opt into ready-made Quickbar categories (Core,
  Unlocked, Recently used, Frequently used) alongside your own favorites.
- **Quickbar** - a slim window showing one category at a time, built for your
  HUD: snap it to a clean grid, change scroll behaviour, customize to your
  liking.
- **Quickbar presets** - save a look plus the bar's size and position, then swap
  between them from the addon's icon.
- **Palette** - a keybound type-to-send popup. Suggests your most-used emotes 
  while empty, Tab cycles a result's send variants (target / sync / You / All), 
  and your favorites category names double as search terms. Position, size, 
  grow direction, and behavior are all configurable.
- **Auto-motes** - auto-fire a catalog emote when one of your own chat messages
  contains a trigger word you set (type "lol" → `/laugh`). Off by default;
  per-emote triggers, watched channels, and an anti-spam cooldown are all
  configurable. Needs the optional [Events: Chat](https://github.com/jsantorek/GW2-Chat)
  addon to read chat.
- **Keybinds** - bind individual emotes and /me-mote variants as Nexus keybinds;
  RadialMenus wheels can invoke them without you blocking a key.
- **RadialMenus export** - export a favorites category as a wheel for the
  [RadialMenus](https://github.com/RaidcoreGG/GW2-RadialMenus) addon; emot3
  packages the files, you copy them into RadialMenus folder and hit reload.
- **Your own catalog** - the emote list is a file you own, not hard-coded. Add,
  edit, or remove any emote without waiting for an update. A built-in icon
  picker lets you assign icons easily.
- **/me-motes** - your own custom `/me` chat emotes with up to three
  variants per entry; pick which to fire from the right-click menu.
- **Multilingual** - emote commands in all four GW2 languages; UI in English and
  German, with more easy to add.

## Screenshots

<table>
<tr>
<td align="center" width="33%">
  <a href="https://github.com/user-attachments/assets/9ff450d5-5d1e-4f7b-ba47-e6443c5649e5"><img src="https://github.com/user-attachments/assets/9ff450d5-5d1e-4f7b-ba47-e6443c5649e5" alt="Library" width="280"></a>
  <br><b>Library</b>
</td>
<td align="center" width="33%">
  <a href="https://github.com/user-attachments/assets/70fc159a-481d-42c7-9766-373986d70be1"><img src="https://github.com/user-attachments/assets/70fc159a-481d-42c7-9766-373986d70be1" alt="Quickbar" width="280"></a>
  <br><b>Quickbar</b>
</td>
<td align="center" width="33%">
  <a href="https://github.com/user-attachments/assets/0e5a29de-9fd9-4a93-9b40-2e0cd8332bc4"><img src="https://github.com/user-attachments/assets/0e5a29de-9fd9-4a93-9b40-2e0cd8332bc4" alt="Palette" width="280"></a>
  <br><b>Palette</b>
</td>
</tr>
<tr>
<td align="center" width="33%">
  <a href="https://github.com/user-attachments/assets/28697d89-a665-450a-9fe9-4b46380c5556"><img src="https://github.com/user-attachments/assets/28697d89-a665-450a-9fe9-4b46380c5556" alt="Catalog & /me-motes" width="280"></a>
  <br><b>Catalog & /me-motes</b>
</td>
<td align="center" width="33%">
  <a href="https://github.com/user-attachments/assets/2172af6e-a9c6-40b1-995f-71ce99cfe9b4"><img src="https://github.com/user-attachments/assets/2172af6e-a9c6-40b1-995f-71ce99cfe9b4" alt="Unlocks & RadialMenus" width="280"></a>
  <br><b>Unlocks & RadialMenus</b>
</td>
</tr>
</table>

## The catalog

emot3's emotes live in `addons/emot3/emotes.json` and are fully editable under
*Options > Catalog*. Each emote has a slash command (sent on click), a display
name, optional **aliases** (extra commands that also match it in search and help
the unlock sync), and an optional custom icon.

## /me-motes

emot3's /me-motes are user-authored `/me` chat snippets for things no
animated emote covers ("LFG", "BRB", "Ready to pull"). Each one ships up to
three pre-written bodies — **Default** on left-click, **You** and **All** on
right-click — so you pick the context at send time. The catalog lives in
`addons/emot3/me_motes.json` and is editable under *Options > /me-motes*.


## Install

1. Install [Nexus](https://raidcore.gg/Nexus) into your Guild Wars 2 folder.
2. Put `emot3.dll` (from the latest release) in `<GW2>/addons/`.
3. Launch. emot3 creates `<GW2>/addons/emot3/` and asks which language to seed.

emot3 auto-updates in-game through Nexus.

### Plus build (optional)

`emot3_plus.dll` adds functionality stripped from the public build for antivirus
compatibility: send an emote while moving (the send stops your character —
held movement keys are absorbed and autorun is cancelled) and mouse-wheel routing 
while the Quickbar is click-through. The Plus build also gets a per-wheel **Sync** 
button in the RadialMenus tab that pushes emote changes directly into RadialMenus 
— no manual folder copy needed.

To use it, download `emot3_plus-<version>.zip` from the
[latest release](https://github.com/isimp/emot3/releases/latest), unzip
`emot3_plus.dll` into `<GW2>/addons/`. It appears in Nexus as a separate
**"emot3 (Plus)"** addon that shares the same `addons/emot3/` settings and
catalog as the regular build, so **enable only one of the two at a time**. Plus
does **not** auto-update (and is never overwritten by the public build) - but it
notifies you when to grab a newer zip from the releases page when an update is
available.

## Notes

- **Sending** opens chat with your GW2 command-chat keybind, types the command,
  and presses Enter. It skips with a short note if chat's open with text, 
  you're holding a key, you just sent one (a brief anti-spam cooldown, 
  Options > General > Sending), or the keybind isn't set (bind it under 
  *GW2 Options > Control Options > User Interface > Chat Command*).
- **Unlock tracking** is manual by default - right-click an unlockable emote to
  toggle. The Unlocks tab can also read your account from the GW2 API (via Hoard
  & Seek or your own key) to fill these in; it only ever adds unlocks.
- **Plus features** work by consuming Windows keyboard and mouse-wheel messages
  from inside the game process before they reach the game —
  see [`src/plus/SendSuppress.cpp`](src/plus/SendSuppress.cpp) and
  [`src/plus/QuickbarWheel.cpp`](src/plus/QuickbarWheel.cpp). That
  message-filtering pattern is indistinguishable from a keylogger to static
  analysis, which is why they're kept in a separate opt-in build.

## Translations & catalog fixes

**Emote catalog** — If a slash command is wrong, a display name differs from
what the game shows, an emote is missing, or you know a useful alias, open an
issue or pull request. The bundled emote data lives in
[`resources/emote_data/emotes_i18n.json`](resources/emote_data/emotes_i18n.json);
the [GW2 Wiki emote page](https://wiki.guildwars2.com/wiki/Emote) is the
reference for per-language commands.

**/me-mote translations** — Bundled /me-mote samples ship in English and
German. To add a language, edit
[`resources/me_mote_data/me_motes_i18n.json`](resources/me_mote_data/me_motes_i18n.json).

**UI translations** — Copy `resources/i18n/en.json` to `<code>.json`, set the
`_lang` field, translate the values (leave the keys alone), and rebuild. Use the
ISO 639-1 two-letter code (`fr`, `es`, `it`, ...). Missing keys fall back to
English. Full steps in [`resources/i18n/README.md`](resources/i18n/README.md).

## Build

Build from source (Windows/MSVC or Linux/MinGW cross-compile) — see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- [Guild Wars 2](https://www.guildwars2.com/) and the bundled official emote
  artwork are property of [ArenaNet, LLC](https://www.arena.net/). emot3 is a
  fan-made tool, not affiliated with or endorsed by ArenaNet.
- The [Guild Wars 2 Wiki](https://wiki.guildwars2.com/wiki/Emote) — source of
  the built-in emote catalog: the emotes, their per-language slash commands, and
  display names.
- [Kiroho](https://github.com/Kiroho) and
  [EmoteTome](https://github.com/Kiroho/EmoteTome) — a great inspiration for
  this addon.
- [Nexus](https://raidcore.gg/Nexus) by [Raidcore](https://raidcore.gg/) — the
  addon framework, ImGui host, render hook, keybind plumbing.
- [Dear ImGui](https://github.com/ocornut/imgui) by
  [Omar Cornut](https://github.com/ocornut) — the immediate-mode UI library
  every pixel of the panel and Quickbar runs through.
- [nlohmann/json](https://github.com/nlohmann/json) by
  [Niels Lohmann](https://github.com/nlohmann) — JSON parsing for settings and
  emote catalogue.
- [GW2 RealTime API (RTAPI)](https://github.com/TyrianDeveloperCollective/GW2-RealTime-API-Releases)
  — the optional addon emot3 reads for additional blocked states (downed,
  swimming, underwater, gliding, and flying).
- [Hoard & Seek](https://github.com/PieOrCake/hoard_and_seek) — the GW2-API
  proxy emot3 can use to read your unlocks without ever handling your API key.
- [RadialMenus](https://github.com/RaidcoreGG/GW2-RadialMenus) — the
  wheel-menu addon emot3 can export favorites categories to.
- [Events: Chat](https://github.com/jsantorek/GW2-Chat) — the optional
  addon emot3 subscribes to so auto-motes can read your chat lines.

## License

[MIT](LICENSE) for emot3's code. ArenaNet retains copyright on the bundled
official emote artwork — see the LICENSE file for the breakdown of which
assets fall under MIT and which don't.
