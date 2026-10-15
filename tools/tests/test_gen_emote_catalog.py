"""Requirement tests for tools/gen_emote_catalog.py.

Each test runs the builder's real entry point (main() with command-line flags)
against a temporary catalog and an offline stand-in for the GW2 API and wiki, so
no network is touched. Run with:

    py -3 -m unittest discover -s tools/tests
"""
import contextlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import urllib.parse
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gen_emote_catalog as g  # noqa: E402


def fake_png(w, h, tag):
    """Bytes the builder accepts as a PNG of w x h (it only reads the IHDR)."""
    return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR"
            + struct.pack(">II", w, h) + bytes([8, 6, 0, 0, 0]) + b"\0\0\0\0"
            + tag.encode("ascii"))


def icon_url(item_id):
    return "https://render.guildwars2.com/file/HASH/%d.png" % item_id


# GW2 API items: per language, the tome NAME carries the localized command.
ITEMS = {
    100: {"en": '"/Readbook" Emote Tome', "de": '"/Lesen"-Emote-Foliant',
          "fr": 'Grimoire d\'emote "/lire"', "es": 'Tomo de animación de "/leer"'},
    110169: {"en": '"/Clap" Emote Tome', "de": '"/Klatschen"-Emote-Foliant',
             "fr": 'Grimoire d\'emote "/applaudir"', "es": 'Tomo de animación de "/aplaudir"'},
    109895: {"en": '"/Popcorn" Emote Tome', "de": '"/Popcorn"-Emote-Foliant',
             "fr": 'Grimoire d\'emote "/popcorn"', "es": 'Tomo de animación de "/palomitas"'},
}
ICONS = {icon_url(i): fake_png(64, 64, "icon%d" % i) for i in ITEMS}

# The EN wiki Emote table: a core emote, a tome emote we ship, and a new tome
# emote (/clap) that so far has only its English column filled in.
EN_TABLE = """{| class="table"
! English !! French !! German !! Spanish
|-
| '''/wave'''
| '''/salut'''
| '''/winken'''
| '''/saludar'''
| {{no}}
| {{no}}
| [Character] waves.
| [Character] waves at [Target].
|
|
|-
| '''/readbook'''<br>'''/read'''
| '''/lire'''
| '''/lesen'''
| '''/leer'''
| {{yes}}
| {{yes}}
| [Character] reads a book.
| {{gray|''Same message as without a target.''}}
| {{item icon|"/Readbook" Emote Tome}}
|
|-
| '''/clap'''
|
|
|
| {{no}}
| {{no}}
|
| {{gray|''Same message as without a target.''}}
| {{item icon|"/Clap" Emote Tome}}
| —
|}
"""

LANG_TABLES = {
    "de": "{|\n|-\n| '''/winken'''\n| '''/wave'''\n|-\n| '''/lesen'''\n| '''/readbook'''\n|}\n",
    "fr": "{|\n|-\n| '''/salut'''\n| '''/wave'''\n|-\n| '''/lire'''\n| '''/readbook'''\n|}\n",
    "es": "{|\n|-\n| '''/saludar'''\n| '''/wave'''\n|-\n| '''/leer'''\n| '''/readbook'''\n|}\n",
}

# Every "X" Emote Tome page the wiki search returns. /Read is a page named by
# one of readbook's other commands; /Popcorn has no row in the Emote table (a
# gem-store tome); /Broken has a page but no item id on it.
TOME_PAGES = {
    '"/Readbook" Emote Tome': "{{Item infobox\n| id = 100\n}}",
    '"/Read" Emote Tome': "{{Item infobox\n| id = 100\n}}",
    '"/Clap" Emote Tome': "{{Item infobox\n| id = 110169\n}}",
    '"/Popcorn" Emote Tome': "{{Item infobox\n| id = 109895\n}}",
    '"/Broken" Emote Tome': "{{Item infobox\n| type = Consumable\n}}",
}


def wiki_slug_of(title):
    return urllib.parse.quote(title.replace(" ", "_"), safe="/:,()!*'")


def fake_fetch(url, binary=False, **_kw):
    """Offline GW2 API + wiki. An unexpected URL fails the test loudly."""
    if url in ICONS:
        return ICONS[url]
    u = urllib.parse.urlparse(url)
    q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
    host = u.netloc.split(".")[0]
    if u.netloc == "api.guildwars2.com" and u.path == "/v2/emotes":
        return json.dumps([{"id": "Readbook", "commands": ["/readbook"], "unlock_items": [100]}])
    if u.netloc == "api.guildwars2.com" and u.path == "/v2/items":
        ids = [int(x) for x in q["ids"].split(",")]
        return json.dumps([{"id": i, "name": ITEMS[i][q["lang"]], "icon": icon_url(i)}
                           for i in ids if i in ITEMS])
    if u.path == "/index.php" and q.get("action") == "raw":
        if q["title"] == "Emote":
            return EN_TABLE if host == "wiki" else LANG_TABLES[host.split("-")[1]]
        if host == "wiki" and q["title"] in TOME_PAGES:
            return TOME_PAGES[q["title"]]
    if host == "wiki" and u.path == "/api.php":
        if q.get("list") == "search":
            return json.dumps({"query": {"search": [{"title": t} for t in TOME_PAGES]}})
        if q.get("prop") == "images":
            return json.dumps({"query": {"pages": {"1": {"title": "Emote", "images": []}}}})
        if q.get("prop") == "imageinfo":
            return json.dumps({"query": {"pages": {}}})
        titles = q.get("titles", "").split("|")
        pages = {}
        for n, t in enumerate(titles, 1):
            if t in TOME_PAGES:
                pages[str(n)] = {"pageid": n, "title": t,
                                 "fullurl": "https://wiki.guildwars2.com/wiki/" + wiki_slug_of(t)}
        return json.dumps({"query": {"pages": pages}})
    raise AssertionError("unexpected fetch: " + url)


# The catalog as shipped before the run: exactly what the builder makes of the
# two emotes it already knows, so any later difference comes from new emotes.
SHIPPED = [
    {"id": "wave", "is_core": True, "targetable": True,
     "en": {"command": "/wave", "name": "Wave"},
     "de": {"command": "/winken", "name": "Winken"},
     "fr": {"command": "/salut", "name": "Salut"},
     "es": {"command": "/saludar", "name": "Saludar"}},
    {"id": "readbook", "is_core": False, "targetable": False, "unlock_item": 100,
     "wiki_slug": "%22/Readbook%22_Emote_Tome",
     "en": {"command": "/readbook", "name": "Readbook", "aliases": ["/read"]},
     "de": {"command": "/lesen", "name": "Lesen"},
     "fr": {"command": "/lire", "name": "Lire"},
     "es": {"command": "/leer", "name": "Leer"}},
]


class CatalogBuilderTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="emot3_catalog_test_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        data_dir = os.path.join(self.root, "resources", "emote_data")
        self.icon_dir = os.path.join(self.root, "resources", "emotes_official")
        os.makedirs(data_dir)
        os.makedirs(self.icon_dir)
        self.catalog_path = os.path.join(data_dir, "emotes_i18n.json")
        self.v1_path = os.path.join(data_dir, "emotes_i18n_v1.json")
        catalog = {"version": 1, "languages": list(g.LANGS), "_note": g.NOTE,
                   "emotes": json.loads(json.dumps(SHIPPED))}
        with open(self.catalog_path, "w", encoding="utf-8") as f:
            f.write(g.dump_json(catalog))
        with open(self.v1_path, "w", encoding="utf-8") as f:
            f.write('{ "version": 1, "emotes": [ { "id": "wave" } ] }\n')
        with open(os.path.join(self.icon_dir, "readbook.png"), "wb") as f:
            f.write(ICONS[icon_url(100)])
        for name, value in (
                ("fetch", fake_fetch),
                ("REPO", self.root),
                ("JSON_PATH", self.catalog_path),
                ("ICON_DIR", self.icon_dir),
                ("CACHE_DIR", os.path.join(self.root, "cache")),
                ("OVERRIDES_PATH", os.path.join(self.root, "overrides", "emote_overrides.json")),
                ("ICON_OVERRIDE_DIR", os.path.join(self.root, "overrides", "icons"))):
            patcher = mock.patch.object(g, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_builder(self, *flags):
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["gen_emote_catalog.py"] + list(flags)), \
                contextlib.redirect_stdout(out):
            g.main()
        return out.getvalue()

    def catalog(self):
        with open(self.catalog_path, encoding="utf-8") as f:
            return {e["id"]: e for e in json.load(f)["emotes"]}

    def read_bytes(self, path):
        with open(path, "rb") as f:
            return f.read()

    def assert_tome_emote(self, entry, item_id, slug, commands):
        self.assertFalse(entry["is_core"], "a tome emote is not a core emote")
        self.assertEqual(entry["unlock_item"], item_id)
        self.assertEqual(entry["wiki_slug"], slug)
        self.assertEqual({lang: entry[lang]["command"] for lang in g.LANGS}, commands)
        icon = os.path.join(self.icon_dir, entry["id"] + ".png")
        self.assertEqual(self.read_bytes(icon), ICONS[icon_url(item_id)],
                         "the official icon is the API render of the tome")

    def test_a_new_tome_emote_in_the_emote_table_arrives_with_its_unlock_and_all_four_languages(self):
        self.run_builder("--add-new", "--write")
        self.assert_tome_emote(
            self.catalog()["clap"], 110169, "%22/Clap%22_Emote_Tome",
            {"en": "/clap", "de": "/klatschen", "fr": "/applaudir", "es": "/aplaudir"})
        self.assertFalse(self.catalog()["clap"]["targetable"],
                         "the wiki gives /clap no separate targeted message")

    def test_a_new_tome_emote_that_only_has_a_tome_page_is_found_and_added_the_same_way(self):
        self.run_builder("--add-new", "--write")
        entry = self.catalog().get("popcorn")
        self.assertIsNotNone(entry, "a tome page alone is enough to find a new emote")
        self.assert_tome_emote(
            entry, 109895, "%22/Popcorn%22_Emote_Tome",
            {"en": "/popcorn", "de": "/popcorn", "fr": "/popcorn", "es": "/palomitas"})
        self.assertFalse(entry["targetable"], "no table row means no targeted message")

    def test_an_emote_we_ship_is_not_added_again_under_another_of_its_commands(self):
        self.run_builder("--add-new", "--write")
        with open(self.catalog_path, encoding="utf-8") as f:
            ids = [e["id"] for e in json.load(f)["emotes"]]
        self.assertEqual(sorted(ids), ["clap", "popcorn", "readbook", "wave"],
                         "each emote appears once, however many sources list it")

    def test_a_tome_page_without_a_usable_item_is_reported_instead_of_added_empty(self):
        out = self.run_builder("--add-new", "--write")
        catalog = self.catalog()
        self.assertNotIn("broken", catalog)
        for eid, entry in catalog.items():
            self.assertIn("en", entry, "%s has no English command to send" % eid)
        self.assertIn("broken", out, "the unusable tome page is named in the report")

    def test_adding_new_emotes_leaves_the_shipped_ones_as_they_were(self):
        self.run_builder("--add-new", "--write")
        catalog = self.catalog()
        for shipped in SHIPPED:
            self.assertEqual(catalog[shipped["id"]], shipped)

    def test_a_dry_run_names_every_new_emote_and_writes_nothing(self):
        before = self.read_bytes(self.catalog_path)
        out = self.run_builder()
        listed = out.split("NOT in our catalog", 1)[1] if "NOT in our catalog" in out else ""
        for eid in ("clap", "popcorn"):
            self.assertIn(eid, listed, "%s is listed as new" % eid)
        self.assertEqual(self.read_bytes(self.catalog_path), before)
        self.assertEqual(sorted(os.listdir(self.icon_dir)), ["readbook.png"])

    def test_running_again_after_adding_changes_nothing(self):
        self.run_builder("--add-new", "--write")
        first = self.read_bytes(self.catalog_path)
        icons = {n: self.read_bytes(os.path.join(self.icon_dir, n)) for n in os.listdir(self.icon_dir)}
        out = self.run_builder("--add-new", "--write")
        self.assertIn("0 emote(s) changed", out)
        self.assertEqual(self.read_bytes(self.catalog_path), first)
        self.assertEqual({n: self.read_bytes(os.path.join(self.icon_dir, n))
                          for n in os.listdir(self.icon_dir)}, icons)

    def test_the_frozen_v1_snapshot_is_never_written(self):
        before = self.read_bytes(self.v1_path)
        self.run_builder("--add-new", "--write")
        self.assertEqual(self.read_bytes(self.v1_path), before)


if __name__ == "__main__":
    unittest.main()
