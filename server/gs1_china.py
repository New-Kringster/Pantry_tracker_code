"""GS1 China product lookup (中国商品信息服务平台, gds.org.cn) with a free account.

The portal is a Vue app that logs in through OpenID Connect at
passport.gds.org.cn (IdentityServer, public client "vuejs_code_client",
authorization-code + PKCE) and then calls bff.gds.org.cn with
"Authorization: Bearer <access_token>". Anonymous calls are refused
("已超出您的检索结果浏览范围,请登录后检索").

The login page needs an image captcha every time, and a login from a new IP
can additionally ask for an SMS code sent to the account's phone. So the sign-in
is done once, by a person, from the pantry web page (which relays the captcha
image and the codes through /api/gs1/*); after that the refresh token is kept
in <data>/gs1_token.json and renewed automatically.

Environment: PANTRY_GDS_USER, PANTRY_GDS_PASS. Nothing here prints them.
Standard library only; lookups fail soft (None) so the chain moves on.
"""
import base64
import hashlib
import http.cookiejar
import html
import json
import os
import re
import secrets
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

AUTHORITY = "https://passport.gds.org.cn"
PORTAL = "https://www.gds.org.cn"
CLIENT_ID = "vuejs_code_client"
REDIRECT = PORTAL + "/#/callback"
SCOPE = "openid profile api1 offline_access"
SEARCH_URL = ("https://bff.gds.org.cn/gds/searching-api/ProductService/ProductListByGTIN"
              "?PageSize=5&PageIndex=1&SearchItem={code}")
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36"

_lock = threading.Lock()
_token = {"access": None, "refresh": None, "expires": 0}
_token_file = None
_last_error = None
_creds = lambda: (os.environ.get("PANTRY_GDS_USER"), os.environ.get("PANTRY_GDS_PASS"))  # noqa: E731
_pending = None  # in-progress interactive login: cookies, form fields, pkce
_last_call = 0.0
MIN_INTERVAL = 5.0  # GS1 answers 429 "操作过于频繁" to back-to-back searches


class RateLimited(Exception):
    """GS1 China said 您的操作过于频繁 (HTTP 429): try again later, do not cache a miss."""


def _log(msg):
    print(f"gs1 china: {msg}", file=sys.stderr, flush=True)


def credentials_changed():
    """Settings changed on the web page: forget the session so the next sign-in uses them."""
    global _last_error
    _token.update(access=None, refresh=None, expires=0)
    _store_token({})
    _last_error = None


def configure(data_dir, creds=None):
    """Where to keep the refresh token; loads a saved one. `creds` returns (user, pass)."""
    global _token_file, _creds
    if creds:
        _creds = creds
    _token_file = os.path.join(data_dir, "gs1_token.json")
    try:
        with open(_token_file) as f:
            saved = json.load(f)
        _token.update(access=saved.get("access"), refresh=saved.get("refresh"), expires=saved.get("expires", 0))
        if _token["refresh"]:
            _log("saved session loaded")
    except (OSError, ValueError):
        pass


def configured():
    u, p = _creds()
    return bool(u and p)


def status():
    if not configured():
        return "no account configured"
    if _token["access"] and _token["expires"] > time.time():
        return "signed in"
    if _token["refresh"]:
        return "session saved (will refresh)"
    return _last_error or "not signed in: open the web page and sign in once"


def signed_in():
    return bool(_token["refresh"] or (_token["access"] and _token["expires"] > time.time()))


# --- HTTP helpers --------------------------------------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _opener(jar, follow=True):
    handlers = [urllib.request.HTTPCookieProcessor(jar)]
    if not follow:
        handlers.append(_NoRedirect())
    return urllib.request.build_opener(*handlers)


def _request(opener, url, data=None, headers=None, timeout=25):
    """(status, final_url, body_text, headers). HTTP errors are returned, not raised."""
    h = {"User-Agent": UA, "Accept": "text/html,application/json;q=0.9,*/*;q=0.8"}
    h.update(headers or {})
    req = urllib.request.Request(url, data=data, headers=h)
    try:
        with opener.open(req, timeout=timeout) as r:
            return r.status, r.geturl(), r.read().decode("utf-8", "replace"), r.headers
    except urllib.error.HTTPError as e:
        return e.code, url, e.read().decode("utf-8", "replace"), e.headers


def _form_fields(page):
    """Inputs of the login form: {name: value}."""
    m = re.search(r"<form[^>]*id=\"loginForm\"[^>]*>(.*?)</form>", page, re.S | re.I)
    form = m.group(1) if m else page
    fields = {}
    for inp in re.findall(r"<input[^>]*>", form, re.I):
        name = re.search(r"name=[\"']([^\"']+)", inp, re.I)
        if not name:
            continue
        val = re.search(r"value=[\"']([^\"']*)", inp, re.I)
        fields[name.group(1)] = html.unescape(val.group(1)) if val else ""
    return fields


def _json(body):
    try:
        j = json.loads(body)
        return json.loads(j) if isinstance(j, str) else j  # the page does JSON.parse(res) on a string
    except ValueError:
        return None


# --- Tokens ------------------------------------------------------------------------

def _store_token(tok):
    _token["access"] = tok.get("access_token")
    _token["refresh"] = tok.get("refresh_token") or _token["refresh"]
    _token["expires"] = time.time() + int(tok.get("expires_in", 3600)) - 60
    if _token_file:
        try:
            with open(_token_file, "w") as f:
                json.dump(_token, f)
            os.chmod(_token_file, 0o600)
        except OSError as e:
            _log(f"cannot save token: {e}")


def _token_request(params):
    data = urllib.parse.urlencode(params).encode()
    code, _, body, _ = _request(_opener(http.cookiejar.CookieJar()), AUTHORITY + "/connect/token", data,
                                {"Content-Type": "application/x-www-form-urlencoded", "Accept": "application/json"})
    j = _json(body) or {}
    if code == 200 and j.get("access_token"):
        return j, None
    return None, j.get("error_description") or j.get("error") or f"http {code}"


def _ensure_token():
    """Valid access token or None (refreshes; never starts an interactive login)."""
    global _last_error
    with _lock:
        if _token["access"] and _token["expires"] > time.time():
            return _token["access"]
        if _token["refresh"]:
            tok, err = _token_request({"grant_type": "refresh_token", "client_id": CLIENT_ID,
                                       "refresh_token": _token["refresh"]})
            if tok:
                _store_token(tok)
                return _token["access"]
            _log(f"refresh failed: {err}")
            _last_error = f"session expired ({err}); sign in again on the web page"
            _token["refresh"] = None
            _token["access"] = None
            _store_token({})
        return None


# --- Interactive sign-in (driven from the web page) ------------------------------------

def login_begin():
    """Start a login: returns {captcha: <base64 png>, status} or {error}."""
    global _pending
    if not configured():
        return {"error": "GS1 China username and password are not set (Settings below)"}
    jar = http.cookiejar.CookieJar()
    opener = _opener(jar)
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    q = urllib.parse.urlencode({"client_id": CLIENT_ID, "redirect_uri": REDIRECT, "response_type": "code",
                                "scope": SCOPE, "state": state, "code_challenge": challenge,
                                "code_challenge_method": "S256"})
    code, url, page, _ = _request(opener, AUTHORITY + "/connect/authorize?" + q)
    if code != 200 or "loginForm" not in page:
        return {"error": f"login page not reachable (http {code})"}
    fields = _form_fields(page)
    _pending = {"jar": jar, "fields": fields, "verifier": verifier, "login_url": url, "state": state}
    return _captcha()


def _captcha():
    p = _pending
    code, _, body, _ = _request(_opener(p["jar"]), AUTHORITY + "/Account/Captcha",
                                headers={"Accept": "application/json", "Referer": p["login_url"]})
    j = _json(body) or {}
    if code != 200 or not j.get("Base64"):
        return {"error": f"captcha not available (http {code})"}
    p["fields"]["codekey"] = j.get("Id", "")
    return {"captcha": j["Base64"], "status": status()}


def login_submit(ver_code):
    """Post the login form with the captcha answer. Returns one of
    {ok: true}, {error, captcha?} (wrong captcha: a fresh one is included),
    {sms_required: true, phone: '138****1234'}."""
    p = _pending
    if not p:
        return {"error": "no login in progress"}
    f = dict(p["fields"])
    user, pw = _creds()
    f.update({"Type": "account", "Button": "login", "data": "", "username": user,
              "password": pw, "phone": "", "phoneVer": "", "barCode": "",
              "passwordBar": "", "verCode": ver_code.strip(), "wkpassuser": ""})
    opener = _opener(p["jar"])
    code, _, body, _ = _request(opener, AUTHORITY + "/Account/Login", urllib.parse.urlencode(f).encode(),
                                {"Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
                                 "X-Requested-With": "XMLHttpRequest", "Referer": p["login_url"],
                                 "Accept": "*/*"})
    j = _json(body)
    if j is None:
        return {"error": f"unexpected reply from the login page (http {code})"}
    rc = j.get("Code")
    data = j.get("Data") or {}
    if rc == 1:
        return _finish()
    if rc == 100:  # GS1 wants the user agreement accepted in a browser first
        url = data.get("Url") if isinstance(data, dict) else None
        if url and url.startswith("/"):
            url = AUTHORITY + url
        return {"error": "GS1 wants you to accept their user agreement first. Log in once at https://www.gds.org.cn "
                         "in your browser, accept it, then sign in here again.", "agreement_url": url}
    if rc == 3:
        out = {"error": "wrong captcha, try the new one"}
        out.update(_captcha())
        return out
    if rc == 5:
        phone = data.get("SendPhone") if isinstance(data, dict) else None
        if not phone:
            return {"error": "this account has no verified phone; GS1 wants one for a new location"}
        p["sms_phone"] = phone
        masked = re.sub(r"^(\d{3})\d{4}(\d{4})$", r"\1****\2", phone)
        return {"sms_required": True, "phone": masked}
    if rc == 7:
        return {"error": "GS1 says the password is too weak and must be changed on their site first"}
    if rc == 0 and isinstance(data, dict) and data.get("msg"):
        out = {"error": data["msg"]}
        out.update(_captcha())
        return out
    return {"error": f"login refused (code {rc})"}


def sms_send():
    """Ask GS1 to text the verification code to the account's phone."""
    p = _pending
    if not p or not p.get("sms_phone"):
        return {"error": "no SMS verification pending"}
    code, _, body, _ = _request(_opener(p["jar"]), AUTHORITY + "/Account/SendIPVaildPhone",
                                urllib.parse.urlencode({"phone": p["sms_phone"]}).encode(),
                                {"Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
                                 "X-Requested-With": "XMLHttpRequest", "Referer": p["login_url"]})
    j = _json(body) or {}
    if j.get("Code") == 1:
        return {"ok": True}
    if j.get("Code") == 2:
        return {"error": "GS1 says: too many SMS requests, wait a bit"}
    return {"error": f"SMS not sent (code {j.get('Code')}, http {code})"}


def sms_verify(note):
    """Submit the SMS code, then complete the login."""
    p = _pending
    if not p or not p.get("sms_phone"):
        return {"error": "no SMS verification pending"}
    q = urllib.parse.urlencode({"phone": p["sms_phone"], "note": note.strip(), "UserName": _creds()[0]})
    code, _, body, _ = _request(_opener(p["jar"]), AUTHORITY + "/Account/VaildSendPhoneNote?" + q,
                                headers={"X-Requested-With": "XMLHttpRequest", "Referer": p["login_url"]})
    j = _json(body) or {}
    if j.get("Code") != 1:
        msg = (j.get("Data") or {}).get("msg") if isinstance(j.get("Data"), dict) else None
        return {"error": msg or f"SMS code rejected (code {j.get('Code')}, http {code})"}
    # The page now re-posts the form the classic way; the cookie set by the AJAX
    # login already authenticates us, so go straight to the authorize callback.
    return _finish()


def _finish():
    """Authenticated cookie in hand: finish the OIDC flow and store the tokens."""
    global _pending, _last_error
    p = _pending
    ret = p["fields"].get("ReturnUrl") or ""
    target = urllib.parse.urljoin(AUTHORITY, html.unescape(ret)) if ret else p["login_url"]
    no_follow = _opener(p["jar"], follow=False)
    location = target
    auth_code = None
    for _ in range(6):
        code, _, body, hdrs = _request(no_follow, location)
        loc = hdrs.get("Location") if hdrs else None
        if not loc:
            break
        if "code=" in loc:
            qs = urllib.parse.urlparse(loc.replace("#/callback", "")).query
            auth_code = (urllib.parse.parse_qs(qs).get("code") or [None])[0]
            break
        location = urllib.parse.urljoin(location, loc)
    if not auth_code:
        return {"error": "GS1 accepted the login but did not hand back an authorization code"}
    tok, err = _token_request({"grant_type": "authorization_code", "client_id": CLIENT_ID, "code": auth_code,
                               "redirect_uri": REDIRECT, "code_verifier": p["verifier"]})
    if not tok:
        return {"error": f"token exchange failed: {err}"}
    with _lock:
        _store_token(tok)
    _last_error = None
    _pending = None
    _log("signed in")
    return {"ok": True, "status": status()}


# --- Lookup ----------------------------------------------------------------------------

def _pick(d, *keys):
    for k in keys:
        v = d.get(k)
        if v not in (None, "", [], {}):
            return str(v).strip()
    return None


def lookup(barcode):
    """{name, brand, quantity, image_url} or None."""
    if not configured():
        return None
    token = _ensure_token()
    if not token:
        return None  # anonymous calls are refused; do not even try
    headers = {"Referer": PORTAL + "/", "Origin": PORTAL, "Accept": "application/json",
               "Authorization": "Bearer " + token}
    global _last_call
    for attempt in range(2):
        with _lock:
            wait = MIN_INTERVAL - (time.time() - _last_call)
            if wait > 0:
                time.sleep(wait)
            _last_call = time.time()
        code, _, body, _ = _request(_opener(http.cookiejar.CookieJar()),
                                    SEARCH_URL.format(code=urllib.parse.quote(barcode)), headers=headers)
        j = _json(body)
        limited = code == 429 or (isinstance(j, dict) and str(j.get("Code")) == "2" and "频繁" in str(j.get("Msg", "")))
        if not limited:
            break
        _log(f"{barcode}: rate limited" + (", retrying" if attempt == 0 else ""))
    else:
        raise RateLimited()
    if not isinstance(j, dict):
        _log(f"{barcode}: http {code}, not json")
        return None
    if str(j.get("Code")) not in ("0", "200", "1") or not j.get("Data"):
        _log(f"{barcode}: {j.get('Msg') or j.get('Code')}")
        if "登录" in str(j.get("Msg", "")):
            _token["access"] = None  # token rejected; refresh next time
        return None
    data = j["Data"]
    rows = (data.get("Items") or data.get("items") or data.get("list") or data.get("List")
            or (data if isinstance(data, list) else []))
    if not rows:
        return None
    # Prefer a record whose GTIN matches exactly (the search is a prefix search).
    want = barcode.lstrip("0")
    p = next((r for r in rows if str(r.get("gtin", "")).lstrip("0") == want), rows[0])
    # Observed fields (2026-11): keyword = product name, brandcn = brand, firm_name =
    # manufacturer, specification = net content, picture_filename, description (often
    # empty), RegulatedProductName, gpcname (category).
    rec = {"name": _pick(p, "keyword", "description", "RegulatedProductName", "productName", "name"),
           "brand": _pick(p, "brandcn", "brand", "brandName") or _pick(p, "firm_name"),
           "quantity": _pick(p, "specification", "spec", "netContent"),
           "image_url": _pick(p, "picture_filename", "pictureUrl", "picture", "image"),
           "manufacturer": _pick(p, "firm_name"), "category": _pick(p, "gpcname")}
    if rec["image_url"] and not rec["image_url"].startswith("http"):
        rec["image_url"] = "https://oss.gds.org.cn/" + rec["image_url"].lstrip("/")
    if not rec["name"]:
        _log(f"{barcode}: unrecognised row keys {sorted(p)[:25]}")
        return None
    return rec


if __name__ == "__main__":  # python3 gs1_china.py <barcode>  (env from /etc/pantry.env)
    configure(sys.argv[2] if len(sys.argv) > 2 else "/var/lib/pantry")
    print(json.dumps(lookup(sys.argv[1]) if len(sys.argv) > 1 else {"status": status()}, ensure_ascii=False))
    print("status:", status(), file=sys.stderr)
