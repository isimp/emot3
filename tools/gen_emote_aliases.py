#!/usr/bin/env python3
"""Populate per-language command aliases in resources/emote_data/emotes_i18n.json
from the GW2 wiki's raw emote table.

The wiki page https://wiki.guildwars2.com/wiki/Emote stores every emote's chat
commands in one sortable table whose first four cells per row are the
English | French | German | Spanish command columns, each holding one or more
'''/command''' entries separated by <br>. This script extracts those variants
and, for each emote we already ship, adds the *extra* variants (everything but
the primary command we already store) as that language's "aliases".

It only ADDS aliases: existing "command"/"name" values are never changed, and no
new language objects are created. Re-run after refreshing the wiki dump.

Usage:
    # one-time: fetch the raw wikitext next to this script
    curl -sSL "https://wiki.guildwars2.com/index.php?title=Emote&action=raw" -o emote_wiki.txt
    py -3 tools/gen_emote_aliases.py [path/to/emote_wiki.txt]
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
JSON_PATH = os.path.join(REPO, "resources", "emote_data", "emotes_i18n.json")

# Wiki command columns are in this order; map to our language keys.
COL_LANGS = ["en", "fr", "de", "es"]

CMD_RE = re.compile(r"'''\s*(/[^'<]+?)\s*'''")


def norm(cmd):
    """Match/dedupe key: lowercase, strip a leading slash and surrounding space."""
    return cmd.strip().lstrip("/").strip().lower()


def normalize_command(cmd):
    """Store form: trimmed, leading slash, lowercase (mirrors NormalizeEmoteCommand)."""
    c = cmd.strip().lower()
    if not c:
        return ""
    if not c.startswith("/"):
        c = "/" + c
    return c


def parse_wiki(text):
    """Return list of rows; each row is a list of 4 command-lists (en, fr, de, es)."""
    rows = []
    cur_cells = None  # list of cell strings for the current row
    for line in text.splitlines():
        s = line.rstrip("\n")
        if s.startswith("|-"):
            if cur_cells is not None:
                rows.append(cur_cells)
            cur_cells = []
            continue
        if cur_cells is None:
            continue
        if s.startswith("|"):
            cur_cells.append(s)
    if cur_cells is not None:
        rows.append(cur_cells)

    parsed = []
    for cells in rows:
        if len(cells) < 4:
            continue
        cmd_cols = []
        ok = True
        for i in range(4):
            cmds = CMD_RE.findall(cells[i])
            cmds = [c.strip() for c in cmds if c.strip()]
            cmd_cols.append(cmds)
        # A real emote row has at least an English command in the first cell.
        if not cmd_cols[0]:
            continue
        parsed.append(cmd_cols)
    return parsed


def build_index(parsed):
    """Map every English command stem -> {lang: [commands]}."""
    idx = {}
    for cmd_cols in parsed:
        langcmds = {COL_LANGS[i]: cmd_cols[i] for i in range(4)}
        for en in cmd_cols[0]:
            idx[norm(en)] = langcmds
    return idx


def dump_json(data):
    """Serialize matching the existing hand-formatted block style (alignment dropped)."""
    def jstr(s):
        return json.dumps(s, ensure_ascii=False)

    out = ["{"]
    out.append('  "version": %s,' % json.dumps(data["version"]))
    out.append('  "languages": [%s],' % ", ".join(jstr(x) for x in data["languages"]))
    out.append('  "_note": %s,' % jstr(data["_note"]))
    out.append('  "emotes": [')

    emotes = data["emotes"]
    flag_keys = ("id", "is_core", "targetable", "mad_king")
    for ei, e in enumerate(emotes):
        head_parts = []
        for k in flag_keys:
            if k in e:
                v = e[k]
                head_parts.append('%s: %s' % (jstr(k), json.dumps(v)))
        head = "    { " + ", ".join(head_parts) + ","
        out.append(head)
        langs = [k for k in e.keys() if k not in flag_keys]
        for li, lk in enumerate(langs):
            o = e[lk]
            inner = '"command": %s, "name": %s' % (jstr(o["command"]), jstr(o["name"]))
            if o.get("aliases"):
                inner += ', "aliases": [%s]' % ", ".join(jstr(a) for a in o["aliases"])
            last_lang = (li == len(langs) - 1)
            close = " } }" if last_lang else " },"
            line = '      %s: { %s%s' % (jstr(lk), inner, close)
            out.append(line)
        if ei != len(emotes) - 1:
            out[-1] += ","
    out.append("  ]")
    out.append("}")
    return "\n".join(out) + "\n"


def main():
    wiki_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "emote_wiki.txt")
    with open(wiki_path, encoding="utf-8") as f:
        parsed = parse_wiki(f.read())
    idx = build_index(parsed)
    print("Parsed %d wiki emote rows (%d English stems)." % (len(parsed), len(idx)))

    with open(JSON_PATH, encoding="utf-8") as f:
        data = json.load(f)

    updated = 0
    per_lang = {l: 0 for l in COL_LANGS}
    unmatched = []
    for e in data["emotes"]:
        eid = e["id"]
        en_cmd = e.get("en", {}).get("command", "")
        row = idx.get(norm(eid)) or (idx.get(norm(en_cmd)) if en_cmd else None)
        if not row:
            unmatched.append(eid)
            continue
        touched = False
        for lk in COL_LANGS:
            if lk not in e:
                continue  # only existing language objects
            primary = norm(e[lk].get("command", ""))
            aliases = []
            seen = set()
            for c in row.get(lk, []):
                n = norm(c)
                if not n or n == primary or n in seen:
                    continue
                seen.add(n)
                aliases.append(normalize_command(c))
            if aliases:
                e[lk]["aliases"] = aliases
                per_lang[lk] += len(aliases)
                touched = True
            elif "aliases" in e[lk]:
                del e[lk]["aliases"]  # supersede any stale hand-added entry
        if touched:
            updated += 1

    # Refresh the provenance note.
    data["_note"] = (
        "Bundled emote-localization table (the 'separate mechanism'). Per emote: "
        "flags (is_core, targetable, mad_king = 'Your Mad King Says...' event set) "
        "+ a per-language { command, name, aliases } object. Seeding for language L "
        "uses data[L]; when L is absent the emote falls back to English entirely. "
        "'command' is the primary slash command (ASCII alias preferred when the wiki "
        "lists one). 'aliases' are the OTHER slash commands the GW2 wiki lists for "
        "that emote in that language (extra variants beyond 'command'); they identify "
        "the emote for unlock sync + search but are never sent. 'name' is a natural, "
        "properly spaced/capitalized display label (best-effort, not a command split). "
        "Commands work regardless of client language, so the unlock index pools every "
        "language's command + aliases. No Portuguese GW2 client exists, so PT is absent. "
        "Aliases generated by tools/gen_emote_aliases.py from the EN/FR/DE/ES columns of "
        "the GW2 wiki emote table; commands/names are hand-curated. Verify in-game."
    )

    with open(JSON_PATH, "w", encoding="utf-8") as f:
        f.write(dump_json(data))

    print("Updated %d emote(s). Aliases added per language: %s" % (updated, per_lang))
    if unmatched:
        print("No wiki row matched (%d): %s" % (len(unmatched), ", ".join(unmatched)))
    # Wiki rows whose English stem matched none of our emotes (informational).
    our_stems = set()
    for e in data["emotes"]:
        our_stems.add(norm(e["id"]))
        if "en" in e:
            our_stems.add(norm(e["en"]["command"]))
    extra = sorted({s for s in idx if s not in our_stems})
    if extra:
        print("Wiki stems not in our catalog (%d): %s" % (len(extra), ", ".join(extra)))


if __name__ == "__main__":
    main()
