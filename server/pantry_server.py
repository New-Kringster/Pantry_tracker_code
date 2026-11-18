#!/usr/bin/env python3
"""Pantry tracker server: pantries, items with expiry dates, product lookup, thumbnails, MQTT.

Standard library plus Pillow (thumbnails) and, optionally, paho-mqtt (Home
Assistant). SQLite in --data. Run:

    python3 pantry_server.py --port 8080 --bind 0.0.0.0 --data ./data

API (JSON unless noted):
    GET    /api/health                          {ok, items, mqtt, mqtt_connected}
    GET    /api/pantries                        {pantries:[{id,name,items,expired,expiring_soon}]}
    POST   /api/pantries                        {name?} -> pantry (name defaults to "Pantry N")
    PATCH  /api/pantries/<id>                   {name}
    DELETE /api/pantries/<id>                   items move to the default pantry (id 1 cannot go)
    GET    /api/items[?pantry=<id>]             {today, items:[...]} sorted by expiry
    POST   /api/items                           {barcode, expires:"YYYY-MM-DD", name?, pantry_id?} -> item
    PATCH  /api/items/<id>                      {name?, expires?, pantry_id?}; a name also names the product
    DELETE /api/items/<id>
    GET    /api/products/<barcode>              product info (lookup chain, cached)
    PATCH  /api/products/<barcode>              {name} your own name for a product the databases miss
    GET    /api/products/<barcode>/thumb.jpg?s=64   baseline JPEG, s x s box, 404 if no image
    GET    /api/gs1, GET/POST /api/gs1/login, POST /api/gs1/sms, /api/gs1/sms-verify   GS1 China sign-in
    POST   /api/transcribe[?lang=zh]            audio body (wav / pcm16 16 kHz mono) -> {text, language}
    POST   /api/device                          CoreS3 telemetry {battery, charging, rssi, ip, firmware, uptime_s, event?, barcode?}
    GET/POST /api/settings                      web-portal settings (secrets reported as set/unset)
    GET    /                                    browser UI

Product lookup chain, first hit wins and is recorded as `source`:
    1. Open Food Facts (free; Chinese name preferred when it has one)
    2. GS1 China, gds.org.cn (free account: PANTRY_GDS_USER / PANTRY_GDS_PASS; see gs1_china.py)
    3. A keyed barcode API (PANTRY_BARCODE_URL with {code}, PANTRY_BARCODE_APPCODE header)
    4. Your own names (PATCH) always win over all of them.

Home Assistant: set PANTRY_MQTT_HOST (and optionally PANTRY_MQTT_PORT, PANTRY_MQTT_USER,
PANTRY_MQTT_PASS). The server publishes MQTT discovery for four sensors and a
retained state message on every change.
"""
import argparse
import datetime as dt
import io
import json
import os
import re
import sqlite3
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs1_china  # noqa: E402

VERSION = "0.2.0"
USER_AGENT = f"PantryTracker/{VERSION} (https://www.chiambucket.com)"
OFF_URL = ("https://world.openfoodfacts.org/api/v2/product/{code}.json"
           "?fields=product_name,product_name_zh,product_name_en,brands,quantity,"
           "image_front_small_url,image_front_url")
SOON_DAYS_DEFAULT = 3  # "expiring soon" threshold; overridable in settings


def soon_days():
    try:
        return int(CONFIG.get("soon_days", SOON_DAYS_DEFAULT)) if CONFIG else SOON_DAYS_DEFAULT
    except ValueError:
        return SOON_DAYS_DEFAULT
DEFAULT_PANTRY = "Default Pantry"

HERE = os.path.dirname(os.path.abspath(__file__))


def utcnow():
    return dt.datetime.utcnow().isoformat(timespec="seconds")


# --- Settings (web portal) -------------------------------------------------------
# Key -> (environment fallback, secret?, description). Values saved from the web
# page live in the `settings` table and override the environment.
SETTING_KEYS = {
    "gds_user":        ("PANTRY_GDS_USER", False, "GS1 China (gds.org.cn) username"),
    "gds_pass":        ("PANTRY_GDS_PASS", True,  "GS1 China password"),
    "mqtt_host":       ("PANTRY_MQTT_HOST", False, "MQTT broker host (Home Assistant); empty disables"),
    "mqtt_port":       ("PANTRY_MQTT_PORT", False, "MQTT port"),
    "mqtt_user":       ("PANTRY_MQTT_USER", False, "MQTT username"),
    "mqtt_pass":       ("PANTRY_MQTT_PASS", True,  "MQTT password"),
    "barcode_url":     ("PANTRY_BARCODE_URL", False, "Keyed barcode API URL with {code} (optional, e.g. Aliyun Market)"),
    "barcode_appcode": ("PANTRY_BARCODE_APPCODE", True, "Keyed barcode API AppCode / key"),
    "stt_model":       ("PANTRY_STT_MODEL", False, "Speech model (tiny, base, small, medium); small by default"),
    "name_lang":       ("PANTRY_NAME_LANG", False, "Product name language from Open Food Facts: zh, en or auto"),
    "soon_days":       ("PANTRY_SOON_DAYS", False, "Days counted as 'expiring soon'"),
}


class Config:
    """Settings with the database first and the environment as fallback."""

    def __init__(self, db, lock):
        self.db, self.lock = db, lock
        self.db.execute("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)")
        self.db.commit()
        self.listeners = []

    def get(self, key, default=None):
        with self.lock:
            row = self.db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        if row is not None and row["value"] is not None:
            return row["value"]
        env = SETTING_KEYS.get(key, (None,))[0]
        v = os.environ.get(env) if env else None
        return v if v not in (None, "") else default

    def public(self):
        """For the web page: secrets are reported as set/unset only."""
        out = {}
        for key, (env, secret, desc) in SETTING_KEYS.items():
            v = self.get(key)
            out[key] = {"description": desc, "secret": secret,
                        "value": (None if secret else (v or "")), "set": bool(v)}
        return out

    def update(self, values):
        """values: {key: str|None}. None/'' for a secret keeps it unless clear_<key> is true."""
        changed = set()
        with self.lock:
            for key, v in values.items():
                if key.startswith("clear_") and v and key[6:] in SETTING_KEYS:
                    self.db.execute("REPLACE INTO settings (key,value) VALUES (?,?)", (key[6:], ""))
                    changed.add(key[6:])
                    continue
                if key not in SETTING_KEYS:
                    continue
                v = ("" if v is None else str(v)).strip()
                if SETTING_KEYS[key][1] and v == "":
                    continue  # blank secret field: keep what we have
                self.db.execute("REPLACE INTO settings (key,value) VALUES (?,?)", (key, v))
                changed.add(key)
            self.db.commit()
        for fn in self.listeners:
            fn(changed)
        return changed


CONFIG: "Config" = None


# --- Storage -----------------------------------------------------------------

class Store:
    def __init__(self, data_dir):
        os.makedirs(os.path.join(data_dir, "thumbs"), exist_ok=True)
        os.makedirs(os.path.join(data_dir, "images"), exist_ok=True)
        self.data_dir = data_dir
        self.lock = threading.Lock()
        self.db = sqlite3.connect(os.path.join(data_dir, "pantry.db"), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS products (
                barcode TEXT PRIMARY KEY, found INTEGER NOT NULL, name TEXT, brand TEXT,
                quantity TEXT, image_url TEXT, has_image INTEGER NOT NULL DEFAULT 0,
                fetched TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS items (
                id INTEGER PRIMARY KEY AUTOINCREMENT, barcode TEXT NOT NULL,
                name TEXT NOT NULL, expires TEXT NOT NULL, added TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS pantries (
                id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, created TEXT NOT NULL);
        """)
        self._add_column("items", "pantry_id", "INTEGER NOT NULL DEFAULT 1")
        self._add_column("products", "source", "TEXT")
        self._add_column("products", "user_name", "TEXT")
        if not self.db.execute("SELECT 1 FROM pantries WHERE id=1").fetchone():
            self.db.execute("INSERT INTO pantries (id,name,created) VALUES (1,?,?)", (DEFAULT_PANTRY, utcnow()))
        self.db.commit()
        global CONFIG
        CONFIG = Config(self.db, self.lock)

    def _add_column(self, table, col, decl):
        cols = [r["name"] for r in self.db.execute(f"PRAGMA table_info({table})")]
        if col not in cols:
            self.db.execute(f"ALTER TABLE {table} ADD COLUMN {col} {decl}")

    def image_path(self, barcode):
        return os.path.join(self.data_dir, "images", f"{barcode}.jpg")

    def thumb_path(self, barcode, size):
        return os.path.join(self.data_dir, "thumbs", f"{barcode}_{size}.jpg")

    # Pantries ---------------------------------------------------------------

    def pantries(self):
        today = dt.date.today()
        with self.lock:
            rows = self.db.execute("SELECT * FROM pantries ORDER BY id").fetchall()
            items = self.db.execute("SELECT pantry_id, expires FROM items").fetchall()
        out = []
        for r in rows:
            mine = [i for i in items if i["pantry_id"] == r["id"]]
            left = [(dt.date.fromisoformat(i["expires"]) - today).days for i in mine]
            out.append({"id": r["id"], "name": r["name"], "items": len(mine),
                        "expired": sum(1 for d in left if d < 0),
                        "expiring_soon": sum(1 for d in left if 0 <= d <= soon_days())})
        return out

    def add_pantry(self, name=None):
        with self.lock:
            if not name:
                n = self.db.execute("SELECT COUNT(*) FROM pantries").fetchone()[0]
                name = f"Pantry {n}"
            cur = self.db.execute("INSERT INTO pantries (name,created) VALUES (?,?)", (name, utcnow()))
            self.db.commit()
            new_id = cur.lastrowid
        return next(p for p in self.pantries() if p["id"] == new_id)

    def rename_pantry(self, pantry_id, name):
        with self.lock:
            n = self.db.execute("UPDATE pantries SET name=? WHERE id=?", (name, pantry_id)).rowcount
            self.db.commit()
        return n > 0

    def delete_pantry(self, pantry_id):
        if pantry_id == 1:
            return False
        with self.lock:
            n = self.db.execute("DELETE FROM pantries WHERE id=?", (pantry_id,)).rowcount
            if n:
                self.db.execute("UPDATE items SET pantry_id=1 WHERE pantry_id=?", (pantry_id,))
            self.db.commit()
        return n > 0

    def pantry_exists(self, pantry_id):
        with self.lock:
            return self.db.execute("SELECT 1 FROM pantries WHERE id=?", (pantry_id,)).fetchone() is not None

    # Products ---------------------------------------------------------------

    def product(self, barcode, refresh=False):
        """Cached product record; looks it up when missing, stale-unknown or refresh."""
        with self.lock:
            row = self.db.execute("SELECT * FROM products WHERE barcode=?", (barcode,)).fetchone()
        if row and not refresh:
            age = dt.datetime.utcnow() - dt.datetime.fromisoformat(row["fetched"])
            if row["found"] or age < dt.timedelta(days=1):
                return self._present(dict(row))
        rec = lookup_product(barcode)
        rec["has_image"] = 0
        if rec["image_url"]:
            data = fetch_bytes(rec["image_url"])
            if data:
                try:
                    from PIL import Image
                    im = Image.open(io.BytesIO(data)).convert("RGB")
                    im.thumbnail((240, 240))
                    im.save(self.image_path(barcode), "JPEG", quality=85)
                    rec["has_image"] = 1
                except Exception as e:  # noqa: BLE001 - a bad image must not block the add
                    print(f"image for {barcode} unusable: {e}", file=sys.stderr)
        rec["fetched"] = utcnow()
        rec["user_name"] = row["user_name"] if row else None
        if rec.pop("transient", False) and not rec["found"]:
            return self._present(rec)  # a source was rate limited: do not cache the miss
        with self.lock:
            self.db.execute(
                "REPLACE INTO products (barcode,found,name,brand,quantity,image_url,has_image,fetched,source,user_name)"
                " VALUES (:barcode,:found,:name,:brand,:quantity,:image_url,:has_image,:fetched,:source,:user_name)", rec)
            if rec["found"] and rec["name"]:  # items added while it was unknown take the real name
                self.db.execute("UPDATE items SET name=? WHERE barcode=? AND name LIKE 'Unknown item %'", (rec["name"], barcode))
            self.db.commit()
            for f in os.listdir(os.path.join(self.data_dir, "thumbs")):
                if f.startswith(barcode + "_"):
                    os.remove(os.path.join(self.data_dir, "thumbs", f))
        return self._present(rec)

    @staticmethod
    def _present(rec):
        """What clients see: your own name wins, and `source` says who named it."""
        out = {k: rec.get(k) for k in ("barcode", "found", "name", "brand", "quantity", "has_image", "source")}
        out["has_image"] = bool(out["has_image"])
        if rec.get("user_name"):
            out.update(name=rec["user_name"], found=1, source="user")
        return out

    def name_product(self, barcode, name):
        self.product(barcode)  # make sure a row exists
        with self.lock:
            self.db.execute("UPDATE products SET user_name=? WHERE barcode=?", (name or None, barcode))
            if name:
                self.db.execute("UPDATE items SET name=? WHERE barcode=?", (name, barcode))
            self.db.commit()
        return self.product(barcode)

    def thumb(self, barcode, size):
        """Baseline JPEG fitting a size x size box, or None. Cached on disk."""
        p = self.thumb_path(barcode, size)
        if os.path.exists(p):
            return open(p, "rb").read()
        src = self.image_path(barcode)
        if not os.path.exists(src):
            return None
        from PIL import Image
        im = Image.open(src).convert("RGB")
        im.thumbnail((size, size))
        # Pad to a white square so the device can blit it into a fixed box.
        square = Image.new("RGB", (size, size), "white")
        square.paste(im, ((size - im.width) // 2, (size - im.height) // 2))
        buf = io.BytesIO()
        square.save(buf, "JPEG", quality=80, optimize=False, progressive=False)
        with open(p, "wb") as f:
            f.write(buf.getvalue())
        return buf.getvalue()

    # Items ------------------------------------------------------------------

    def items(self, pantry_id=None):
        today = dt.date.today()
        sql = ("SELECT i.*, p.brand, p.quantity, p.has_image, p.source, p.user_name, n.name AS pantry FROM items i"
               " LEFT JOIN products p ON p.barcode=i.barcode LEFT JOIN pantries n ON n.id=i.pantry_id")
        args = ()
        if pantry_id is not None:
            sql += " WHERE i.pantry_id=?"
            args = (pantry_id,)
        with self.lock:
            rows = self.db.execute(sql + " ORDER BY i.expires, i.id", args).fetchall()
        out = []
        for r in rows:
            d = dict(r)
            d["days_left"] = (dt.date.fromisoformat(d["expires"]) - today).days
            d["days_ago"] = (today - dt.date.fromisoformat(d["added"])).days
            d["has_image"] = bool(d["has_image"])
            if d.pop("user_name", None):
                d["source"] = "user"
            out.append(d)
        return today.isoformat(), out

    def add_item(self, barcode, expires, name=None, pantry_id=1):
        prod = self.product(barcode)
        if not name:
            name = prod["name"] or f"Unknown item {barcode}"
        with self.lock:
            cur = self.db.execute(
                "INSERT INTO items (barcode,name,expires,added,pantry_id) VALUES (?,?,?,?,?)",
                (barcode, name, expires, dt.date.today().isoformat(), pantry_id))
            self.db.commit()
            new_id = cur.lastrowid
        _, items = self.items()
        return next(i for i in items if i["id"] == new_id)

    def update_item(self, item_id, name=None, expires=None, pantry_id=None):
        with self.lock:
            row = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
        if not row:
            return None
        if name:
            self.name_product(row["barcode"], name)  # every item with this barcode, and the next scan
        with self.lock:
            if expires:
                self.db.execute("UPDATE items SET expires=? WHERE id=?", (expires, item_id))
            if pantry_id:
                self.db.execute("UPDATE items SET pantry_id=? WHERE id=?", (pantry_id, item_id))
            self.db.commit()
        _, items = self.items()
        return next(i for i in items if i["id"] == item_id)

    def delete_item(self, item_id):
        with self.lock:
            n = self.db.execute("DELETE FROM items WHERE id=?", (item_id,)).rowcount
            self.db.commit()
        return n > 0


# --- Product lookup chain -----------------------------------------------------

def fetch_bytes(url, timeout=10, headers=None):
    try:
        h = {"User-Agent": USER_AGENT}
        h.update(headers or {})
        req = urllib.request.Request(url, headers=h)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        print(f"fetch {url}: {e}", file=sys.stderr)
        return None


def fetch_json(url, timeout=10, headers=None):
    data = fetch_bytes(url, timeout, headers)
    if not data:
        return None
    try:
        return json.loads(data)
    except ValueError:
        print(f"fetch {url}: not json", file=sys.stderr)
        return None


def empty_record(barcode):
    return {"barcode": barcode, "found": 0, "name": None, "brand": None, "quantity": None,
            "image_url": None, "source": None}


def clean(s):
    return (str(s).strip() or None) if s is not None else None


def lookup_product(barcode):
    """First source that knows the barcode wins; an unknown record otherwise.
    A rate-limited source marks the miss transient so it is not cached."""
    transient = False
    for fn in (lookup_open_food_facts, lookup_gs1_china, lookup_keyed_api):
        try:
            rec = fn(barcode)
        except gs1_china.RateLimited:
            transient = True
            rec = None
        except Exception as e:  # noqa: BLE001 - one broken source must not block the others
            print(f"{fn.__name__} {barcode}: {e}", file=sys.stderr)
            rec = None
        if rec:
            return rec
    rec = empty_record(barcode)
    rec["transient"] = transient
    return rec


def lookup_open_food_facts(barcode):
    j = fetch_json(OFF_URL.format(code=barcode))
    if not j or j.get("status") != 1:
        return None
    p = j.get("product", {})
    # Name language: zh (default, the device renders CJK), en, or auto (the product's own).
    lang = (CONFIG.get("name_lang", "zh") if CONFIG else "zh").lower()
    zh, en, own = clean(p.get("product_name_zh")), clean(p.get("product_name_en")), clean(p.get("product_name"))
    name = (zh or own or en) if lang == "zh" else (en or own or zh) if lang == "en" else (own or en or zh)
    rec = empty_record(barcode)
    rec.update(found=1, source="off", name=name, brand=clean(p.get("brands")), quantity=clean(p.get("quantity")),
               image_url=p.get("image_front_small_url") or p.get("image_front_url"))
    return rec


def lookup_gs1_china(barcode):
    """中国商品信息服务平台 via gs1_china.py (logs in with PANTRY_GDS_USER / PANTRY_GDS_PASS)."""
    p = gs1_china.lookup(barcode)
    if not p:
        return None
    rec = empty_record(barcode)
    rec.update(found=1, source="gs1cn", name=p.get("name"), brand=p.get("brand"),
               quantity=p.get("quantity"), image_url=p.get("image_url"))
    return rec if rec["name"] else None


def lookup_keyed_api(barcode):
    """Generic slot for a paid/keyed JSON API (Aliyun Market style). PANTRY_BARCODE_URL
    contains {code}; PANTRY_BARCODE_APPCODE goes in the Authorization header.
    Common Chinese field names are tried; the raw reply is logged when none match."""
    url = CONFIG.get("barcode_url") if CONFIG else None
    if not url:
        return None
    headers = {}
    appcode = CONFIG.get("barcode_appcode") if CONFIG else None
    if appcode:
        headers["Authorization"] = "APPCODE " + appcode
    j = fetch_json(url.replace("{code}", urllib.parse.quote(barcode)), headers=headers)
    if not j:
        return None
    node = j  # dig to the first dict that looks like a product
    for key in ("result", "data", "showapi_res_body", "newslist"):
        if isinstance(node, dict) and key in node:
            node = node[key]
    if isinstance(node, list):
        node = node[0] if node else {}
    if not isinstance(node, dict):
        return None

    def pick(*keys):
        for k in keys:
            v = node.get(k)
            if v:
                return clean(v)
        return None
    name = pick("goodsName", "name", "productName", "product_name", "title", "description")
    if not name:
        print(f"keyed api {barcode}: no name in {json.dumps(j, ensure_ascii=False)[:300]}", file=sys.stderr)
        return None
    rec = empty_record(barcode)
    rec.update(found=1, source="api", name=name, brand=pick("brand", "brandName", "trademark", "manuName"),
               quantity=pick("spec", "specification", "standard", "netContent"),
               image_url=pick("img", "image", "imageUrl", "picture", "pic"))
    return rec


# --- Speech to text (naming items by voice) -------------------------------------

_stt = {"model": None, "error": None, "name": None}


def STT_MODEL():  # noqa: N802 - kept as a callable so settings changes apply
    return (CONFIG.get("stt_model", "small") if CONFIG else "small").strip() or "small"
STT_PROMPT = "食品名称，饮料，零食，牛奶，酸奶，Nutella, milk, yoghurt, juice, snack"


def stt_text():
    st = stt_status()
    if not st["available"]:
        return "not installed"
    return ("loaded" if st["loaded"] else "ready") + f" ({st['model']})"


def stt_status():
    try:
        import faster_whisper  # noqa: F401
        have = True
    except ImportError:
        have = False
    return {"available": have, "model": STT_MODEL(), "loaded": _stt["model"] is not None, "error": _stt["error"]}


def _stt_model():
    if _stt["model"] is None or _stt["name"] != STT_MODEL():
        from faster_whisper import WhisperModel
        _stt["model"] = WhisperModel(STT_MODEL(), device="cpu", compute_type="int8", cpu_threads=max(1, os.cpu_count() or 1))
        _stt["name"] = STT_MODEL()
    return _stt["model"]


def pcm16_to_float(data):
    import array
    a = array.array("h")
    a.frombytes(data[: len(data) - len(data) % 2])
    return [x / 32768.0 for x in a]


def transcribe(audio, content_type, lang=None):
    """Returns (text, language). Accepts a WAV (16 kHz mono PCM16 preferred) or raw PCM16 @16 kHz."""
    import numpy as np
    rate = 16000
    if audio[:4] == b"RIFF":
        import wave
        with wave.open(io.BytesIO(audio)) as w:
            rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
            frames = w.readframes(w.getnframes())
        if sw != 2:
            raise ValueError("wav must be 16-bit")
        samples = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
        if ch > 1:
            samples = samples.reshape(-1, ch).mean(axis=1)
    else:
        samples = np.frombuffer(audio[: len(audio) - len(audio) % 2], dtype=np.int16).astype(np.float32) / 32768.0
    if rate != 16000:  # crude linear resample; the device sends 16 kHz anyway
        idx = np.linspace(0, len(samples) - 1, int(len(samples) * 16000 / rate))
        samples = np.interp(idx, np.arange(len(samples)), samples).astype(np.float32)
    model = _stt_model()
    segments, info = model.transcribe(samples, language=None if lang in (None, "", "auto") else lang,
                                      beam_size=2, vad_filter=True, initial_prompt=STT_PROMPT,
                                      condition_on_previous_text=False)
    text = "".join(seg.text for seg in segments).strip()
    # Product names: drop trailing punctuation the model likes to add.
    text = text.rstrip("。.!！,，").strip()
    return text, info.language


# --- Home Assistant over MQTT --------------------------------------------------

DEVICE = {"identifiers": ["pantry_tracker"], "name": "Pantry Tracker", "manufacturer": "Braven",
          "model": "CoreS3 + Module13.2 QRCode", "sw_version": VERSION}


class Mqtt:
    """Home Assistant MQTT discovery for the whole tracker.

    Topics (all retained except the event):
        pantry/availability        online / offline (last will)
        pantry/state               totals, next expiry, expiring list, items[], pantries[]
        pantry/<id>/state          the same per pantry
        pantry/device              CoreS3 telemetry (battery, charging, rssi, ip, last scan, last seen)
        pantry/diag                server version, GS1 China and speech status
        pantry/event               {"event_type": scanned|added|removed|renamed, ...}  (not retained)
    Entities: four summary sensors, next expiry date, expiring list, per-pantry devices
    (via_device the tracker), battery / charging / rssi / ip / last scan / last seen,
    three diagnostic sensors and an `event` entity. No-op when PANTRY_MQTT_HOST is unset
    or paho-mqtt is missing."""

    def __init__(self):
        self.client = None
        self.connected = False
        self._pantry_ids = set()
        self._retained = {}
        host = CONFIG.get("mqtt_host") if CONFIG else os.environ.get("PANTRY_MQTT_HOST")
        if not host:
            return
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("paho-mqtt not installed; MQTT disabled", file=sys.stderr)
            return
        try:
            port = int(CONFIG.get("mqtt_port", "1883") or 1883)
        except ValueError:
            port = 1883
        if hasattr(mqtt, "CallbackAPIVersion"):  # paho 2.x
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pantry-tracker")
        else:  # paho 1.x (Ubuntu 24.04's python3-paho-mqtt)
            self.client = mqtt.Client(client_id="pantry-tracker")
        user = CONFIG.get("mqtt_user")
        if user:
            self.client.username_pw_set(user, CONFIG.get("mqtt_pass", "") or "")
        self.client.will_set("pantry/availability", "offline", retain=True)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.connect_async(host, port, keepalive=60)
        self.client.loop_start()
        self.host = f"{host}:{port}"

    @property
    def enabled(self):
        return self.client is not None

    def stop(self):
        if self.client:
            try:
                self.client.publish("pantry/availability", "offline", retain=True)
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:  # noqa: BLE001
                pass
        self.client = None
        self.connected = False

    def _on_disconnect(self, *args, **kwargs):
        self.connected = False

    def _on_connect(self, client, userdata, flags, reason, props=None):  # same shape in paho 1.x and 2.x
        self.connected = True
        self._discover_fixed()
        for topic, payload in list(self._retained.items()):  # replay state after a reconnect
            client.publish(topic, payload, retain=True)
        client.publish("pantry/availability", "online", retain=True)

    # Discovery ----------------------------------------------------------------

    def _cfg(self, component, key, name, extra, device=None, state_topic="pantry/state"):
        cfg = {"name": name, "unique_id": f"pantry_{key}", "state_topic": state_topic, "device": device or DEVICE,
               "availability_topic": "pantry/availability"}
        cfg.update(extra)
        self.client.publish(f"homeassistant/{component}/pantry_{key}/config", json.dumps(cfg), retain=True)

    def _remove(self, component, key):
        self.client.publish(f"homeassistant/{component}/pantry_{key}/config", "", retain=True)

    def _discover_fixed(self):
        c = self._cfg
        count = {"unit_of_measurement": "items", "state_class": "measurement"}
        # Summary over every pantry.
        c("sensor", "expired", "Pantry expired", dict(count, icon="mdi:food-off", value_template="{{ value_json.expired }}",
                                                      json_attributes_topic="pantry/state", json_attributes_template="{{ {'items': value_json.expired_items} | tojson }}"))
        c("sensor", "expiring_soon", "Pantry expiring soon", dict(count, icon="mdi:clock-alert", value_template="{{ value_json.expiring_soon }}",
                                                                  json_attributes_topic="pantry/state", json_attributes_template="{{ {'items': value_json.expiring_items, 'threshold_days': value_json.soon_days} | tojson }}"))
        c("sensor", "total", "Pantry items", dict(count, icon="mdi:fridge", value_template="{{ value_json.total }}",
                                                  json_attributes_topic="pantry/state", json_attributes_template="{{ {'items': value_json.items, 'pantries': value_json.pantries} | tojson }}"))
        c("sensor", "next_expiry", "Pantry next expiry", {"icon": "mdi:calendar-clock", "value_template": "{{ value_json.next_name }}",
                                                          "json_attributes_topic": "pantry/state",
                                                          "json_attributes_template": "{{ {'expires': value_json.next_expires, 'days_left': value_json.next_days_left, 'barcode': value_json.next_barcode, 'pantry': value_json.next_pantry} | tojson }}"})
        c("sensor", "next_expiry_date", "Pantry next expiry date", {"device_class": "date", "icon": "mdi:calendar",
                                                                    "value_template": "{{ value_json.next_expires if value_json.next_expires else None }}"})
        c("sensor", "expiring_list", "Pantry expiring list", {"icon": "mdi:format-list-bulleted", "value_template": "{{ value_json.expiring_text }}"})
        # The CoreS3 itself.
        c("sensor", "battery", "Pantry Tracker battery", {"device_class": "battery", "unit_of_measurement": "%", "state_class": "measurement",
                                                           "value_template": "{{ value_json.battery }}", "entity_category": "diagnostic"}, state_topic="pantry/device")
        c("binary_sensor", "charging", "Pantry Tracker charging", {"device_class": "battery_charging", "value_template": "{{ 'ON' if value_json.charging else 'OFF' }}",
                                                                    "entity_category": "diagnostic"}, state_topic="pantry/device")
        c("sensor", "rssi", "Pantry Tracker WiFi signal", {"device_class": "signal_strength", "unit_of_measurement": "dBm", "state_class": "measurement",
                                                            "value_template": "{{ value_json.rssi }}", "entity_category": "diagnostic"}, state_topic="pantry/device")
        c("sensor", "ip", "Pantry Tracker IP", {"icon": "mdi:ip-network", "value_template": "{{ value_json.ip }}", "entity_category": "diagnostic"}, state_topic="pantry/device")
        c("sensor", "last_scan", "Pantry Tracker last scan", {"icon": "mdi:barcode-scan", "value_template": "{{ value_json.last_scan_name }}",
                                                               "json_attributes_topic": "pantry/device",
                                                               "json_attributes_template": "{{ {'barcode': value_json.last_scan, 'at': value_json.last_scan_at} | tojson }}"}, state_topic="pantry/device")
        c("sensor", "last_seen", "Pantry Tracker last seen", {"device_class": "timestamp", "value_template": "{{ value_json.last_seen }}",
                                                               "entity_category": "diagnostic"}, state_topic="pantry/device")
        c("sensor", "firmware", "Pantry Tracker firmware", {"icon": "mdi:chip", "value_template": "{{ value_json.firmware }}", "entity_category": "diagnostic"}, state_topic="pantry/device")
        # Server diagnostics.
        c("sensor", "server_version", "Pantry server version", {"icon": "mdi:server", "value_template": "{{ value_json.version }}", "entity_category": "diagnostic"}, state_topic="pantry/diag")
        c("sensor", "gs1_status", "Pantry GS1 China", {"icon": "mdi:database-search", "value_template": "{{ value_json.gs1 }}", "entity_category": "diagnostic"}, state_topic="pantry/diag")
        c("sensor", "stt_status", "Pantry speech recognition", {"icon": "mdi:microphone-message", "value_template": "{{ value_json.stt }}", "entity_category": "diagnostic"}, state_topic="pantry/diag")
        # Events for automations: scanned / added / removed / renamed.
        c("event", "events", "Pantry Tracker event", {"event_types": ["scanned", "added", "removed", "renamed", "pantry_created", "pantry_deleted"],
                                                       "icon": "mdi:barcode-scan"}, state_topic="pantry/event")

    def _discover_pantries(self, pantries):
        ids = {p["id"] for p in pantries}
        for pid in self._pantry_ids - ids:  # deleted pantries: drop their entities
            for key in ("items", "expired", "expiring_soon", "next_expiry", "next_expiry_date"):
                self._remove("sensor", f"p{pid}_{key}")
        for p in pantries:
            pid = p["id"]
            dev = {"identifiers": [f"pantry_{pid}"], "name": f"Pantry: {p['name']}", "manufacturer": "Braven",
                   "model": "Pantry", "via_device": "pantry_tracker"}
            t = f"pantry/{pid}/state"
            count = {"unit_of_measurement": "items", "state_class": "measurement"}
            self._cfg("sensor", f"p{pid}_items", f"{p['name']} items", dict(count, icon="mdi:fridge", value_template="{{ value_json.total }}",
                                                                           json_attributes_topic=t, json_attributes_template="{{ {'items': value_json.items} | tojson }}"), dev, t)
            self._cfg("sensor", f"p{pid}_expired", f"{p['name']} expired", dict(count, icon="mdi:food-off", value_template="{{ value_json.expired }}"), dev, t)
            self._cfg("sensor", f"p{pid}_expiring_soon", f"{p['name']} expiring soon", dict(count, icon="mdi:clock-alert", value_template="{{ value_json.expiring_soon }}"), dev, t)
            self._cfg("sensor", f"p{pid}_next_expiry", f"{p['name']} next expiry", {"icon": "mdi:calendar-clock", "value_template": "{{ value_json.next_name }}"}, dev, t)
            self._cfg("sensor", f"p{pid}_next_expiry_date", f"{p['name']} next expiry date", {"device_class": "date",
                      "value_template": "{{ value_json.next_expires if value_json.next_expires else None }}"}, dev, t)
        self._pantry_ids = ids

    # State ---------------------------------------------------------------------

    def _retain(self, topic, payload):
        self._retained[topic] = payload
        if self.client:
            self.client.publish(topic, payload, retain=True)

    @staticmethod
    def _summary(today, items):
        nxt = items[0] if items else None
        soon = [i for i in items if 0 <= i["days_left"] <= soon_days()]
        expired = [i for i in items if i["days_left"] < 0]
        def label(i):
            d = i["days_left"]
            return f"{i['name']} ({'expired' if d < 0 else 'today' if d == 0 else str(d) + ' d'})"
        text = ", ".join(label(i) for i in expired + soon)
        return {
            "today": today, "total": len(items), "expired": len(expired), "expiring_soon": len(soon),
            "soon_days": soon_days(),
            "next_name": nxt["name"] if nxt else "none",
            "next_expires": nxt["expires"] if nxt else None,
            "next_days_left": nxt["days_left"] if nxt else None,
            "next_barcode": nxt["barcode"] if nxt else None,
            "next_pantry": nxt.get("pantry") if nxt else None,
            "expiring_text": (text[:250] + "…") if len(text) > 250 else (text or "nothing"),
            "expired_items": [i["name"] for i in expired],
            "expiring_items": [f"{i['name']} ({i['days_left']} d)" for i in soon],
            "items": [{k: i.get(k) for k in ("id", "name", "barcode", "expires", "days_left", "pantry", "pantry_id")} for i in items],
        }

    def publish_state(self, today, items, pantries):
        if not self.client:
            return
        self._discover_pantries(pantries)
        state = self._summary(today, items)
        state["pantries"] = pantries
        self._retain("pantry/state", json.dumps(state, ensure_ascii=False))
        for p in pantries:
            mine = [i for i in items if i.get("pantry_id") == p["id"]]
            ps = self._summary(today, mine)
            ps["pantry"] = p["name"]
            self._retain(f"pantry/{p['id']}/state", json.dumps(ps, ensure_ascii=False))

    def publish_diag(self, gs1, stt):
        if not self.client:
            return
        self._retain("pantry/diag", json.dumps({"version": VERSION, "gs1": gs1, "stt": stt}, ensure_ascii=False))

    def publish_device(self, payload):
        """Telemetry posted by the CoreS3 (merged with what we already know)."""
        if not self.client:
            return
        try:
            cur = json.loads(self._retained.get("pantry/device", "{}"))
        except ValueError:
            cur = {}
        cur.update(payload)
        cur["last_seen"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
        self._retain("pantry/device", json.dumps(cur, ensure_ascii=False))

    def publish_event(self, event_type, **data):
        if not self.client:
            return
        payload = {"event_type": event_type}
        payload.update(data)
        self.client.publish("pantry/event", json.dumps(payload, ensure_ascii=False), retain=False)


# --- HTTP ---------------------------------------------------------------------

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
BARCODE_RE = re.compile(r"^[0-9A-Za-z._-]{1,64}$")


def valid_date(s):
    if not s or not DATE_RE.match(s):
        return False
    try:
        dt.date.fromisoformat(s)
    except ValueError:
        return False
    return True


class Handler(BaseHTTPRequestHandler):
    server_version = f"PantryTracker/{VERSION}"
    store: Store = None
    mqtt: Mqtt = None

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    # helpers
    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, data, ctype, code=200, cache="public, max-age=86400"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(data)

    def send_empty(self, code=204):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def error(self, code, msg):
        self.send_json({"error": msg}, code)

    def read_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 65536:
            return None
        try:
            return json.loads(self.rfile.read(n))
        except ValueError:
            return None

    def changed(self, event=None, **data):
        today, items = self.store.items()
        self.mqtt.publish_state(today, items, self.store.pantries())
        self.mqtt.publish_diag(gs1_china.status(), stt_text())
        if event:
            self.mqtt.publish_event(event, **data)

    # routes
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query, keep_blank_values=True)
        path = u.path.rstrip("/") or "/"
        if path == "/":
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                return self.send_bytes(f.read(), "text/html; charset=utf-8", cache="no-cache")
        if path == "/api/health":
            _, items = self.store.items()
            return self.send_json({"ok": True, "version": VERSION, "items": len(items),
                                   "mqtt": getattr(self.mqtt, "host", None) if self.mqtt.enabled else None,
                                   "mqtt_connected": self.mqtt.connected, "gs1_china": gs1_china.status()})
        if path == "/api/pantries":
            return self.send_json({"pantries": self.store.pantries()})
        if path == "/api/settings":
            return self.send_json({"settings": CONFIG.public(), "env_file": "/etc/pantry.env"})
        if path == "/api/gs1":
            return self.send_json({"status": gs1_china.status(), "signed_in": gs1_china.signed_in(),
                                   "configured": gs1_china.configured()})
        if path == "/api/gs1/login":   # start an interactive sign-in: returns the captcha
            return self.send_json(gs1_china.login_begin())
        if path == "/api/stt":
            return self.send_json(stt_status())
        if path == "/api/items":
            pantry = q.get("pantry", [None])[0]
            pid = None
            if pantry not in (None, "", "all"):
                if not pantry.isdigit() or not self.store.pantry_exists(int(pantry)):
                    return self.error(404, "no such pantry")
                pid = int(pantry)
            today, items = self.store.items(pid)
            barcode = q.get("barcode", [None])[0]
            if barcode:  # every item with this barcode (any pantry unless pantry= given), soonest first
                items = [i for i in items if i["barcode"] == barcode]
            return self.send_json({"today": today, "pantry": pid, "items": items,
                                   "mqtt_connected": self.mqtt.connected})
        m = re.match(r"^/api/products/([^/]+)(/thumb\.jpg)?$", path)
        if m:
            barcode = m.group(1)
            if not BARCODE_RE.match(barcode):
                return self.error(400, "bad barcode")
            if m.group(2):
                size = max(16, min(240, int(q.get("s", ["64"])[0])))
                self.store.product(barcode)
                data = self.store.thumb(barcode, size)
                if data is None:
                    return self.error(404, "no image")
                return self.send_bytes(data, "image/jpeg")
            return self.send_json(self.store.product(barcode, refresh="refresh" in q))
        self.error(404, "not found")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        path = u.path.rstrip("/")
        if path == "/api/transcribe":
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 44 or n > 4_000_000:
                return self.error(400, "audio body required (wav or pcm16 mono 16 kHz, <4 MB)")
            audio = self.rfile.read(n)
            q = urllib.parse.parse_qs(u.query, keep_blank_values=True)
            lang = (q.get("lang") or [None])[0]
            try:
                text, detected = transcribe(audio, self.headers.get("Content-Type", ""), lang)
            except Exception as e:  # noqa: BLE001
                print(f"transcribe: {e}", file=sys.stderr)
                return self.error(503, f"transcriber unavailable: {e}")
            return self.send_json({"text": text, "language": detected})
        body = self.read_json() or {}
        if path == "/api/items":
            if not BARCODE_RE.match(str(body.get("barcode", ""))):
                return self.error(400, "barcode required")
            expires = str(body.get("expires", ""))
            if not valid_date(expires):
                return self.error(400, "expires must be a real YYYY-MM-DD date")
            pid = body.get("pantry_id") or 1
            if not isinstance(pid, int) or not self.store.pantry_exists(pid):
                return self.error(400, "no such pantry")
            item = self.store.add_item(str(body["barcode"]), expires, (body.get("name") or "").strip() or None, pid)
            self.changed("added", name=item["name"], barcode=item["barcode"], expires=item["expires"],
                         days_left=item["days_left"], pantry=item.get("pantry"), source=item.get("source"))
            self.mqtt.publish_device({"last_scan": item["barcode"], "last_scan_name": item["name"],
                                      "last_scan_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")})
            return self.send_json(item, 201)
        if path == "/api/pantries":
            name = (body.get("name") or "").strip()[:40] or None
            pantry = self.store.add_pantry(name)
            self.changed("pantry_created", pantry=pantry["name"], pantry_id=pantry["id"])
            return self.send_json(pantry, 201)
        if path == "/api/settings":
            changed = CONFIG.update(body)
            if changed & {"mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass"}:
                Handler.mqtt.stop()
                Handler.mqtt = Mqtt()
                self.changed()
            if changed & {"gds_user", "gds_pass"}:
                gs1_china.credentials_changed()
            return self.send_json({"ok": True, "changed": sorted(changed), "settings": CONFIG.public()})
        if path == "/api/device":          # CoreS3 telemetry: {battery, charging, rssi, ip, firmware, uptime_s, event?, barcode?}
            allowed = {k: body[k] for k in ("battery", "charging", "rssi", "ip", "firmware", "uptime_s", "screen") if k in body}
            self.mqtt.publish_device(allowed)
            if body.get("event") == "scanned" and body.get("barcode"):
                prod = self.store.product(str(body["barcode"]))
                self.mqtt.publish_event("scanned", barcode=str(body["barcode"]), name=prod.get("name"),
                                        known=bool(prod.get("found")), source=prod.get("source"))
                self.mqtt.publish_device({"last_scan": str(body["barcode"]), "last_scan_name": prod.get("name") or "unknown",
                                          "last_scan_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")})
            return self.send_json({"ok": True})
        if path == "/api/gs1/login":       # {code}: captcha answer
            return self.send_json(gs1_china.login_submit(str(body.get("code", ""))))
        if path == "/api/gs1/sms":         # ask GS1 to text the code
            return self.send_json(gs1_china.sms_send())
        if path == "/api/gs1/sms-verify":  # {code}: the SMS code
            return self.send_json(gs1_china.sms_verify(str(body.get("code", ""))))
        self.error(404, "not found")

    def do_PATCH(self):
        path = self.path.rstrip("/")
        body = self.read_json() or {}
        m = re.match(r"^/api/items/(\d+)$", path)
        if m:
            name = (body.get("name") or "").strip()[:80] or None
            expires = body.get("expires")
            if expires is not None and not valid_date(str(expires)):
                return self.error(400, "expires must be a real YYYY-MM-DD date")
            pid = body.get("pantry_id")
            if pid is not None and (not isinstance(pid, int) or not self.store.pantry_exists(pid)):
                return self.error(400, "no such pantry")
            item = self.store.update_item(int(m.group(1)), name, expires, pid)
            if not item:
                return self.error(404, "no such item")
            self.changed("renamed" if name else None, name=item["name"], barcode=item["barcode"])
            return self.send_json(item)
        m = re.match(r"^/api/pantries/(\d+)$", path)
        if m:
            name = (body.get("name") or "").strip()[:40]
            if not name:
                return self.error(400, "name required")
            if not self.store.rename_pantry(int(m.group(1)), name):
                return self.error(404, "no such pantry")
            self.changed()
            return self.send_json({"ok": True})
        m = re.match(r"^/api/products/([^/]+)$", path)
        if m and BARCODE_RE.match(m.group(1)):
            rec = self.store.name_product(m.group(1), (body.get("name") or "").strip()[:80])
            self.changed()
            return self.send_json(rec)
        self.error(404, "not found")

    def do_DELETE(self):
        path = self.path.rstrip("/")
        m = re.match(r"^/api/items/(\d+)$", path)
        if m:
            _, before = self.store.items()
            gone = next((i for i in before if i["id"] == int(m.group(1))), None)
            if not self.store.delete_item(int(m.group(1))):
                return self.error(404, "no such item")
            self.changed("removed", name=gone["name"] if gone else None, barcode=gone["barcode"] if gone else None,
                         pantry=gone.get("pantry") if gone else None)
            return self.send_empty()
        m = re.match(r"^/api/pantries/(\d+)$", path)
        if m:
            if not self.store.delete_pantry(int(m.group(1))):
                return self.error(404, "no such pantry (or it is the default one)")
            self.changed("pantry_deleted", pantry_id=int(m.group(1)))
            return self.send_empty()
        self.error(404, "not found")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    a = ap.parse_args()
    Handler.store = Store(a.data)
    gs1_china.configure(a.data, lambda: (CONFIG.get("gds_user"), CONFIG.get("gds_pass")))
    Handler.mqtt = Mqtt()
    today, items = Handler.store.items()
    Handler.mqtt.publish_state(today, items, Handler.store.pantries())
    Handler.mqtt.publish_diag(gs1_china.status(), stt_text())
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    print(f"pantry server {VERSION} on http://{a.bind}:{a.port} data={a.data}"
          f" mqtt={'on ' + Handler.mqtt.host if Handler.mqtt.enabled else 'off'}"
          f" gs1cn={gs1_china.status()}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
