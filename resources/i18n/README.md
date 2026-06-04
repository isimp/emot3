# Translating emot3

emot3's interface text lives in small JSON files in this folder, one per
language (`en.json`, `de.json`, ...). Adding a language is just dropping
in another file — no C++ changes needed. `en.json` is the source of
truth: it always holds every key, and any key a translation leaves out
falls back to the English text automatically.

## Add a language in 4 steps

1. **Copy `en.json`** to `<code>.json`. Use the **ISO 639-1 two-letter
   code** for your language (`fr.json`, `es.json`, `it.json`, `pl.json`,
   ...) — that's what Nexus uses internally, so users who have Nexus set
   to your language will get it picked up automatically. Any string
   technically works (users can still pick a non-standard code from
   emot3's dropdown), but auto-follow-Nexus only resolves when the
   codes match.
2. **Set `"_lang"`** to the language's own name as it should appear in
   the Options dropdown, e.g. `"_lang": "Français"`.
3. **Translate the *values*, never the keys.** Keys (the part left of
   the `:`, like `"main.search_hint"`) are identifiers — leave them
   exactly as-is. Only change the text on the right.
4. **Rebuild** (or run `py -3 tools/gen_rc.py`, which the build does for
   you). The new language is auto-discovered from the filename and shows
   up in *Options > General > Interface language*. Done.

You don't have to translate everything at once — any key you omit just
shows the English text. Delete the `"_note"` line if you like; it's only
a comment.

## Rules that matter

- **Keep `%s` / `%d` placeholders**, in the same order. They get filled
  in at runtime (a name, a count, ...). `"Category \"%s\" already
  exists."` → the `%s` must stay.
- **Keep `\n` line breaks** where they make sense for multi-line
  tooltips.
- **Accented Latin letters are fine** (`ä ö ü ß`, `é è ç à`, `ñ ¿ ¡`,
  etc.) — the font covers them. **Avoid** the em dash `—`, ellipsis `…`,
  and arrows `→`; they render as `?`. Use `-`, `...`, `>` instead.
- Save the file as **UTF-8**.

## Emote names and commands are separate

The files here cover emot3's own buttons, tooltips and labels. The emote
**names and slash-commands** come from a different file,
`resources/emote_data/emotes_i18n.json`, which carries a per-language
`{ command, name }` for each bundled emote. To add or fix emote wording
for a language, edit that file (see its `_note` for the format). Emote
command wording is a preference — every variant works in chat regardless
of your game's language.

## Sharing a translation

These files are plain text. Open a pull request (or send the file) with
your `<code>.json` and we can bundle it so everyone gets it.
