# Curated overrides for `gen_emote_catalog.py`

This folder is the **curated layer** the catalog builder consults. It is a
*generator input* — it is **not** bundled into the addon. Everything here WINS
over the GW2 API + wiki when the catalog is rebuilt.

## `emote_overrides.json`

Per-emote, optional values that override the auto-derived catalog:

```jsonc
{
  "emotes": {
    "<id>": {
      "is_core": false, "targetable": true, "mad_king": true,   // flag overrides (optional)
      "en": { "name": "Cross Arms", "command": "/crossarms", "aliases": ["/x"] },
      "de": { "name": "Arme kreuzen" }
      // ... per-language name / command / aliases, all optional
    }
  }
}
```

Precedence per field: **override → GW2 API → wiki**. Today this holds the
best-effort display **names** (Sonnet-seeded) and the `mad_king` set; add a
`command`/`aliases`/flag to pin those too.

## `icons/<id>.png`

A **hard icon replacement**. Drop a PNG named after the emote `id` (e.g.
`flex.png`) and the builder uses it verbatim for `resources/emotes_official/<id>.png`:

- It **wins over** the API render icon and the wiki tome icon.
- It **survives `--refresh-icons`** (overrides are never re-downloaded).
- It also lets you give an official icon to an emote the API/wiki can't cover
  (e.g. a core emote that otherwise falls back to AI art).
- Same guards as a download: must be a valid **PNG**, **≤128px** (the addon's
  icon cap). A bad/oversized override is reported and ignored (falls through to
  API/wiki).

Run `py -3 tools/gen_emote_catalog.py` (dry-run) to see what's in effect, then
`--write` to apply.

> Note: `tools/overrides/emote_overrides.json` is distinct from the addon's
> runtime `addons/emot3/emotes.json` (the user catalog) and the bundled
> `resources/emote_data/emotes_i18n.json` (the seed table). This file only feeds
> the generator.
