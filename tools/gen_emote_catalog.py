#!/usr/bin/env python3
"""Rebuild the bundled emote catalog (resources/emote_data/emotes_i18n.json) and
download provenance-clean official icons, by merging the GW2 API with the GW2
wiki in all four client languages (EN/DE/FR/ES).

This is the single, repeatable source of truth for the bundled catalog. It
replaces the old alias-only tools/gen_emote_aliases.py (which only re-derived
per-language aliases from a one-time wiki dump).

WHAT EACH SOURCE GIVES US (verified)
  - /v2/emotes + /v2/items (lang en/de/fr/es): authoritative primary command per
    language for the ~14 *tome-unlockable* emotes (parsed from the item NAME, e.g.
    `"/Rockout" Emote Tome`) plus a render-service icon URL (== the real emote
    icon). Core + festival/gemstore emotes are NOT in this endpoint.
  - EN wiki `Emote` table: the complete catalog -- a 4-column (en/fr/de/es)
    command table with all alias variants, the persistent/sitting/targeted flags,
    and an "Unlock item" cell carrying the tome icon `{{item icon|"X" Emote Tome}}`.
  - DE/FR/ES wiki `Emote` tables: each carries a "command (English)" column that
    joins a localized row back to our id without langlink guessing -- used to UNION
    in any localized alias the EN table's column happened to omit.
  - There is NO localized display-name column on any wiki, and core emotes have no
    wiki icon / usable subpage (their wiki names collide with EoD skiff skills).

SAFETY MODEL
  - Dry-run by default: prints a diff + a report and writes NOTHING. Pass --write
    to actually update the JSON and download icons.
  - Never silently clobbers hand-curated `command`/`name`/flags. Curated values
    win unless you pass the matching --refresh-* flag; conflicts are reported.
  - Icons are only ever ADDED (or refreshed with --refresh-icons); existing icons
    are never deleted, and only icons that trace to the wiki Emote table / API are
    fetched (no fuzzy guessing).
  - Cached HTTP (tools/.emote_cache/, gitignored) so re-runs are cheap/offline.

CLEAN-SLATE MODEL
  The catalog is a pure projection of the sources -- no legacy hand-curated value
  is carried forward. Per language: command = override > API > wiki; aliases = the
  other API/wiki variants (+ override aliases); name = override > best-effort
  derive. Curated overrides live under tools/overrides/: emote_overrides.json is
  the reserved top-priority layer (best-effort display names + manual command/alias/
  flag pins), and icons/<id>.png hard-replaces an emote's icon (wins over API/wiki,
  survives --refresh-icons). A language with no source at all is dropped.

USAGE
    py -3 tools/gen_emote_catalog.py                 # dry-run: diff + report
    py -3 tools/gen_emote_catalog.py --write         # apply JSON + download icons
    py -3 tools/gen_emote_catalog.py --refresh       # force re-fetch (ignore cache)
    py -3 tools/gen_emote_catalog.py --write --refresh-icons   # normalize icons to API
    py -3 tools/gen_emote_catalog.py --add-new       # also add wiki emotes we lack
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
JSON_PATH = os.path.join(REPO, "resources", "emote_data", "emotes_i18n.json")
ICON_DIR = os.path.join(REPO, "resources", "emotes_official")
CACHE_DIR = os.path.join(HERE, ".emote_cache")
# Curated overrides (NOT bundled), grouped under tools/overrides/:
#   emote_overrides.json -- top-priority command/name/aliases/flags per emote.
#   icons/<id>.png       -- per-emote icon override; wins over API/wiki and
#                           survives --refresh-icons (a "hard replace").
OVERRIDES_DIR = os.path.join(HERE, "overrides")
OVERRIDES_PATH = os.path.join(OVERRIDES_DIR, "emote_overrides.json")
ICON_OVERRIDE_DIR = os.path.join(OVERRIDES_DIR, "icons")

LANGS = ["en", "de", "fr", "es"]            # our catalog language order (file order)
# The EN wiki's four command columns are ordered English | French | German |
# Spanish -- NOT our LANGS order. Parse with this, not LANGS.
WIKI_COL_LANGS = ["en", "fr", "de", "es"]
WIKI_HOST = {"en": "wiki", "de": "wiki-de", "fr": "wiki-fr", "es": "wiki-es"}
USER_AGENT = "emot3-catalog-builder/1.0 (https://github.com/isimp/emot3)"
DEFAULT_TTL = 7 * 24 * 3600                  # 7 days
# Hard cap for downloaded icons, matching the addon's icon dimension cap
# (ProbeIconFile / icon_cache.json maxIconDim). We can't resize (stdlib only), so
# an over-cap icon is skipped (the existing/AI icon stands) rather than shipped.
MAX_ICON_DIM = 128

CMD_RE = re.compile(r"'''\s*(/[^'<]+?)\s*'''")
ITEM_NAME_CMD_RE = re.compile(r'"\s*(/[^"]+?)\s*"')        # `"/Rockout" Emote Tome`
ITEM_ICON_RE = re.compile(r'\{\{\s*item icon\s*\|\s*"\s*/?([^"|}]+?)\s*"', re.I)
# The full {{item icon|ARG}} argument == the linked tome page title (`"/X" Emote Tome`).
ITEM_ICON_ARG_RE = re.compile(r'\{\{\s*item icon\s*\|\s*([^}|]+?)\s*(?:\||\}\})', re.I)
# A tome wiki page title like `"/HappyDance" Emote Tome` embeds the command.
TOME_TITLE_CMD_RE = re.compile(r'"\s*(/[^"]+?)\s*"\s*Emote Tome', re.I)
# `| id = 109579` in an {{Item infobox}}.
INFOBOX_ID_RE = re.compile(r'^\s*\|\s*id\s*=\s*(\d+)', re.I | re.M)


# ---------------------------------------------------------------------------
# Cached HTTP
# ---------------------------------------------------------------------------
def fetch(url, binary=False, ttl=DEFAULT_TTL, refresh=False, no_cache=False):
    """GET a URL, backed by an on-disk cache keyed by URL hash. Returns str
    (text) or bytes (binary)."""
    os.makedirs(CACHE_DIR, exist_ok=True)
    key = hashlib.sha1(url.encode("utf-8")).hexdigest()
    path = os.path.join(CACHE_DIR, key + (".bin" if binary else ".txt"))
    if not refresh and not no_cache and os.path.exists(path):
        if (time.time() - os.path.getmtime(path)) < ttl:
            with open(path, "rb") as f:
                data = f.read()
            return data if binary else data.decode("utf-8")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
    time.sleep(0.3)  # be polite to the wiki / API
    if not no_cache:
        with open(path, "wb") as f:
            f.write(data)
    return data if binary else data.decode("utf-8")


def fetch_json(url, **kw):
    return json.loads(fetch(url, **kw))


# ---------------------------------------------------------------------------
# Normalization (mirrors NormalizeEmoteCommand / NormalizeUnlockKey in C++)
# ---------------------------------------------------------------------------
def norm(cmd):
    """Dedupe/match key: lowercase, strip a leading slash and surrounding space."""
    return cmd.strip().lstrip("/").strip().lower()


def normalize_command(cmd):
    """Store form: trimmed, leading slash, lowercase."""
    c = cmd.strip().lower()
    if not c:
        return ""
    return c if c.startswith("/") else "/" + c


def derive_name(command):
    """Best-effort display name from a slash command: strip slash, capitalize the
    first letter (kept ASCII-safe; the JSON stores diacritics fine). Imperfect for
    multi-word concatenated commands -- always surfaced in the report."""
    s = command.strip().lstrip("/").strip()
    return s[:1].upper() + s[1:] if s else ""


def wiki_slug(title):
    """FALLBACK encoder for a wiki URL path slug when MediaWiki's own fullurl isn't
    available (see resolve_canonical_slugs, the authoritative path): spaces -> '_',
    specials percent-encoded the MediaWiki way ('"/Flex" Emote Tome' ->
    '%22/Flex%22_Emote_Tome'). Verified byte-identical to the wiki's URLs."""
    return urllib.parse.quote(title.replace(" ", "_"), safe="/:,;@$!*()")


# ---------------------------------------------------------------------------
# GW2 API
# ---------------------------------------------------------------------------
def fetch_items_by_lang(opts, item_ids):
    """Return {lang: {item_id: item_json}} for the given ids -- one batched
    /v2/items call per language."""
    out = {lang: {} for lang in LANGS}
    ids = sorted(set(item_ids))
    if not ids:
        return out
    ids_param = ",".join(str(i) for i in ids)
    for lang in LANGS:
        arr = fetch_json("https://api.guildwars2.com/v2/items?ids=%s&lang=%s"
                         % (ids_param, lang),
                         refresh=opts.refresh, no_cache=opts.no_cache)
        out[lang] = {it["id"]: it for it in arr}
    return out


def discover_tome_titles(opts):
    """Map norm(command) -> tome wiki page title, for EVERY `"X" Emote Tome` page
    on the wiki (via search). Catches tome emotes the main Emote table omits
    entirely (flex, happydance) and the per-pose tomes. Container tomes (Fashion /
    Cute Poses) have no slash-command in their title, so they're skipped."""
    url = ("https://%s.guildwars2.com/api.php?format=json&action=query&list=search"
           "&srsearch=intitle:%%22Emote%%20Tome%%22&srnamespace=0&srlimit=500"
           % WIKI_HOST["en"])
    data = fetch_json(url, refresh=opts.refresh, no_cache=opts.no_cache)
    out = {}
    for hit in data.get("query", {}).get("search", []):
        m = TOME_TITLE_CMD_RE.search(hit["title"])
        if m:
            out[norm(m.group(1))] = hit["title"]
    return out


def collect_tome_ids(opts, en_rows, catalog_keys):
    """Map norm(command) -> GW2 API item id for every tome-unlockable emote we
    ship. Sources, in order:
      1. /v2/emotes -- 14 ids for free, no page fetch.
      2. the EN Emote table's "Unlock item" column: each `{{item icon|"X" Emote
         Tome}}` cell IS a link to that emote's tome page, whose {{Item infobox}}
         carries `| id =` -- fetched for any catalog emote not already covered.
      3. a wiki search backfills tome emotes the table omits entirely (flex,
         happydance).
    Only tome pages for emotes we actually ship (and don't already have an id for)
    are fetched, so the page-fetch count stays bounded.

    Returns (key_to_id, key_to_slug): the API item id, and the tome wiki page title
    (a stable link target -- e.g. `"/Flex" Emote Tome`) for catalog emotes."""
    key_to_id = {}
    for e in fetch_json("https://api.guildwars2.com/v2/emotes?ids=all",
                        refresh=opts.refresh, no_cache=opts.no_cache):
        unlock = e.get("unlock_items", [])
        if unlock:
            key_to_id[norm(e["id"])] = unlock[0]

    # Prefer the wiki SEARCH titles (canonical page titles, correct casing) for the
    # slug; the unlock-column link only backfills tomes search somehow missed.
    titles = {}
    for k, t in discover_tome_titles(opts).items():
        titles[k] = t                                     # canonical search title
    for row in en_rows:
        tp = row.get("tome_page", "")
        m = TOME_TITLE_CMD_RE.search(tp) if tp else None
        if m:
            titles.setdefault(norm(m.group(1)), tp)       # unlock-column fallback

    for key, title in sorted(titles.items()):
        if key not in catalog_keys or key in key_to_id:
            continue
        page = fetch("https://%s.guildwars2.com/index.php?title=%s&action=raw"
                     % (WIKI_HOST["en"], urllib.parse.quote(title)),
                     refresh=opts.refresh, no_cache=opts.no_cache)
        m = INFOBOX_ID_RE.search(page)
        if m:
            key_to_id[key] = int(m.group(1))
    # slug only for catalog emotes that have a tome page title
    key_to_slug = {k: t for k, t in titles.items() if k in catalog_keys}
    return key_to_id, key_to_slug


def fetch_api(opts, key_to_id, key_to_slug):
    """Authoritative API layer: {key: {lang: {command, icon_url, item_id}}} for
    EVERY tome emote we ship. The localized command is parsed from the item NAME
    (`"/Rockout" Emote Tome`); the icon is the render-service URL. This is the
    PREFERRED source for command + icon in merge() -- it wins over wiki/curated.

    Also BACKFILLS key_to_slug for emotes whose unlock item is a volume/container
    (no individual `"X" Emote Tome` page, e.g. geargrind/shuffle/step -> "How to
    Dance, Volume 1"): the unlock item's own wiki page is the shared-info link."""
    items_by_lang = fetch_items_by_lang(opts, key_to_id.values())
    out = {}
    for key, iid in key_to_id.items():
        per_lang = {}
        for lang in LANGS:
            it = items_by_lang.get(lang, {}).get(iid)
            if not it:
                continue
            m = ITEM_NAME_CMD_RE.search(it.get("name", ""))
            if m:
                per_lang[lang] = {
                    "command": normalize_command(m.group(1)),
                    "icon_url": it.get("icon", ""),
                    "item_id": iid,
                }
        if per_lang:
            out[key] = per_lang
        # slug backfill: no tome-page slug yet -> link the unlock item's wiki page
        if key not in key_to_slug:
            it = items_by_lang.get("en", {}).get(iid)
            page = resolve_wiki_page(it.get("name", ""), opts) if it else ""
            if page:
                key_to_slug[key] = page
    return out


# ---------------------------------------------------------------------------
# Wiki table parsing
# ---------------------------------------------------------------------------
def split_rows(text):
    """Split a wiki page's first table into rows of cell-strings. Cells are
    one-per-line (`| ...`); header lines (`!`) and the `|-` row separators
    delimit. Stops at the table close `|}`."""
    rows, cur, in_table = [], None, False
    for line in text.splitlines():
        s = line.rstrip("\n")
        if s.startswith("{|"):
            in_table = True
            continue
        if not in_table:
            continue
        if s.startswith("|}"):
            break
        if s.startswith("|-"):
            if cur is not None:
                rows.append(cur)
            cur = []
            continue
        if cur is None:
            continue
        if s.startswith("|"):
            cur.append(s[1:].strip())   # drop the leading pipe
    if cur:
        rows.append(cur)
    return rows


def cmds_in(cell):
    """All `'''/command'''` variants in a cell, in order."""
    return [c.strip() for c in CMD_RE.findall(cell) if c.strip()]


def parse_en_table(text):
    """Parse the EN wiki Emote table. Columns:
       0-3 commands en/fr/de/es, 4 Persistent, 5 Sitting, 6 Chat msg,
       7 Targeted msg, 8 Unlock item, 9 Animation.
    Returns list of row dicts keyed for merge by every EN command norm."""
    rows = []
    for cells in split_rows(text):
        if len(cells) < 4 or not cmds_in(cells[0]):
            continue  # not an emote row (header / note / malformed)
        row = {
            "cmds": {WIKI_COL_LANGS[i]: cmds_in(cells[i]) for i in range(4)},
            "persistent": "{{yes}}" in cells[4].lower() if len(cells) > 4 else False,
            "targeted_msg": cells[7].strip() if len(cells) > 7 else "",
            "icon_token": "",
            "tome_page": "",
        }
        unlock = cells[8] if len(cells) > 8 else ""
        m = ITEM_ICON_RE.search(unlock)
        if m:
            row["icon_token"] = m.group(1).strip()   # e.g. Barbecue / BloodstoneBoogie
        m = ITEM_ICON_ARG_RE.search(unlock)
        if m:
            row["tome_page"] = m.group(1).strip()    # e.g. "/Barbecue" Emote Tome
        rows.append(row)
    return rows


def parse_lang_table(text, lang):
    """Parse a localized wiki Emote table. Column 0 = localized command(s),
    column 1 = English command (the join key). Returns {en_norm: [localized cmds]}."""
    out = {}
    for cells in split_rows(text):
        if len(cells) < 2:
            continue
        loc = cmds_in(cells[0])
        eng = cmds_in(cells[1])
        if not loc or not eng:
            continue
        out[norm(eng[0])] = loc
    return out


def build_wiki(opts):
    """Return (en_rows, en_index, lang_extra) where en_index maps every EN command
    norm -> its row, and lang_extra maps lang -> {en_norm: [localized cmds]}."""
    en_text = fetch("https://%s.guildwars2.com/index.php?title=Emote&action=raw"
                    % WIKI_HOST["en"], refresh=opts.refresh, no_cache=opts.no_cache)
    en_rows = parse_en_table(en_text)
    en_index = {}
    for row in en_rows:
        for c in row["cmds"]["en"]:
            en_index[norm(c)] = row
    lang_extra = {}
    for lang in ("de", "fr", "es"):
        text = fetch("https://%s.guildwars2.com/index.php?title=Emote&action=raw"
                     % WIKI_HOST[lang], refresh=opts.refresh, no_cache=opts.no_cache)
        lang_extra[lang] = parse_lang_table(text, lang)
    return en_rows, en_index, lang_extra


# ---------------------------------------------------------------------------
# Icon resolution (provenance-clean: API render URL, else wiki File on the page)
# ---------------------------------------------------------------------------
def build_wiki_icon_index(opts):
    """Map a lowercased icon token -> exact wiki image URL, using the set of
    File:"X" Emote Tome.png files actually used on the EN Emote page."""
    url = ("https://%s.guildwars2.com/api.php?format=json&action=query&prop=images"
           "&imlimit=500&titles=Emote" % WIKI_HOST["en"])
    data = fetch_json(url, refresh=opts.refresh, no_cache=opts.no_cache)
    pages = data.get("query", {}).get("pages", {})
    titles = [im["title"] for p in pages.values() for im in p.get("images", [])]
    tome_titles = [t for t in titles if "emote tome" in t.lower()]
    index = {}
    for title in tome_titles:
        # title like: File:"Barbecue" Emote Tome.png  -> token "barbecue"
        m = re.search(r'"\s*([^"]+?)\s*"\s*emote tome', title, re.I)
        if m:
            index[m.group(1).strip().lower()] = title
    return index


def resolve_image_url(file_title, opts):
    url = ("https://%s.guildwars2.com/api.php?format=json&action=query"
           "&prop=imageinfo&iiprop=url&titles=%s"
           % (WIKI_HOST["en"], urllib.parse.quote(file_title)))
    data = fetch_json(url, refresh=opts.refresh, no_cache=opts.no_cache)
    for p in data.get("query", {}).get("pages", {}).values():
        info = p.get("imageinfo")
        if info:
            return info[0]["url"]
    return ""


def resolve_wiki_page(title, opts):
    """Return the canonical wiki page title if a page (or redirect) exists for
    `title`, else "". Used to link volume/container-unlocked emotes (no individual
    tome page) to their unlock item's wiki page."""
    if not title:
        return ""
    url = ("https://%s.guildwars2.com/api.php?format=json&action=query&redirects=1"
           "&titles=%s" % (WIKI_HOST["en"], urllib.parse.quote(title)))
    data = fetch_json(url, refresh=opts.refresh, no_cache=opts.no_cache)
    for p in data.get("query", {}).get("pages", {}).values():
        if "missing" not in p and int(p.get("pageid", -1)) > 0:
            return p.get("title", "")
    return ""


def resolve_canonical_slugs(opts, titles):
    """Map page title -> the canonical URL path slug taken STRAIGHT from MediaWiki
    (prop=info&inprop=url -> fullurl, the part after '/wiki/'). Authoritative -- no
    self-encoding guesswork. Batched (<=50 titles/query); the `normalized` map
    handles any title MediaWiki rewrites so the input title still resolves."""
    out = {}
    uniq = sorted({t for t in titles if t})
    marker = "/wiki/"
    for i in range(0, len(uniq), 50):
        batch = uniq[i:i + 50]
        url = ("https://%s.guildwars2.com/api.php?format=json&action=query"
               "&prop=info&inprop=url&redirects=1&titles=%s"
               % (WIKI_HOST["en"], urllib.parse.quote("|".join(batch))))
        q = fetch_json(url, refresh=opts.refresh, no_cache=opts.no_cache).get("query", {})
        norm_map = {n["from"]: n["to"] for n in q.get("normalized", [])}
        title_slug = {}
        for p in q.get("pages", {}).values():
            full = p.get("fullurl", "")
            j = full.find(marker)
            if j >= 0:
                title_slug[p.get("title", "")] = full[j + len(marker):]
        for t in batch:
            slug = title_slug.get(norm_map.get(t, t)) or title_slug.get(t)
            if slug:
                out[t] = slug
    return out


def png_dims(data):
    """(w, h) from a PNG's IHDR, or None. Mirrors the C++ ProbeIconFile check."""
    if len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n":
        w, h = struct.unpack(">II", data[16:24])
        return w, h
    return None


# ---------------------------------------------------------------------------
# Merge
# ---------------------------------------------------------------------------
def merge(existing, api, en_rows, en_index, lang_extra, icon_index, shared_urls,
          overrides, key_to_id, key_to_slug, opts):
    """Rebuild every emote's per-language objects as a CLEAN PROJECTION of the
    sources -- no legacy hand-curated value is carried forward. Precedence:

        command : override > API > wiki (first listed variant)
        aliases : every other API/wiki variant, plus any override aliases
        name    : override > derived-from-command (no source carries a name)

    `overrides` (tools/overrides/emote_overrides.json) is the reserved curated layer: it WINS
    over API/wiki and is where best-effort display names live + where future manual
    command/alias pins go. Structural flags (is_core/targetable/mad_king) are NOT
    localization values and are preserved. A language with no source at all is
    dropped (the emote falls back to English at runtime).

    Returns (emotes, report, icon_jobs)."""
    report = {
        "override_applied": [],  # (id, lang, field) curated override took effect
        "name_derived": [],      # (id, lang) name fell back to derive (no override)
        "primary_src": {},       # source -> count, for the primary command
        "lang_dropped": [],      # (id, lang) removed: no API/wiki/override source
        "flag_changed": [],      # (id, flag, old, new) derived/override flag differs
        "icon_new": [],          # (id, kind)
        "icon_refresh": [],      # (id, kind)
        "icon_override": [],     # (id) curated icon override in effect
        "icon_bad_override": [], # (id) override file present but not a valid PNG / over cap
        "icon_missing": [],      # (id) tome emote with no resolvable icon
        "icon_shared": [],       # (id) icon is a shared collection icon, skipped
        "wiki_not_in_catalog": [],
        "added_emote": [],       # (id) brand-new emote added via --add-new
    }
    icon_jobs = []
    by_id = {e["id"]: e for e in existing["emotes"]}

    def resolve_icon(eid):
        """Pick a provenance-clean icon URL for an emote, or None. Prefers the API
        render URL (exact, verified), else the wiki tome File."""
        api_url = (api.get(eid, {}).get("en", {}) or {}).get("icon_url")
        if api_url:
            if api_url in shared_urls:
                report["icon_shared"].append(eid)   # collection icon -> keep existing
                return None                          # don't also try the wiki path
            return ("api", api_url)
        token = en_index.get(eid, {}).get("icon_token", "") if en_index.get(eid) else ""
        if token:
            title = icon_index.get(token.lower())
            if title:
                url = resolve_image_url(title, opts)
                if url:
                    return ("wiki", url)
            report["icon_missing"].append(eid)
        return None

    def fill_emote(e, row):
        eid = e["id"]
        ov = overrides.get(eid, {})
        old_flags = {k: e.get(k, False) for k in ("is_core", "targetable", "mad_king")}
        for lang in LANGS:
            ov_lang = ov.get(lang, {})
            api_cmd = (api.get(eid, {}).get(lang, {}) or {}).get("command", "")
            wiki_cmds = list(row["cmds"].get(lang, [])) if row else []
            extra = lang_extra.get(lang, {}).get(eid, []) if lang != "en" else []
            ov_cmd = normalize_command(ov_lang.get("command", "")) if ov_lang.get("command") else ""

            variants = []
            for c in ([ov_cmd] if ov_cmd else []) + ([api_cmd] if api_cmd else []) + wiki_cmds + extra:
                nc = normalize_command(c)
                if nc and nc not in variants:
                    variants.append(nc)

            # primary: override > api > wiki(first). No legacy/curated retention.
            if ov_cmd:
                primary, src = ov_cmd, "override"
            elif api_cmd:
                primary, src = normalize_command(api_cmd), "api"
            elif variants:
                primary, src = variants[0], "wiki"
            else:
                primary, src = "", ""

            if not primary:
                if lang in e:
                    del e[lang]                       # clean slate: drop unsourced lang
                    report["lang_dropped"].append((eid, lang))
                continue
            if primary not in variants:
                variants.insert(0, primary)
            report["primary_src"][src] = report["primary_src"].get(src, 0) + 1

            # name: override > derived (nothing else carries a localized name)
            if ov_lang.get("name"):
                name = ov_lang["name"]
                report["override_applied"].append((eid, lang, "name"))
            else:
                name = derive_name(primary)
                report["name_derived"].append((eid, lang))
            if ov_cmd:
                report["override_applied"].append((eid, lang, "command"))

            # aliases: all other variants + any override aliases (additive)
            aliases = [v for v in variants if norm(v) != norm(primary)]
            for a in ov_lang.get("aliases", []):
                na = normalize_command(a)
                if na and norm(na) != norm(primary) and na not in aliases:
                    aliases.append(na)
                    report["override_applied"].append((eid, lang, "alias"))

            obj = {"command": primary, "name": name}
            if aliases:
                obj["aliases"] = aliases
            e[lang] = obj                              # REPLACE wholesale (clean slate)

        # ---- structural fields: DERIVED from sources, override wins ----
        unlock_id = key_to_id.get(eid)
        slug = key_to_slug.get(eid)

        # is_core = has no unlock item (override wins)
        is_core = ov.get("is_core", unlock_id is None)
        # targetable = the wiki "Targeted message" is a DISTINCT message (override
        # wins). No wiki row -> unknown -> False unless overridden.
        derived_target = False
        if row:
            tm = row["targeted_msg"]
            derived_target = bool(tm) and "same message" not in tm.lower() and tm not in ("—", "-")
        targetable = ov.get("targetable", derived_target)
        # mad_king has no source -> override-only (the curated set lives in overrides)
        mad_king = bool(ov.get("mad_king", False))

        for fname, newv in (("is_core", is_core), ("targetable", targetable),
                            ("mad_king", mad_king)):
            if bool(old_flags.get(fname)) != bool(newv):
                report["flag_changed"].append((eid, fname, old_flags.get(fname), newv))

        e["is_core"] = bool(is_core)
        e["targetable"] = bool(targetable)
        if mad_king:
            e["mad_king"] = True
        elif "mad_king" in e:
            del e["mad_king"]
        if unlock_id is not None:
            e["unlock_item"] = unlock_id
        elif "unlock_item" in e:
            del e["unlock_item"]
        if slug:
            e["wiki_slug"] = slug              # canonical URL path slug (see main)
        elif "wiki_slug" in e:
            del e["wiki_slug"]

        # icon: a curated override (tools/overrides/icons/<id>.png) is a HARD
        # replace - it wins over API/wiki and survives --refresh-icons. Validated
        # like a download (PNG + <=cap); a bad override falls through to API/wiki.
        ov_icon = os.path.join(ICON_OVERRIDE_DIR, eid + ".png")
        handled = False
        if os.path.isfile(ov_icon):
            with open(ov_icon, "rb") as f:
                d = f.read()
            dims = png_dims(d)
            if dims and dims[0] <= MAX_ICON_DIM and dims[1] <= MAX_ICON_DIM:
                report["icon_override"].append(eid)
                icon_jobs.append((eid, "override", ov_icon))
                handled = True
            else:
                report["icon_bad_override"].append(eid)  # invalid -> fall through
        if not handled:
            icon = resolve_icon(eid)
            if icon:
                kind, url = icon
                dest = os.path.join(ICON_DIR, eid + ".png")
                if not os.path.exists(dest):
                    report["icon_new"].append((eid, kind))
                    icon_jobs.append((eid, kind, url))
                elif opts.refresh_icons:
                    report["icon_refresh"].append((eid, kind))
                    icon_jobs.append((eid, kind, url))

    # 1) update existing emotes
    for e in existing["emotes"]:
        fill_emote(e, en_index.get(e["id"]))

    # 2) report (and optionally add) wiki emotes not in our catalog. Match against
    # EVERY English command/alias we already ship (not just a row's first command)
    # so an emote the wiki lists alias-first (e.g. scissors as "/scis<br>/scissors")
    # isn't mistaken for a new emote.
    known_en = set(by_id.keys())
    for e in existing["emotes"]:
        en = e.get("en", {})
        if en.get("command"):
            known_en.add(norm(en["command"]))
        for a in en.get("aliases", []):
            known_en.add(norm(a))
    for row in en_rows:
        en_cmds = row["cmds"].get("en", [])
        if not en_cmds:
            continue
        norms = [norm(c) for c in en_cmds]
        if any(n in known_en for n in norms):
            continue
        eid = norms[0]
        report["wiki_not_in_catalog"].append(eid)
        if opts.add_new:
            e = {"id": eid}                  # flags are derived in fill_emote
            existing["emotes"].append(e)
            by_id[eid] = e
            known_en.update(norms)
            fill_emote(e, row)
            report["added_emote"].append(eid)

    # de-dupe report lists that may have repeated via the row-values loop
    report["wiki_not_in_catalog"] = sorted(set(report["wiki_not_in_catalog"]))
    return existing["emotes"], report, icon_jobs


# ---------------------------------------------------------------------------
# Serialization (hand-formatted, matches the existing file style)
# ---------------------------------------------------------------------------
NOTE = (
    "Bundled emote-localization table (the 'separate mechanism'). Per emote: "
    "flags (is_core, targetable, mad_king = 'Your Mad King Says...' event set) "
    "+ a per-language { command, name, aliases } object. Seeding for language L "
    "uses data[L]; when L is absent the emote falls back to English entirely. "
    "'command' is the primary slash command (ASCII alias preferred when available). "
    "'aliases' are the OTHER slash commands for that emote in that language; they "
    "identify the emote for unlock sync + search but are never sent. 'name' is a "
    "display label. Commands work regardless of client language, so the unlock "
    "index pools every language's command + aliases. No Portuguese GW2 client "
    "exists, so PT is absent. Generated by tools/gen_emote_catalog.py as a clean "
    "projection of the sources: command = curated override > GW2 API (/v2/items, "
    "id via the wiki Unlock-item link or /v2/emotes) > wiki; aliases = the other "
    "API/wiki variants; name = override > best-effort derive (no source carries a "
    "localized display name). Curated overrides live in tools/overrides/. "
    "Names are best-effort; verify in-game."
)


def jstr(s):
    return json.dumps(s, ensure_ascii=False)


def dump_json(data):
    out = ["{"]
    out.append('  "version": %s,' % json.dumps(data["version"]))
    out.append('  "languages": [%s],' % ", ".join(jstr(x) for x in data["languages"]))
    out.append('  "_note": %s,' % jstr(data["_note"]))
    out.append('  "emotes": [')
    emotes = data["emotes"]
    flag_keys = ("id", "is_core", "targetable", "mad_king", "unlock_item", "wiki_slug")
    for ei, e in enumerate(emotes):
        head_parts = []
        for k in flag_keys:
            if k in e:
                head_parts.append('%s: %s' % (jstr(k), json.dumps(e[k], ensure_ascii=False)))
        out.append("    { " + ", ".join(head_parts) + ",")
        langs = [k for k in e.keys() if k not in flag_keys]
        for li, lk in enumerate(langs):
            o = e[lk]
            inner = '"command": %s, "name": %s' % (jstr(o["command"]), jstr(o.get("name", "")))
            if o.get("aliases"):
                inner += ', "aliases": [%s]' % ", ".join(jstr(a) for a in o["aliases"])
            close = " } }" if li == len(langs) - 1 else " },"
            out.append('      %s: { %s%s' % (jstr(lk), inner, close))
        if ei != len(emotes) - 1:
            out[-1] += ","
    out.append("  ]")
    out.append("}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Diff + report printing
# ---------------------------------------------------------------------------
def emote_repr(e):
    """A stable, comparable per-emote dict (order-independent)."""
    HEAD = ("id", "is_core", "targetable", "mad_king", "unlock_item", "wiki_slug")
    r = {k: e[k] for k in HEAD if k in e}
    for lk in [k for k in e if k not in HEAD]:
        o = e[lk]
        r[lk] = {"command": o.get("command", ""), "name": o.get("name", ""),
                 "aliases": list(o.get("aliases", []))}
    return r


def print_diff(old_emotes, new_emotes):
    old = {e["id"]: emote_repr(e) for e in old_emotes}
    new = {e["id"]: emote_repr(e) for e in new_emotes}
    changed = 0
    for eid in new:
        o, n = old.get(eid), new[eid]
        if o == n:
            continue
        changed += 1
        if o is None:
            print("  + NEW emote %s" % eid)
            continue
        for k in n:
            if o.get(k) != n.get(k):
                print("  ~ %-18s %s: %s -> %s" % (eid, k, json.dumps(o.get(k), ensure_ascii=False),
                                                  json.dumps(n.get(k), ensure_ascii=False)))
    print("\n%d emote(s) changed." % changed)
    return changed


def print_report(report):
    def section(title, items, fmt):
        if not items:
            return
        print("\n[%s] (%d)" % (title, len(items)))
        for it in items:
            print("   " + fmt(it))
    if report["primary_src"]:
        print("\n[primary command source] " +
              ", ".join("%s=%d" % (k, v) for k, v in sorted(report["primary_src"].items())))
    ov = report["override_applied"]
    ov_cmds = [t for t in ov if t[2] == "command"]
    print("[curated overrides] name=%d command=%d alias=%d"
          % (sum(1 for t in ov if t[2] == "name"), len(ov_cmds),
             sum(1 for t in ov if t[2] == "alias")))
    section("  override commands (manual pins)", ov_cmds,
            lambda t: "%s [%s]" % (t[0], t[1]))
    section("languages dropped (no API/wiki/override source)", report["lang_dropped"],
            lambda t: "%s: %s" % (t[0], t[1]))
    print("[names: derived from command] %d (no override name yet)"
          % len(report["name_derived"]))
    section("flags changed (derived/override vs previous)", report["flag_changed"],
            lambda t: "%s.%s: %s -> %s" % (t[0], t[1], t[2], t[3]))
    section("icon overrides in effect (tools/overrides/icons/)",
            [(x,) for x in report["icon_override"]], lambda t: t[0])
    section("BAD icon overrides (not a PNG / over cap; ignored)",
            [(x,) for x in report["icon_bad_override"]], lambda t: t[0])
    section("icons to add", report["icon_new"],
            lambda t: "%s (%s)" % t)
    section("icons to refresh (--refresh-icons)", report["icon_refresh"],
            lambda t: "%s (%s)" % t)
    section("tome emotes with no resolvable icon", [(x,) for x in report["icon_missing"]],
            lambda t: t[0])
    section("shared/collection icon, not downloaded (kept existing)",
            [(x,) for x in report["icon_shared"]],
            lambda t: t[0])
    section("added new emotes (--add-new)", [(x,) for x in report["added_emote"]],
            lambda t: t[0])
    section("wiki emotes NOT in our catalog (use --add-new to include)",
            [(x,) for x in report["wiki_not_in_catalog"]],
            lambda t: t[0])


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true",
                    help="apply changes to the JSON and download icons")
    ap.add_argument("--refresh", action="store_true",
                    help="ignore the HTTP cache and re-fetch all sources")
    ap.add_argument("--no-cache", action="store_true",
                    help="neither read nor write the HTTP cache")
    ap.add_argument("--refresh-icons", action="store_true",
                    help="re-download icons that already exist on disk (normalize to API)")
    ap.add_argument("--add-new", action="store_true",
                    help="add wiki emotes not present in our catalog (review flags!)")
    opts = ap.parse_args()

    with open(JSON_PATH, encoding="utf-8") as f:
        existing = json.load(f)
    old_emotes = json.loads(json.dumps(existing["emotes"]))  # deep copy for diff

    print("Fetching GW2 wikis (en/de/fr/es) ...")
    en_rows, en_index, lang_extra = build_wiki(opts)
    print("  EN wiki emote rows: %d" % len(en_rows))

    # Authoritative API layer for EVERY tome emote: the EN table's unlock-item
    # column links to each emote's tome page -> item id -> /v2/items (command +
    # icon). PREFERRED over wiki/curated in merge().
    print("Fetching GW2 API (tome item ids via unlock-item links) ...")
    catalog_keys = {e["id"] for e in existing["emotes"]}
    key_to_id, key_to_slug = collect_tome_ids(opts, en_rows, catalog_keys)
    api = fetch_api(opts, key_to_id, key_to_slug)
    # key_to_slug currently holds page TITLES; replace each with the canonical URL
    # path slug taken straight from MediaWiki (fallback: our own encoder).
    title_to_slug = resolve_canonical_slugs(opts, key_to_slug.values())
    for k, title in list(key_to_slug.items()):
        key_to_slug[k] = title_to_slug.get(title) or wiki_slug(title)
    print("  API tome emotes: %d  (unlock ids: %d, wiki slugs: %d)"
          % (len(api), len(key_to_id), len(key_to_slug)))

    # An icon URL shared by more than one emote is a collection/container icon
    # (e.g. the Cute Poses tome art is identical for all four poses), NOT
    # emote-specific -- never download it as a per-emote icon.
    url_counts = {}
    for langs in api.values():
        u = (langs.get("en", {}) or {}).get("icon_url")
        if u:
            url_counts[u] = url_counts.get(u, 0) + 1
    shared_urls = {u for u, c in url_counts.items() if c > 1}

    print("Fetching wiki icon index ...")
    icon_index = build_wiki_icon_index(opts)
    print("  wiki tome icons: %d" % len(icon_index))

    overrides = {}
    if os.path.exists(OVERRIDES_PATH):
        with open(OVERRIDES_PATH, encoding="utf-8") as f:
            overrides = json.load(f).get("emotes", {})
    print("  curated overrides: %d emote(s)" % len(overrides))

    existing["version"] = existing.get("version", 1)
    existing["languages"] = LANGS
    existing["_note"] = NOTE
    emotes, report, icon_jobs = merge(existing, api, en_rows, en_index, lang_extra,
                                      icon_index, shared_urls, overrides,
                                      key_to_id, key_to_slug, opts)

    print("\n===== DIFF =====")
    changed = print_diff(old_emotes, emotes)
    print("\n===== REPORT =====")
    print_report(report)

    if not opts.write:
        print("\n(dry-run) nothing written. Re-run with --write to apply.")
        return

    # write JSON
    with open(JSON_PATH, "w", encoding="utf-8") as f:
        f.write(dump_json(existing))
    print("\nWrote %s" % os.path.relpath(JSON_PATH, REPO))

    # download / copy icons (override jobs read a local file, the rest fetch)
    if icon_jobs:
        os.makedirs(ICON_DIR, exist_ok=True)
        for eid, kind, src in icon_jobs:
            if kind == "override":
                with open(src, "rb") as f:
                    data = f.read()
            else:
                data = fetch(src, binary=True, refresh=opts.refresh, no_cache=opts.no_cache)
            dims = png_dims(data)
            if not dims:
                print("  ! %s: not a PNG, skipped (%s)" % (eid, src))
                continue
            if dims[0] > MAX_ICON_DIM or dims[1] > MAX_ICON_DIM:
                print("  ! %s: %dx%d exceeds %dpx cap, skipped (kept existing)"
                      % (eid, dims[0], dims[1], MAX_ICON_DIM))
                continue
            dest = os.path.join(ICON_DIR, eid + ".png")
            if os.path.exists(dest):                      # idempotent: skip identical bytes
                with open(dest, "rb") as f:
                    if f.read() == data:
                        continue
            with open(dest, "wb") as f:
                f.write(data)
            print("  icon %s <- %s (%dx%d)" % (eid, kind, dims[0], dims[1]))
    print("\nDone. Review with: git diff resources/")


if __name__ == "__main__":
    main()
