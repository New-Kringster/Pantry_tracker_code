// Pantry tracker: M5Stack CoreS3 + Module13.2 QRCode (M145).
//
// Scan a product barcode, the server looks it up (Open Food Facts, GS1 China),
// pick an expiry with the Days / Weeks / Months pickers, and the HOME list shows
// everything sorted by what expires first, per pantry. The server feeds Home
// Assistant over MQTT.
//
// Screens (320x240, touch), styled after Braven's Figma frames:
//   HOME   : "Pantry v" pill (pantry picker), "+" pill, status pill (HA, WiFi, battery),
//            cards with thumbnail, name, "1wk ago", big "3 Days"; SCAN NOW pill at the bottom.
//   SCAN   : black with a red glow, "Scanning...", pantry pill to pick the target pantry.
//   ENTRY  : product card with Days / Weeks / Months pickers; SAVE pill; x pill cancels.
//   DETAIL : the same card for a tracked item, DELETE pill.
// Screen changes slide; the pantry dropdown unfolds; the glow breathes.
//
// Module13.2 QRCode: scanner engine on UART (115200 8N1; on this unit the DIP
// switches land on CoreS3 G13 TX / G18 RX, saved with `qrpins`) plus a
// PI4IOE5V6408 IO expander at I2C 0x43 that gates the engine's power (P0) and
// trigger (P4). The engine is powered only on the SCAN page. Never send it a byte
// in its first 1.5 s after power-up (that coincided with a factory reset).
//
// Serial console (115200, USB CDC): type `help`.
#include <Arduino.h>
#include <M5Unified.h>
#include "driver/uart.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <M5ModuleQRCode.h>
#include <map>
#include <set>
#include <vector>
#include <math.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#if __has_include("secrets.h") && !defined(PANTRY_NO_SECRETS)
#include "secrets.h"  // optional build-time defaults (dev only); release images build with -DPANTRY_NO_SECRETS
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef SERVER_URL
#define SERVER_URL ""
#endif
#ifndef FIRMWARE_REV
#define FIRMWARE_REV "dev"
#endif
const char* kFirmwareRev = FIRMWARE_REV;

// --- Settings (flash) ----------------------------------------------------

struct Settings {
  String ssid = WIFI_SSID, pass = WIFI_PASS, server = SERVER_URL;
  int qrTx = 13, qrRx = 18;  // first guess; qrInit() finds the real pair if the engine stays silent
  int pantryId = 1;
  bool beep = false;         // scanner decode beep (control centre)
  int bright = 160;          // screen brightness 0-255 (control centre)
  void load() {
    Preferences p;
    p.begin("pantry", true);
    ssid = p.getString("ssid", ssid);
    pass = p.getString("pass", pass);
    server = p.getString("server", server);
    qrTx = p.getInt("qrtx", qrTx);
    qrRx = p.getInt("qrrx", qrRx);
    pantryId = p.getInt("pantry", pantryId);
    beep = p.getBool("beep", beep);
    bright = p.getInt("bright", bright);
    p.end();
  }
  void save() {
    Preferences p;
    p.begin("pantry", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.putString("server", server);
    p.putInt("qrtx", qrTx);
    p.putInt("qrrx", qrRx);
    p.putInt("pantry", pantryId);
    p.putBool("beep", beep);
    p.putInt("bright", bright);
    p.end();
  }
} settings;

// --- Dates (days since 1970-01-01; Howard Hinnant's civil algorithms) ------

long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
void civilFromDays(long z, int& y, int& m, int& d) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
}
bool parseDate(const String& s, long& days) {  // "YYYY-MM-DD"
  if (s.length() != 10 || s[4] != '-' || s[7] != '-') return false;
  days = daysFromCivil(s.substring(0, 4).toInt(), s.substring(5, 7).toInt(), s.substring(8, 10).toInt());
  return true;
}
String fmtDate(long days) {
  int y, m, d;
  civilFromDays(days, y, m, d);
  char b[12];
  snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
  return b;
}
const char* weekday(long days) {
  static const char* const n[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  return n[((days % 7) + 7) % 7];
}
long addMonths(long days, int months) {
  int y, m, d;
  civilFromDays(days, y, m, d);
  m += months;
  y += (m - 1) / 12;
  m = (m - 1) % 12 + 1;
  if (d > 28) d = 28;  // keep it simple: never overflow into the next month
  return daysFromCivil(y, m, d);
}

// --- Data from the server ------------------------------------------------

struct Item {
  int id = 0;
  String barcode, name, brand, qty, expires, pantry;
  int daysLeft = 0, daysAgo = 0, pantryId = 1;
  bool hasImage = false;
};
struct Product {
  String barcode, name, brand, qty, source;
  bool found = false, hasImage = false;
};
struct Pantry {
  int id = 1, items = 0, expired = 0, soon = 0;
  String name;
};
std::vector<Item> items;
std::vector<Pantry> pantries;
long today = 0;  // days since epoch, from the server
bool serverOk = false, mqttOk = false;
String lastError;
uint32_t lastFetchMs = 0;

constexpr int kThumbList = 50, kThumbBig = 56;
std::map<String, std::vector<uint8_t>> thumbs;  // "<barcode>@<size>" -> jpeg
std::set<String> thumbMissing;
String thumbKey(const String& b, int s) { return b + "@" + String(s); }

// --- HTTP ----------------------------------------------------------------

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

int httpRequest(const char* method, const String& path, const String& body, String* out,
                std::vector<uint8_t>* outBytes = nullptr, uint32_t timeoutMs = 6000) {
  if (!wifiUp()) return -1;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, settings.server + path)) return -2;
  int code;
  if (body.length()) {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, body);
  } else {
    code = http.sendRequest(method);
  }
  if (code > 0) {
    if (outBytes) {
      int len = http.getSize();
      outBytes->clear();
      if (len > 0 && len < 200000) {
        outBytes->resize(len);
        WiFiClient* s = http.getStreamPtr();
        size_t got = 0;
        uint32_t t0 = millis();
        while (got < (size_t)len && millis() - t0 < 6000) {
          int n = s->read(outBytes->data() + got, len - got);
          if (n > 0) got += n; else delay(2);
        }
        if (got != (size_t)len) outBytes->clear();
      }
    } else if (out) {
      *out = http.getString();
    }
  }
  http.end();
  serverOk = code > 0;
  if (code <= 0) lastError = String("server: ") + HTTPClient::errorToString(code);
  return code;
}

String str(JsonVariantConst v) { return v.isNull() ? String() : v.as<String>(); }

bool fetchPantries() {
  String body;
  if (httpRequest("GET", "/api/pantries", "", &body) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  pantries.clear();
  for (JsonObject o : doc["pantries"].as<JsonArray>()) {
    Pantry p;
    p.id = o["id"] | 1; p.name = str(o["name"]); p.items = o["items"] | 0;
    p.expired = o["expired"] | 0; p.soon = o["expiring_soon"] | 0;
    pantries.push_back(p);
  }
  // The selected pantry may have been deleted on the web page.
  bool have = false;
  for (auto& p : pantries) if (p.id == settings.pantryId) have = true;
  if (!have && !pantries.empty()) { settings.pantryId = pantries[0].id; settings.save(); }
  return true;
}

bool fetchItems() {
  String body;
  int code = httpRequest("GET", "/api/items?pantry=" + String(settings.pantryId), "", &body);
  lastFetchMs = millis();
  if (code == 404) {  // pantry gone: fall back to whatever exists
    if (fetchPantries()) code = httpRequest("GET", "/api/items?pantry=" + String(settings.pantryId), "", &body);
  }
  if (code != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) { lastError = "bad items json"; return false; }
  parseDate(str(doc["today"]), today);
  mqttOk = doc["mqtt_connected"] | false;
  items.clear();
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    Item it;
    it.id = o["id"] | 0;
    it.barcode = str(o["barcode"]);
    it.name = str(o["name"]);
    it.brand = str(o["brand"]);
    it.qty = str(o["quantity"]);
    it.expires = str(o["expires"]);
    it.pantry = str(o["pantry"]);
    it.pantryId = o["pantry_id"] | 1;
    it.daysLeft = o["days_left"] | 0;
    it.daysAgo = o["days_ago"] | 0;
    it.hasImage = o["has_image"] | false;
    items.push_back(it);
  }
  return true;
}

bool fetchProduct(const String& barcode, Product& p) {
  String body;
  p = Product();
  p.barcode = barcode;
  // Lookups can take a while: Open Food Facts, then GS1 China (paced to its rate limit).
  int code = httpRequest("GET", "/api/products/" + barcode, "", &body, nullptr, 20000);
  if (code != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  p.found = doc["found"] | 0;
  p.name = str(doc["name"]);
  p.brand = str(doc["brand"]);
  p.qty = str(doc["quantity"]);
  p.source = str(doc["source"]);
  p.hasImage = doc["has_image"] | false;
  return true;
}

// Fetch one missing thumbnail. Returns true when it did network work.
bool ensureThumb(const String& barcode, int size) {
  const String key = thumbKey(barcode, size);
  if (thumbs.count(key) || thumbMissing.count(key)) return false;
  std::vector<uint8_t> bytes;
  int code = httpRequest("GET", "/api/products/" + barcode + "/thumb.jpg?s=" + size, "", nullptr, &bytes);
  if (code == 200 && !bytes.empty()) {
    if (thumbs.size() > 60) thumbs.clear();  // crude cap; the server re-serves them
    thumbs[key] = std::move(bytes);
  } else {
    thumbMissing.insert(key);
  }
  return true;
}

int httpPostBytes(const String& path, const uint8_t* data, size_t len, const char* contentType, String* out) {
  if (!wifiUp()) return -1;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(45000);  // transcription takes a while on the VM
  if (!http.begin(client, settings.server + path)) return -2;
  http.addHeader("Content-Type", contentType);
  int code = http.POST(const_cast<uint8_t*>(data), len);
  if (code > 0 && out) *out = http.getString();
  http.end();
  serverOk = code > 0;
  if (code <= 0) lastError = String("server: ") + HTTPClient::errorToString(code);
  return code;
}

bool renameItem(int id, const String& name) {
  JsonDocument doc;
  doc["name"] = name;
  String body, resp;
  serializeJson(doc, body);
  return httpRequest("PATCH", "/api/items/" + String(id), body, &resp) == 200;
}

bool postItem(const String& barcode, long expiresDays, int pantryId, const String& name, String& err) {
  JsonDocument doc;
  doc["barcode"] = barcode;
  doc["expires"] = fmtDate(expiresDays);
  doc["pantry_id"] = pantryId;
  if (name.length()) doc["name"] = name;
  String body, resp;
  serializeJson(doc, body);
  int code = httpRequest("POST", "/api/items", body, &resp);
  if (code == 201) return true;
  err = code > 0 ? "HTTP " + String(code) : lastError;
  return false;
}

bool deleteItem(int id) {
  String resp;
  return httpRequest("DELETE", "/api/items/" + String(id), "", &resp) == 204;
}

// Telemetry for Home Assistant (battery, WiFi, last scan) via the server.
extern const char* kFirmwareRev;
void postDevice(const char* event = nullptr, const String& barcode = "") {
  if (!wifiUp()) return;
  JsonDocument doc;
  doc["battery"] = M5.Power.getBatteryLevel();
  doc["charging"] = M5.Power.isCharging() == m5::Power_Class::is_charging;
  doc["rssi"] = WiFi.RSSI();
  doc["ip"] = WiFi.localIP().toString();
  doc["firmware"] = kFirmwareRev;
  doc["uptime_s"] = millis() / 1000;
  if (event) { doc["event"] = event; doc["barcode"] = barcode; }
  String body, resp;
  serializeJson(doc, body);
  httpRequest("POST", "/api/device", body, &resp);
}

int createPantry() {  // server names it "Pantry N"; returns the new id or 0
  String resp;
  if (httpRequest("POST", "/api/pantries", "{}", &resp) != 201) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return 0;
  return doc["id"] | 0;
}

// --- Scanner module ------------------------------------------------------

M5ModuleQRCode qr;
bool qrPresent = false;
bool qrDecoding = false;   // engine powered for scanning
bool qrArmed = false;      // decode started (after the boot delay)
uint32_t qrPowerOnMs = 0;
String qrVersion;
constexpr uint32_t kEngineBootMs = 1500;  // never talk to the engine earlier

bool qrLinkUp() { return qrPresent && qrVersion.length() > 0; }

// Configuration write only when the stored value differs (23 = read, 21 = write;
// every write goes to the engine's flash, and bursts of writes while it was busy
// decoding coincided with it losing all settings). Returns true when it matches.
bool qrEnsure(uint8_t pid, uint8_t fid, uint8_t value) {
  while (Serial1.available()) Serial1.read();
  const uint8_t rd[] = {0x23, pid, fid};
  Serial1.write(rd, sizeof rd); Serial1.flush();
  uint32_t t0 = millis(); std::vector<uint8_t> buf;
  while (millis() - t0 < 300) { while (Serial1.available()) { buf.push_back(Serial1.read()); t0 = millis() - 220; } delay(5); }
  for (size_t i = 0; i + 4 <= buf.size(); i++)
    if (buf[i] == 0x24 && buf[i + 1] == pid && buf[i + 2] == fid) {
      if (buf[i + 3] == value) return true;
      break;
    }
  const uint8_t wr[] = {0x21, pid, fid, value};
  Serial1.write(wr, sizeof wr); Serial1.flush();
  delay(400);  // let it store before the next command
  while (Serial1.available()) Serial1.read();
  Serial.printf("scanner: set %02X %02X = %02X\r\n", pid, fid, value);
  return true;
}

// Same for a 2-byte value (e.g. 64 82 same-code interval in ms).
bool qrEnsure16(uint8_t pid, uint8_t fid, uint16_t value) {
  while (Serial1.available()) Serial1.read();
  const uint8_t rd[] = {0x23, pid, fid};
  Serial1.write(rd, sizeof rd); Serial1.flush();
  uint32_t t0 = millis(); std::vector<uint8_t> buf;
  while (millis() - t0 < 300) { while (Serial1.available()) { buf.push_back(Serial1.read()); t0 = millis() - 220; } delay(5); }
  for (size_t i = 0; i + 5 <= buf.size(); i++)
    if (buf[i] == 0x24 && buf[i + 1] == pid && buf[i + 2] == fid) {
      if ((((uint16_t)buf[i + 3] << 8) | buf[i + 4]) == value) return true;
      break;
    }
  const uint8_t wr[] = {0x21, pid, fid, (uint8_t)(value >> 8), (uint8_t)value};
  Serial1.write(wr, sizeof wr); Serial1.flush();
  delay(400);
  while (Serial1.available()) Serial1.read();
  Serial.printf("scanner: set %02X %02X = %u\r\n", pid, fid, value);
  return true;
}
// Suffix CR LF after every decode so bursts of reads can be split (51 4C enable, 51 C2 content).
void qrEnsureSuffix() {
  while (Serial1.available()) Serial1.read();
  const uint8_t rd[] = {0x23, 0x51, 0x4C};
  Serial1.write(rd, sizeof rd); Serial1.flush();
  uint32_t t0 = millis(); std::vector<uint8_t> buf;
  while (millis() - t0 < 300) { while (Serial1.available()) { buf.push_back(Serial1.read()); t0 = millis() - 220; } delay(5); }
  for (size_t i = 0; i + 4 <= buf.size(); i++)
    if (buf[i] == 0x24 && buf[i + 1] == 0x51 && buf[i + 2] == 0x4C && buf[i + 3] == 0x01) return;  // already on
  const uint8_t content[] = {0x21, 0x51, 0xC2, 0x00, 0x02, 0x0D, 0x0A};
  Serial1.write(content, sizeof content); Serial1.flush(); delay(400);
  const uint8_t enable[] = {0x21, 0x51, 0x4C, 0x01};
  Serial1.write(enable, sizeof enable); Serial1.flush(); delay(400);
  while (Serial1.available()) Serial1.read();
  Serial.print("scanner: suffix CR LF enabled\r\n");
}

// Status query done by hand: collect everything for 400 ms and find the reply
// header (44 02 <id> <len hi> <len lo> <data>) even when decode output of an
// engine left in auto mode is mixed into the stream.
String qrQueryInfo(uint8_t id) {
  while (Serial1.available()) Serial1.read();
  const uint8_t cmd[] = {0x43, 0x02, id};
  Serial1.write(cmd, sizeof cmd);
  Serial1.flush();
  std::vector<uint8_t> buf;
  uint32_t t0 = millis();
  while (millis() - t0 < 400) {
    while (Serial1.available()) { buf.push_back(Serial1.read()); t0 = millis() - 320; }
    delay(5);
  }
  for (size_t i = 0; i + 5 <= buf.size(); i++) {
    if (buf[i] == 0x44 && buf[i + 1] == 0x02 && buf[i + 2] == id) {
      size_t len = ((size_t)buf[i + 3] << 8) | buf[i + 4];
      if (len == 0 || len > 64 || i + 5 + len > buf.size()) continue;
      String out;
      for (size_t k = 0; k < len; k++) { char c = buf[i + 5 + k]; if (c >= 0x20 && c < 0x7F) out += c; }
      return out;
    }
  }
  return "";
}

// The module's TX and RX DIP blocks each route the engine's UART to one of four bus
// pins. The PIN MAP printed on the board lists its rows in the switches' physical
// order (switch 4 at the top), which is easy to misread, so when the engine stays
// silent on the saved pins, try every pair the blocks can select and keep the one
// that answers. Worst case about 6 s, and only while the engine is not answering.
bool qrAutoDetectPins() {
  static const int kTx[] = {13, 17, 43, 6};  // TX block: switch 2, 3, 4, 1 on a CoreS3
  static const int kRx[] = {7, 18, 44, 10};  // RX block: switch 2, 3, 4, 1
  for (int tx : kTx) for (int rx : kRx) {
    if (tx == settings.qrTx && rx == settings.qrRx) continue;  // already tried
    Serial1.end();
    Serial1.begin(115200, SERIAL_8N1, rx, tx);
    delay(30);
    String v = qrQueryInfo(0xC1);
    if (v.length()) {
      Serial.printf("scanner: answers on tx %d rx %d (saved pins were %d / %d); saved\r\n", tx, rx, settings.qrTx, settings.qrRx);
      settings.qrTx = tx; settings.qrRx = rx; settings.save();
      qrVersion = v;
      return true;
    }
  }
  Serial1.end();
  Serial1.begin(115200, SERIAL_8N1, settings.qrRx, settings.qrTx);
  Serial.print("scanner: no answer on any DIP pin pair (engine still in USB mode?)\r\n");
  return false;
}

bool qrInit() {
  auto cfg = qr.getConfig();
  cfg.pin_tx = settings.qrTx;
  cfg.pin_rx = settings.qrRx;
  cfg.serial = &Serial1;
  cfg.i2c = &M5.In_I2C;
  qr.setConfig(cfg);
  Serial1.end();
  qrPresent = qr.begin();  // finds the IO expander, powers the engine, opens the UART
  if (!qrPresent) return false;
  delay(kEngineBootMs);
  qrVersion = "";
  for (int attempt = 0; attempt < 3 && qrVersion.isEmpty(); attempt++) {
    if (attempt == 2) { qr.setEnable(false); delay(300); qr.setEnable(true); delay(kEngineBootMs); }
    qrVersion = qrQueryInfo(0xC1);
  }
  if (qrVersion.isEmpty()) qrAutoDetectPins();
  Serial.printf("scanner engine fw '%s'\r\n", qrVersion.c_str());
  if (qrVersion.length()) {
    // Only touch settings that differ. Key-trigger mode when idle (auto mode would
    // decode at power-up and spam the link), silent, lights on decode.
    qrEnsure(0x61, 0x41, 0x00);  // trigger mode: button/key
    qrEnsure(0x63, 0x45, 0x00);  // startup tone off
    qrEnsure(0x63, 0x42, settings.beep ? 0x01 : 0x00);  // decode beep (control centre)
    qrEnsure(0x62, 0x41, 0x02);  // fill light: on while decoding
    qrEnsure(0x62, 0x42, 0x01);  // aiming light: flash while decoding
    qrEnsure16(0x64, 0x82, 2500); // same code again only after 2.5 s
    qrEnsure(0x64, 0x43, 0x00);   // ...and no "same code, no delay"
    qrEnsureSuffix();
  }
  qr.setEnable(false);  // off until the SCAN page
  return true;
}
// Power the engine; qrService() arms it once it has booted (keeps the UI animating).
void qrStart() {
  if (!qrPresent) return;
  qr.setEnable(true);
  qrPowerOnMs = millis();
  qrDecoding = true;
  qrArmed = false;
}
void qrService() {
  if (!qrDecoding || qrArmed || millis() - qrPowerOnMs < kEngineBootMs) return;
  while (Serial1.available()) Serial1.read();
  if (qrLinkUp()) {
    // Auto mode: the engine decodes for as long as it is powered, and it is only
    // powered on this page. No start command needed (the engine never acked one).
    qrEnsure(0x63, 0x42, settings.beep ? 0x01 : 0x00);  // beep setting may have changed
    qrEnsure(0x61, 0x41, 0x02);  // auto: decodes while powered
    Serial.print("scanner: auto decode on\r\n");
  } else {
    // UART link down (engine in USB mode?): hold TRIG low so it still decodes,
    // which is how its own setting barcodes get scanned.
    qr.setTriggerLevel(false);
    Serial.print("scanner uart link down: holding TRIG low for this scan\r\n");
  }
  qrArmed = true;
}
void qrStop() {
  if (!qrPresent) return;
  if (qrArmed) {
    if (qrLinkUp()) qrEnsure(0x61, 0x41, 0x00);  // idle at next power-up
    else qr.setTriggerLevel(true);
  }
  qr.setEnable(false);
  qrDecoding = qrArmed = false;
}
// A decoded code arrives as plain text (protocol format 0). Read a whole burst,
// then keep printable characters only.
bool qrPoll(String& code) {
  if (!qrArmed || !Serial1.available()) return false;
  // Read the whole burst (until 60 ms of quiet), then keep the first complete code.
  String text, raw;
  uint32_t last = millis();
  while (millis() - last < 60) {
    while (Serial1.available()) {
      char c = Serial1.read();
      char h[4]; snprintf(h, sizeof h, "%02X ", (uint8_t)c); raw += h;
      text += (c == '\r' || c == '\n') ? '\n' : (c >= 0x20 && c < 0x7F) ? c : ' ';
      last = millis();
    }
    delay(2);
  }
  Serial.printf("qr rx: %s\r\n", raw.c_str());
  code = "";
  int start = 0;
  while (start < (int)text.length()) {  // first non-empty line
    int nl = text.indexOf('\n', start);
    String tok = text.substring(start, nl < 0 ? text.length() : nl);
    tok.trim();
    if (tok.length()) { code = tok; break; }
    if (nl < 0) break;
    start = nl + 1;
  }
  // No suffix yet (engine not configured): several reads of one code arrive glued
  // together, e.g. 13 digits repeated. Keep one period.
  for (int len : {13, 8, 12, 14}) {
    if ((int)code.length() > len && code.length() % len == 0) {
      String first = code.substring(0, len); bool rep = true;
      for (int i = len; i < (int)code.length() && rep; i += len) rep = code.substring(i, i + len) == first;
      if (rep) { code = first; break; }
    }
  }
  return code.length() > 0;
}

// Console diagnostics (bring-up tools; see Plan.md "Scanner").
void qrProbe(long baud) {
  if (!qrPresent) { Serial.print("qrcode module: absent (IO expander 0x43 not found)\r\n"); return; }
  if (baud <= 0) baud = 115200;
  const int cand[] = {17, 18, 13, 7, 6, 14, 43, 44};  // every selectable bus GPIO
  qr.setEnable(true);
  delay(kEngineBootMs);
  Serial.printf("probing at %ld baud\r\n", baud);
  for (int tx : cand) for (int rx : cand) {
    if (tx == rx) continue;
    Serial1.end();
    Serial1.begin(baud, SERIAL_8N1, rx, tx);
    delay(50);
    while (Serial1.available()) Serial1.read();
    const uint8_t cmd[] = {0x43, 0x02, 0xC1};
    Serial1.write(cmd, sizeof cmd);
    uint32_t t0 = millis(); String hex;
    while (millis() - t0 < 400) {
      while (Serial1.available()) { char h[4]; snprintf(h, sizeof h, "%02X ", Serial1.read()); hex += h; t0 = millis() - 300; }
      delay(5);
    }
    if (hex.length()) Serial.printf("probe tx %2d rx %2d: %s\r\n", tx, rx, hex.c_str());
  }
  Serial.print("probe done\r\n");
  Serial1.end();
  Serial1.begin(115200, SERIAL_8N1, settings.qrRx, settings.qrTx);
  qr.setEnable(false);
}
void qrLevels() {
  if (!qrPresent) { Serial.print("qrcode module: absent\r\n"); return; }
  Serial1.end();
  qr.setEnable(true); delay(kEngineBootMs);
  const int pins[] = {17, 18, 6, 7, 13, 14, 43, 44};
  for (int p : pins) {
    pinMode(p, INPUT_PULLDOWN); delay(5); int lo = digitalRead(p);
    pinMode(p, INPUT_PULLUP); delay(5); int hi = digitalRead(p);
    pinMode(p, INPUT);
    Serial.printf("gpio %2d: %s\r\n", p, (lo && hi) ? "driven HIGH" : (!lo && !hi) ? "driven LOW" : "floating");
  }
  Serial1.begin(115200, SERIAL_8N1, settings.qrRx, settings.qrTx);
  uart_set_loop_back(UART_NUM_1, true);
  while (Serial1.available()) Serial1.read();
  Serial1.print("ping"); Serial1.flush(); delay(20);
  String got; while (Serial1.available()) got += (char)Serial1.read();
  uart_set_loop_back(UART_NUM_1, false);
  Serial.printf("uart1 internal loopback: %s\r\n", got == "ping" ? "ok" : "FAILED");
  qr.setEnable(false);
}
void qrTrig(int ms) {
  if (!qrPresent) { Serial.print("qrcode module: absent\r\n"); return; }
  if (ms <= 0) ms = 8000;
  qr.setEnable(true); delay(kEngineBootMs);
  while (Serial1.available()) Serial1.read();
  Serial.printf("TRIG low for %d ms, present the barcode now\r\n", ms);
  qr.setTriggerLevel(false);
  uint32_t end = millis() + ms; String hex;
  while (millis() < end) {
    while (Serial1.available()) { char h[4]; snprintf(h, sizeof h, "%02X ", Serial1.read()); hex += h; }
    delay(5);
  }
  qr.setTriggerLevel(true);
  Serial.printf("TRIG released; uart rx: %s\r\n", hex.length() ? hex.c_str() : "-");
  delay(300);
  qr.setEnable(false);
}
void qrCmd(const String& hexArg) {  // qrcmd 23 42 40  -> raw protocol bytes, prints the reply
  if (!qrPresent) { Serial.print("qrcode module: absent\r\n"); return; }
  uint8_t buf[64]; size_t n = 0; String tok;
  for (size_t i = 0; i <= hexArg.length() && n < sizeof buf; i++) {
    char c = i < hexArg.length() ? hexArg[i] : ' ';
    if (isxdigit((unsigned char)c)) tok += c;
    else if (tok.length()) { buf[n++] = (uint8_t)strtoul(tok.c_str(), nullptr, 16); tok = ""; }
  }
  if (!n) { Serial.print("usage: qrcmd 43 02 C1\r\n"); return; }
  bool wasOn = qrDecoding;
  if (!wasOn) { qr.setEnable(true); delay(kEngineBootMs); }
  while (Serial1.available()) Serial1.read();
  Serial1.write(buf, n); Serial1.flush();
  uint32_t t0 = millis(); String hex, txt;
  while (millis() - t0 < 600) {
    while (Serial1.available()) { uint8_t b = Serial1.read(); char h[4]; snprintf(h, sizeof h, "%02X ", b); hex += h; txt += (b >= 0x20 && b < 0x7F) ? (char)b : '.'; t0 = millis() - 450; }
    delay(5);
  }
  Serial.printf("qrcmd -> %s %s\r\n", hex.length() ? hex.c_str() : "(no reply)", txt.c_str());
  if (!wasOn) qr.setEnable(false);
}
void qrInfo() {
  if (!qrPresent) { Serial.print("qrcode module: absent\r\n"); return; }
  bool wasOn = qrDecoding;
  if (!wasOn) { qr.setEnable(true); delay(kEngineBootMs); }
  String fw = qrQueryInfo(0xC1), sw = qrQueryInfo(0xC2);
  if (fw.length()) qrVersion = fw;
  Serial.printf("qrcode fw '%s' sw '%s' pins tx %d rx %d\r\n", fw.c_str(), sw.c_str(), settings.qrTx, settings.qrRx);
  if (!wasOn) qr.setEnable(false);
}

// --- Provisioning: hotspot + web page --------------------------------------
// Without WiFi credentials (or when the network cannot be joined) the device
// opens an open hotspot "Pantry-XXXX" with a captive page to enter WiFi and the
// server URL. The same page is served on the LAN afterwards (http://<ip>/ and
// http://pantry-tracker.local/) for every device setting.

enum class Screen { Home, Scan, Entry, Detail, Setup };
extern Screen screen;
extern bool dirty;
WebServer web(80);
DNSServer dns;
bool apMode = false;
String apName;
uint32_t apSinceMs = 0, staTryMs = 0, reconnectAt = 0;
void render(bool transition, int dir);
void goHome(bool refresh, bool animate);

const char kSetupPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pantry tracker setup</title><style>body{font-family:-apple-system,system-ui,sans-serif;background:#000;color:#eee;margin:0;padding:18px;max-width:520px;margin-inline:auto}
h1{font-size:1.4rem}label{display:block;margin:12px 0 4px;opacity:.75}input,select,button{font:inherit;width:100%;box-sizing:border-box;padding:10px;border-radius:10px;border:1px solid #333;background:#1c1c1e;color:#eee}
button{background:#30d158;color:#000;font-weight:700;margin-top:16px}.row{display:flex;gap:8px}.row>*{flex:1}.st{background:#1c1c1e;border-radius:12px;padding:10px 12px;font-size:.9rem;line-height:1.6}small{opacity:.6}</style></head><body>
<h1>Pantry tracker</h1><div class="st" id="st">loading…</div>
<form method="post" action="/save"><label>WiFi network</label><input name="ssid" id="ssid" list="nets" placeholder="SSID" required><datalist id="nets"></datalist>
<label>WiFi password</label><input name="pass" type="password" placeholder="leave blank to keep">
<label>Pantry server URL</label><input name="server" id="server" placeholder="http://192.168.1.10:8080">
<div class="row"><div><label>Scanner TX pin</label><input name="qrtx" id="qrtx"></div><div><label>Scanner RX pin</label><input name="qrrx" id="qrrx"></div></div>
<div class="row"><div><label>Brightness (0-255)</label><input name="bright" id="bright"></div><div><label>Beep</label><select name="beep" id="beep"><option value="0">off</option><option value="1">on</option></select></div></div>
<button>Save</button></form><p><small>After saving, the device joins the WiFi and turns its hotspot off. Scanner pins are found automatically from the module's DIP switches.</small></p>
<script>fetch('/api/status').then(r=>r.json()).then(j=>{document.getElementById('st').innerHTML=`WiFi: <b>${j.ssid||'(none)'}</b> ${j.wifi?'connected, '+j.ip:'not connected'}<br>Server: ${j.server||'(none)'} ${j.server_ok?'✓':'✗'}<br>Scanner: ${j.qr_present?('fw '+(j.qr_fw||'?')):'not found'} · Battery ${j.battery}%`;
for(const k of ['ssid','server','qrtx','qrrx','bright'])document.getElementById(k).value=j[k]??'';document.getElementById('beep').value=j.beep?1:0;});
fetch('/scan').then(r=>r.json()).then(l=>{document.getElementById('nets').innerHTML=l.map(n=>`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`).join('')});</script></body></html>)HTML";

void webStatus() {
  JsonDocument d;
  d["ssid"] = settings.ssid; d["wifi"] = wifiUp(); d["ip"] = WiFi.localIP().toString();
  d["server"] = settings.server; d["server_ok"] = serverOk; d["qr_present"] = qrPresent; d["qr_fw"] = qrVersion;
  d["qrtx"] = settings.qrTx; d["qrrx"] = settings.qrRx; d["bright"] = settings.bright; d["beep"] = settings.beep;
  d["battery"] = M5.Power.getBatteryLevel(); d["pantry"] = settings.pantryId; d["firmware"] = kFirmwareRev;
  d["ap"] = apMode; d["ap_name"] = apName;
  String out; serializeJson(d, out);
  web.send(200, "application/json", out);
}
void webScan() {
  int n = WiFi.scanNetworks(false, false, false, 200);
  JsonDocument d; JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) { JsonObject o = arr.add<JsonObject>(); o["ssid"] = WiFi.SSID(i); o["rssi"] = WiFi.RSSI(i); }
  WiFi.scanDelete();
  String out; serializeJson(d, out);
  web.send(200, "application/json", out);
}
void webSave() {
  bool wifiChanged = false;
  if (web.hasArg("ssid") && web.arg("ssid").length()) {
    String ssid = web.arg("ssid"), pass = web.arg("pass");
    if (ssid != settings.ssid || pass.length()) { settings.ssid = ssid; if (pass.length() || ssid != settings.ssid) settings.pass = pass; wifiChanged = true; }
  }
  if (web.hasArg("server")) { String u = web.arg("server"); u.trim(); while (u.endsWith("/")) u.remove(u.length() - 1); settings.server = u; }
  if (web.hasArg("qrtx") && web.arg("qrtx").toInt() > 0) settings.qrTx = web.arg("qrtx").toInt();
  if (web.hasArg("qrrx") && web.arg("qrrx").toInt() > 0) settings.qrRx = web.arg("qrrx").toInt();
  if (web.hasArg("bright")) { settings.bright = constrain(web.arg("bright").toInt(), 10, 255); M5.Display.setBrightness(settings.bright); }
  if (web.hasArg("beep")) settings.beep = web.arg("beep") == "1";
  settings.save();
  web.send(200, "text/html; charset=utf-8",
           "<meta charset='utf-8'><body style='font-family:system-ui;background:#000;color:#eee;padding:24px'><h2>Saved</h2>"
           "<p>The device is joining the WiFi now. If this page was opened through the hotspot it will disappear; "
           "find the device afterwards at <b>http://pantry-tracker.local/</b> or its IP.</p></body>");
  if (wifiChanged) reconnectAt = millis() + 800;
  thumbs.clear(); thumbMissing.clear();
  dirty = true;
}
void webNotFound() {
  if (apMode) { web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true); web.send(302, "text/plain", ""); }
  else web.send(404, "text/plain", "not found");
}
void webBegin() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", kSetupPage); });
  web.on("/save", HTTP_POST, webSave);
  web.on("/api/status", HTTP_GET, webStatus);
  web.on("/scan", HTTP_GET, webScan);
  // captive-portal probes
  for (const char* p : {"/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink"}) web.on(p, webNotFound);
  web.onNotFound(webNotFound);
  web.begin();
}
void startAp() {
  if (apMode) return;
  uint64_t mac = ESP.getEfuseMac();  // valid before the radio is up
  char n[20]; snprintf(n, sizeof n, "Pantry-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  apName = n;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true; apSinceMs = millis();
  Serial.printf("hotspot %s open, setup page http://%s/\r\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
  screen = Screen::Setup;
  dirty = true;
}
void stopAp() {
  if (!apMode) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
  Serial.print("hotspot closed\r\n");
  if (screen == Screen::Setup) goHome(true, true);
}

// --- UI: canvases, palette, fonts -----------------------------------------

Screen screen = Screen::Home;
bool dirty = true;
int scrollPx = 0;            // HOME list scroll offset in pixels
int swipedIndex = -1;        // HOME card slid left to reveal its trash button
int swipeOffset = 0;         // how far that card is slid (animated)
constexpr int kTrashW = 64;
int detailIndex = -1;
Product entryProduct;
int pickDays = 0, pickWeeks = 1, pickMonths = 0;  // ENTRY pickers
String entryName;                                  // name given by voice on ENTRY
uint32_t scanStartedMs = 0;
String toastText;
uint32_t toastUntil = 0;
bool dropdownOpen = false;
int dropdownH = 0;           // animated height
bool ccOpen = false;         // control centre (top-right pill)
int ccH = 0;

// Two full-screen canvases: `cv` is drawn into, `old` keeps the previous frame
// for slide transitions. 16-bit, in PSRAM.
M5Canvas canvasA(&M5.Display), canvasB(&M5.Display);
M5Canvas* cvp = &canvasA;
M5Canvas* oldp = &canvasB;
#define cv (*cvp)

constexpr int W = 320, H = 240;
// Colours are 0xRRGGBB; M5GFX takes a 32-bit int as RGB888 (a uint16_t would be read as RGB565).
inline uint32_t rgb(uint32_t c) { return c; }
const uint32_t kBg = 0x000000, kPill = 0x2B2B2D, kCard = 0x3A3A3C, kCardDark = 0x2C2C2E, kLine = 0x5A5A5E,
               kText = 0xF5F5F7, kMuted = 0x9A9A9E, kDim = 0x6E6E73, kRed = 0xFF453A, kYellow = 0xFFB020,
               kGreen = 0x30D158, kThumbBg = 0xD9D9DB, kLightPill = 0xD1D1D6, kDark = 0x1C1C1E;

// Fonts: efont CJK bitmaps carry Chinese product names; FreeSans for the Latin pills.
#define F_TITLE  (&fonts::efontCN_16_b)
#define F_BIG    (&fonts::efontCN_24_b)
#define F_MED    (&fonts::efontCN_14_b)
#define F_SMALL  (&fonts::DejaVu9)
#define F_SUB    (&fonts::efontCN_12)   // subtitles that may carry Chinese (brand, size)
#define F_PILL   (&fonts::FreeSansBold12pt7b)

void toast(const String& s, uint32_t ms = 2500) { toastText = s; toastUntil = millis() + ms; dirty = true; }

uint32_t urgencyColour(int d) { return d <= 1 ? kRed : d <= 7 ? kYellow : kText; }
String daysBig(int d) {
  if (d < 0) return "Expired";
  if (d == 0) return "Today";
  if (d == 1) return "1 Day";
  return String(d) + " Days";
}
String agoLabel(int d) {
  if (d <= 0) return "today";
  if (d == 1) return "1d ago";
  if (d < 7) return String(d) + "d ago";
  if (d < 30) return String(d / 7) + "wk ago";
  if (d < 365) return String(d / 30) + "mo ago";
  return String(d / 365) + "y ago";
}
String fit(const String& s, int maxW) {
  if (cv.textWidth(s) <= maxW) return s;
  String t = s;
  // Trim whole UTF-8 characters, not bytes.
  while (t.length() > 1 && cv.textWidth(t + "..") > maxW) {
    int n = t.length() - 1;
    while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
    t.remove(n);
  }
  return t + "..";
}
String pantryName(int id) {
  for (auto& p : pantries) if (p.id == id) return p.name;
  return id == 1 ? "Default Pantry" : "Pantry";
}
String shortPantryName(int id) {
  String n = pantryName(id);
  if (n == "Default Pantry") return "Pantry";
  return n;
}

// --- UI: widgets -----------------------------------------------------------

struct Rect { int x, y, w, h; bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

void pill(const Rect& r, uint32_t fill) { cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, r.h / 2, rgb(fill)); }
void pillText(const Rect& r, const String& label, uint32_t fill, uint32_t fg, const lgfx::IFont* font) {
  pill(r, fill);
  cv.setFont(font);
  cv.setTextColor(rgb(fg), rgb(fill));
  cv.setTextDatum(textdatum_t::middle_center);
  cv.drawString(label, r.x + r.w / 2, r.y + r.h / 2 + 1);
}
// A small down-pointing triangle, as in "Pantry v".
void caret(int x, int y, uint32_t col) { cv.fillTriangle(x - 5, y - 3, x + 5, y - 3, x, y + 3, rgb(col)); }
// Chevron (up or down) drawn as two thick strokes, like the picker arrows.
void chevron(int cx, int cy, bool up, uint32_t col) {
  int dy = up ? -5 : 5;
  for (int t = -1; t <= 1; t++) {
    cv.drawLine(cx - 9, cy - dy + t, cx, cy + dy + t, rgb(col));
    cv.drawLine(cx + 9, cy - dy + t, cx, cy + dy + t, rgb(col));
  }
}
// Paint the four corners outside a rounded rect back in `bg` (a JPEG is blitted square).
void roundCorners(int x, int y, int w, int h, int r, uint32_t bg) {
  for (int i = 0; i < r; i++) {
    float dy = r - 0.5f - i;
    int inset = r - (int)lroundf(sqrtf((float)r * r - dy * dy));
    if (inset <= 0) continue;
    cv.drawFastHLine(x, y + i, inset, rgb(bg));
    cv.drawFastHLine(x + w - inset, y + i, inset, rgb(bg));
    cv.drawFastHLine(x, y + h - 1 - i, inset, rgb(bg));
    cv.drawFastHLine(x + w - inset, y + h - 1 - i, inset, rgb(bg));
  }
}
void thumbBox(const String& barcode, bool hasImage, int x, int y, int size, int radius, uint32_t bg = kCard) {
  cv.fillSmoothRoundRect(x, y, size, size, radius, rgb(hasImage ? 0xFFFFFF : kThumbBg));
  if (!hasImage) return;
  auto it = thumbs.find(thumbKey(barcode, size));
  if (it != thumbs.end()) {
    cv.drawJpg(it->second.data(), it->second.size(), x, y, size, size);
    roundCorners(x, y, size, size, radius, bg);
  }
}

// Status icons: Home Assistant (house), WiFi, battery.
void iconHome(int x, int y, uint32_t col) {  // 16x14 box at x,y
  cv.fillTriangle(x + 8, y, x, y + 7, x + 16, y + 7, rgb(col));
  cv.fillRect(x + 3, y + 7, 10, 7, rgb(col));
  cv.fillRect(x + 6, y + 9, 4, 5, rgb(kPill));
}
void iconWifi(int cx, int baseY, int rssi, bool up, uint32_t col) {
  int bars = !up ? 0 : rssi > -60 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 1; i <= 3; i++) {
    int r = 3 + i * 4;
    cv.fillArc(cx, baseY, r, r - 2, 225, 315, rgb(i <= bars ? col : kDim));
  }
  cv.fillSmoothCircle(cx, baseY - 1, 2, rgb(bars ? col : kDim));
}
void iconBattery(int x, int y, int pct, bool charging, uint32_t col) {  // 20x11
  cv.drawRoundRect(x, y, 17, 11, 3, rgb(col));
  cv.fillRect(x + 17, y + 3, 2, 5, rgb(col));
  int w = max(1, (int)(13 * max(0, min(100, pct)) / 100));
  cv.fillRect(x + 2, y + 2, w, 7, rgb(pct <= 15 ? kRed : charging ? kGreen : col));
}

// Hit areas (constants shared by drawing and touch).
const Rect kPantryPill{8, 8, 96, 30};
const Rect kPlusPill{112, 8, 32, 30};      // HOME: "+"; ENTRY/DETAIL: "x"
const Rect kStatusPill{222, 8, 90, 30};
const Rect kScanPill{85, 206, 150, 34};    // bottom pill (SCAN NOW / SAVE / DELETE), runs to the screen edge
const Rect kDropdown{16, 44, 288, 170};
const Rect kScanPantryPill{(W - 110) / 2, 176, 110, 32};
constexpr int kRowH = 36;

// The bottom pill: rounded on top, straight sides down to the screen edge.
void bottomPill(const String& label, uint32_t fill, uint32_t fg) {
  const Rect& r = kScanPill;
  const int rad = 18;
  cv.fillSmoothRoundRect(r.x, r.y, r.w, rad * 2, rad, rgb(fill));
  cv.fillRect(r.x, r.y + rad, r.w, H - r.y - rad, rgb(fill));
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextColor(rgb(fg), rgb(fill));
  cv.setTextDatum(textdatum_t::middle_center);
  cv.drawString(label, r.x + r.w / 2, r.y + 19);
}
void drawPantryPill(const Rect& r, int pantryId) {
  pill(r, kPill);
  cv.setFont(F_TITLE);
  cv.setTextColor(rgb(kText), rgb(kPill));
  cv.setTextDatum(textdatum_t::middle_left);
  String n = fit(shortPantryName(pantryId), r.w - 34);
  cv.drawString(n, r.x + 12, r.y + r.h / 2);
  caret(r.x + r.w - 14, r.y + r.h / 2, kMuted);
}
void drawStatusPill() {
  pill(kStatusPill, kPill);
  int y = kStatusPill.y;
  iconHome(kStatusPill.x + 12, y + 8, (serverOk && mqttOk) ? kGreen : kDim);  // Home Assistant link
  iconWifi(kStatusPill.x + 46, y + 21, WiFi.RSSI(), wifiUp(), kText);
  int bat = M5.Power.getBatteryLevel();
  iconBattery(kStatusPill.x + 64, y + 10, bat < 0 ? 100 : bat,
              M5.Power.isCharging() == m5::Power_Class::is_charging, kText);
}
void drawTopBar(const char* secondPill) {
  drawPantryPill(kPantryPill, settings.pantryId);
  if (secondPill) pillText(kPlusPill, secondPill, kPill, kText, F_PILL);
  drawStatusPill();
}

void iconTrash(int cx, int cy, uint32_t col) {
  cv.fillRoundRect(cx - 7, cy - 5, 14, 15, 2, rgb(col));
  cv.fillRect(cx - 9, cy - 8, 18, 2, rgb(col));
  cv.fillRect(cx - 3, cy - 11, 6, 3, rgb(col));
  for (int dx = -4; dx <= 4; dx += 4) cv.drawFastVLine(cx + dx, cy - 2, 9, rgb(kRed));
}
// One HOME card at y (may be partly off-screen). `slide` > 0 reveals the trash button.
void drawCard(const Item& it, int y, int slide = 0) {
  const int w = 304, h = 62;
  if (slide > 0) {
    cv.fillSmoothRoundRect(8 + w - kTrashW - 4, y, kTrashW + 4, h, 14, rgb(kRed));
    iconTrash(8 + w - kTrashW / 2, y + h / 2, kText);
  }
  const int x = 8 - slide;
  cv.fillSmoothRoundRect(x, y, w, h, 14, rgb(kCard));
  thumbBox(it.barcode, it.hasImage, x + 6, y + 6, kThumbList, 10);
  cv.setFont(F_SMALL);
  cv.setTextDatum(textdatum_t::top_right);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  cv.drawString(agoLabel(it.daysAgo), x + w - 10, y + 7);
  cv.setFont(F_BIG);
  cv.setTextDatum(textdatum_t::bottom_right);
  uint32_t col = urgencyColour(it.daysLeft);
  cv.setTextColor(rgb(col), rgb(kCard));
  String big = daysBig(it.daysLeft);
  cv.drawString(big, x + w - 10, y + h - 4);
  int bigW = cv.textWidth(big);
  cv.setFont(F_TITLE);
  cv.setTextDatum(textdatum_t::top_left);
  cv.setTextColor(rgb(kText), rgb(kCard));
  cv.drawString(fit(it.name, w - 66 - 10 - 56), x + 66, y + 9);
  cv.setFont(F_SUB);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  String sub = it.brand.length() ? it.brand : it.expires;
  cv.drawString(fit(sub, w - 66 - bigW - 24), x + 66, y + 31);
}

constexpr int kListTop = 46, kCardStep = 70;
int listContentH() { return (int)items.size() * kCardStep; }
int maxScroll() { return max(0, listContentH() - (H - kListTop - 10)); }

void drawHome() {
  cv.fillSprite(rgb(kBg));
  cv.setClipRect(0, kListTop - 4, W, H - kListTop + 4);
  if (items.empty()) {
    cv.setFont(F_TITLE);
    cv.setTextDatum(textdatum_t::middle_center);
    cv.setTextColor(rgb(kMuted), rgb(kBg));
    if (!wifiUp()) {
      cv.drawString(settings.ssid.length() ? "Connecting to " + settings.ssid + "..." : "No WiFi configured", W / 2, 110);
      cv.setFont(F_SMALL);
      cv.drawString("serial: wifi <ssid> <password>", W / 2, 135);
    } else if (!serverOk) {
      cv.drawString("Server unreachable", W / 2, 110);
      cv.setFont(F_SMALL);
      cv.drawString(fit(settings.server, 300), W / 2, 135);
    } else {
      cv.drawString("Nothing here yet", W / 2, 110);
      cv.setFont(F_SMALL);
      cv.drawString("Tap SCAN NOW and point at a barcode", W / 2, 135);
    }
  }
  for (size_t i = 0; i < items.size(); i++) {
    int y = kListTop + (int)i * kCardStep - scrollPx;
    if (y > H || y + 62 < kListTop - 4) continue;
    drawCard(items[i], y, (int)i == swipedIndex ? swipeOffset : 0);
  }
  cv.clearClipRect();
  cv.fillRect(0, 0, W, kListTop - 4, rgb(kBg));  // cards never show through the bar
  drawTopBar("+");
  bottomPill("SCAN NOW", kLightPill, kDark);
}

// Red glow at the top of the SCAN page, breathing with strength in [0,1].
void drawGlow(float strength) {
  const int cx = W / 2, cy = -40;
  for (int r = 230; r > 0; r -= 6) {
    float f = 1.0f - (float)r / 230.0f;      // 0 at the edge, 1 at the centre
    f = f * f * strength;
    uint8_t red = (uint8_t)(140 * f), g = (uint8_t)(8 * f), b = (uint8_t)(10 * f);
    cv.fillEllipse(cx, cy, r + 60, r, rgb(((uint32_t)red << 16) | ((uint32_t)g << 8) | b));
  }
}
void drawScan() {
  cv.fillSprite(rgb(kBg));
  float t = (millis() % 2400) / 2400.0f;
  float breathe = 0.65f + 0.35f * (0.5f + 0.5f * sinf(t * 6.2831853f));
  drawGlow(breathe);
  cv.setTextDatum(textdatum_t::middle_center);
  cv.setFont(F_BIG);
  cv.setTextColor(rgb(kText), rgb(kBg));
  if (!qrPresent) {
    cv.drawString("No scanner", W / 2, 100);
    cv.setFont(F_SMALL);
    cv.setTextColor(rgb(kMuted), rgb(kBg));
    cv.drawString("QRCode module not found; serial: barcode <digits>", W / 2, 130);
  } else {
    int dots = (millis() / 400) % 4;
    String s = "Scanning";
    for (int i = 0; i < dots; i++) s += ".";
    cv.setTextDatum(textdatum_t::middle_left);  // centred on the word, dots trailing
    int w = cv.textWidth("Scanning...");
    cv.drawString(s, (W - w) / 2, 112);
    if (!qrArmed) {
      cv.setTextDatum(textdatum_t::middle_center);
      cv.setFont(F_SMALL);
      cv.setTextColor(rgb(kMuted), rgb(kBg));
      cv.drawString("waking the scanner", W / 2, 142);
    }
  }
  drawPantryPill(kScanPantryPill, settings.pantryId);
}

// ENTRY / DETAIL product card.
const Rect kCardBig{8, 46, 304, 190};
const int kPickY = 110, kPickW = 72, kPickH = 94, kPickX0 = 74, kPickGap = 6;

const Rect kMicPill{kCardBig.x + kCardBig.w - 46, kCardBig.y + 8, 38, 30};
void iconMic(int cx, int cy, uint32_t col) {
  cv.fillSmoothRoundRect(cx - 4, cy - 10, 8, 13, 4, rgb(col));
  cv.drawArc(cx, cy - 1, 8, 6, 0, 180, rgb(col));
  cv.fillRect(cx - 1, cy + 7, 2, 4, rgb(col));
  cv.fillRect(cx - 5, cy + 11, 10, 2, rgb(col));
}
void drawProductHeader(const String& barcode, const String& name, const String& sub, bool hasImage, const String& line3) {
  const Rect& c = kCardBig;
  cv.fillSmoothRoundRect(c.x, c.y, c.w, c.h, 16, rgb(kCard));
  thumbBox(barcode, hasImage, c.x + 6, c.y + 6, kThumbBig, 12);
  pill(kMicPill, kCardDark);
  iconMic(kMicPill.x + kMicPill.w / 2, kMicPill.y + kMicPill.h / 2, kText);
  int tx = c.x + 72, tw = c.w - 72 - 10 - kMicPill.w - 6;
  cv.setFont(F_TITLE);
  cv.setTextDatum(textdatum_t::top_left);
  cv.setTextColor(rgb(kText), rgb(kCard));
  cv.drawString(fit(name, tw), tx, c.y + 10);
  cv.setFont(F_SUB);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  if (sub.length()) cv.drawString(fit(sub, tw), tx, c.y + 31);
  if (line3.length()) cv.drawString(fit(line3, tw), tx, c.y + 46);
}
void drawPicker(int i, const char* label, int value) {
  int x = kPickX0 + i * (kPickW + kPickGap), y = kPickY;
  cv.fillSmoothRoundRect(x, y, kPickW, kPickH, 10, rgb(kCardDark));
  cv.setFont(F_MED);
  cv.setTextDatum(textdatum_t::top_center);
  cv.setTextColor(rgb(kText), rgb(kCardDark));
  cv.drawString(label, x + kPickW / 2, y + 6);
  chevron(x + kPickW / 2, y + 32, true, kDim);
  cv.setFont(F_BIG);
  cv.setTextDatum(textdatum_t::middle_center);
  cv.setTextColor(rgb(kText), rgb(kCardDark));
  cv.drawString(String(value), x + kPickW / 2, y + 56);
  chevron(x + kPickW / 2, y + 82, false, kDim);
}
long entryDate() { return addMonths(today, pickMonths) + pickDays + 7L * pickWeeks; }

void drawEntry() {
  cv.fillSprite(rgb(kBg));
  const Product& p = entryProduct;
  String name = entryName.length() ? entryName : p.found && p.name.length() ? p.name : "Unknown product";
  String sub = p.brand;
  if (p.qty.length()) sub += (sub.length() ? " / " : "") + p.qty;
  if (entryName.length()) sub = "named by voice";
  else if (!p.found) sub = "not in the databases yet. Tap the mic to name it";
  long d = entryDate();
  String when = String(weekday(d)) + " " + fmtDate(d) + "  (" + daysBig((int)(d - today)) + ")";
  drawProductHeader(p.barcode, name, sub, p.hasImage, when);
  drawPicker(0, "Days", pickDays);
  drawPicker(1, "Weeks", pickWeeks);
  drawPicker(2, "Months", pickMonths);
  drawTopBar("x");
  bottomPill("SAVE", kGreen, kDark);
}
void drawDetail() {
  cv.fillSprite(rgb(kBg));
  if (detailIndex < 0 || detailIndex >= (int)items.size()) return;
  const Item& it = items[detailIndex];
  String sub = it.brand;
  if (it.qty.length()) sub += (sub.length() ? " / " : "") + it.qty;
  long d; String date = it.expires;
  if (parseDate(it.expires, d)) date = String(weekday(d)) + " " + it.expires;
  drawProductHeader(it.barcode, it.name, sub, it.hasImage, "added " + agoLabel(it.daysAgo) + " / " + it.barcode);
  cv.setFont(F_SMALL);
  cv.setTextDatum(textdatum_t::top_left);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  cv.drawString(it.daysLeft < 0 ? "EXPIRED ON" : "EXPIRES", kCardBig.x + 16, kPickY + 4);
  cv.setFont(F_TITLE);
  cv.setTextColor(rgb(kText), rgb(kCard));
  cv.drawString(date, kCardBig.x + 16, kPickY + 20);
  cv.setFont(F_BIG);
  cv.setTextDatum(textdatum_t::top_right);
  cv.setTextColor(rgb(urgencyColour(it.daysLeft)), rgb(kCard));
  cv.drawString(daysBig(it.daysLeft), kCardBig.x + kCardBig.w - 16, kPickY + 2);
  drawTopBar("x");
  bottomPill("DELETE", kRed, kText);
}

// Pantry dropdown overlay (animated height). Opens below the pill, or above it on SCAN.
int dropdownRows() { return (int)pantries.size() + 1; }
int dropdownFullH() { return min(kDropdown.h, dropdownRows() * kRowH + 8); }
Rect dropdownRect() {
  if (screen == Screen::Scan) return Rect{kDropdown.x, kScanPantryPill.y - 6 - dropdownH, kDropdown.w, dropdownH};
  return Rect{kDropdown.x, kPantryPill.y + kPantryPill.h + 6, kDropdown.w, dropdownH};
}
void drawDropdown() {
  if (dropdownH <= 0) return;
  Rect r = dropdownRect();
  cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 16, rgb(kCard));
  cv.setClipRect(r.x, r.y, r.w, r.h);
  cv.setFont(F_TITLE);
  cv.setTextDatum(textdatum_t::middle_left);
  for (int i = 0; i < dropdownRows(); i++) {
    int y = r.y + 4 + i * kRowH;
    bool create = i == (int)pantries.size();
    if (create) {
      cv.setTextColor(rgb(kMuted), rgb(kCard));
      cv.drawString("+", r.x + 16, y + kRowH / 2);
      cv.drawString("Create Pantry", r.x + 40, y + kRowH / 2);
    } else {
      bool sel = pantries[i].id == settings.pantryId;
      cv.setTextColor(rgb(sel ? kText : 0xDADADE), rgb(kCard));
      cv.drawString(fit(pantries[i].name, r.w - 80), r.x + 16, y + kRowH / 2);
      if (pantries[i].items) {
        cv.setFont(F_SMALL);
        cv.setTextDatum(textdatum_t::middle_right);
        cv.setTextColor(rgb(kMuted), rgb(kCard));
        cv.drawString(String(pantries[i].items), r.x + r.w - 16, y + kRowH / 2);
        cv.setFont(F_TITLE);
        cv.setTextDatum(textdatum_t::middle_left);
      }
      if (sel) cv.fillSmoothCircle(r.x + r.w - 40, y + kRowH / 2, 3, rgb(kGreen));
    }
    if (i < dropdownRows() - 1) cv.drawFastHLine(r.x + 12, y + kRowH - 1, r.w - 24, rgb(kLine));
  }
  cv.clearClipRect();
}

// Control centre: beep toggle, brightness slider, battery / Home Assistant / WiFi.
const Rect kCC{W - 8 - 236, 44, 236, 168};
const Rect kCCBeep{kCC.x + kCC.w - 16 - 46, kCC.y + 12, 46, 26};
const Rect kCCSlider{kCC.x + 16, kCC.y + 70, kCC.w - 32, 24};   // touch band around the track
Rect ccRect() { return Rect{kCC.x, kCC.y, kCC.w, ccH}; }
void toggleSwitch(const Rect& r, bool on) {
  cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, r.h / 2, rgb(on ? kGreen : 0x636366));
  int kx = on ? r.x + r.w - r.h + 2 : r.x + 2;
  cv.fillSmoothCircle(kx + (r.h - 4) / 2, r.y + r.h / 2, (r.h - 4) / 2, rgb(0xFFFFFF));
}
void drawControlCenter() {
  if (ccH <= 0) return;
  Rect r = ccRect();
  cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 16, rgb(kCard));
  cv.setClipRect(r.x, r.y, r.w, r.h);
  cv.setFont(F_TITLE);
  cv.setTextDatum(textdatum_t::middle_left);
  cv.setTextColor(rgb(kText), rgb(kCard));
  cv.drawString("Beep", kCC.x + 16, kCCBeep.y + kCCBeep.h / 2);
  toggleSwitch(kCCBeep, settings.beep);
  cv.drawFastHLine(kCC.x + 12, kCC.y + 48, kCC.w - 24, rgb(kLine));
  cv.setFont(F_SMALL);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  cv.drawString("BRIGHTNESS", kCC.x + 16, kCC.y + 60);
  // slider: track with a filled part and a knob
  int tx0 = kCCSlider.x + 8, tx1 = kCCSlider.x + kCCSlider.w - 8, ty = kCCSlider.y + kCCSlider.h / 2;
  cv.fillSmoothRoundRect(tx0, ty - 3, tx1 - tx0, 6, 3, rgb(0x636366));
  int kx = tx0 + (tx1 - tx0) * max(0, settings.bright - 10) / 245;
  cv.fillSmoothRoundRect(tx0, ty - 3, max(6, kx - tx0), 6, 3, rgb(kText));
  cv.fillSmoothCircle(kx, ty, 9, rgb(0xFFFFFF));
  cv.drawFastHLine(kCC.x + 12, kCC.y + 100, kCC.w - 24, rgb(kLine));
  // battery
  int bat = M5.Power.getBatteryLevel();
  bool chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
  cv.setFont(F_TITLE);
  cv.setTextColor(rgb(kText), rgb(kCard));
  cv.drawString(String(bat < 0 ? 100 : bat) + "%", kCC.x + 16, kCC.y + 116);
  cv.setFont(F_SMALL);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  cv.drawString(chg ? "Battery, charging" : "Battery", kCC.x + 62, kCC.y + 116);
  iconBattery(kCC.x + kCC.w - 16 - 20, kCC.y + 110, bat < 0 ? 100 : bat, chg, kText);
  // Home Assistant + WiFi
  bool ha = serverOk && mqttOk;
  iconHome(kCC.x + 16, kCC.y + 134, ha ? kGreen : kDim);
  cv.setTextColor(rgb(ha ? kText : kMuted), rgb(kCard));
  cv.drawString(ha ? "Home Assistant connected" : serverOk ? "Server ok, no Home Assistant" : "Server unreachable", kCC.x + 40, kCC.y + 141);
  iconWifi(kCC.x + 24, kCC.y + 162, WiFi.RSSI(), wifiUp(), kText);
  cv.setTextColor(rgb(kMuted), rgb(kCard));
  cv.drawString(wifiUp() ? fit(settings.ssid, 110) + "  " + String(WiFi.RSSI()) + " dBm" : "WiFi down", kCC.x + 40, kCC.y + 157);
  cv.clearClipRect();
}

void drawSetup() {
  cv.fillSprite(rgb(kBg));
  cv.setFont(F_BIG);
  cv.setTextDatum(textdatum_t::top_left);
  cv.setTextColor(rgb(kText), rgb(kBg));
  cv.drawString("Set me up", 16, 14);
  cv.setFont(F_SUB);
  cv.setTextColor(rgb(kMuted), rgb(kBg));
  cv.drawString("1. Join this WiFi with your phone", 16, 58);
  cv.setFont(F_TITLE); cv.setTextColor(rgb(kText), rgb(kBg));
  cv.drawString(apName, 32, 76);
  cv.setFont(F_SUB); cv.setTextColor(rgb(kMuted), rgb(kBg));
  cv.drawString("2. Open the page that pops up, or", 16, 104);
  cv.setFont(F_TITLE); cv.setTextColor(rgb(kText), rgb(kBg));
  cv.drawString("http://192.168.4.1/", 32, 122);
  cv.setFont(F_SUB); cv.setTextColor(rgb(kMuted), rgb(kBg));
  cv.drawString("3. Enter WiFi + server URL", 16, 150);
  if (settings.ssid.length()) cv.drawString(fit("still trying " + settings.ssid + "...", 190), 16, 174);
  // WiFi QR for the hotspot (scan with the phone camera)
  String wifiQr = "WIFI:T:nopass;S:" + apName + ";;";
  cv.fillRect(216, 56, 96, 96, rgb(0xFFFFFF));
  cv.qrcode(wifiQr.c_str(), 220, 60, 88, 3);
  cv.setFont(F_SMALL); cv.setTextDatum(textdatum_t::top_center);
  cv.drawString("scan to join", 264, 156);
  bottomPill(wifiUp() ? "CONTINUE" : "RETRY WIFI", kLightPill, kDark);
}

void drawScreen() {
  switch (screen) {
    case Screen::Setup: drawSetup(); break;
    case Screen::Home: drawHome(); break;
    case Screen::Scan: drawScan(); break;
    case Screen::Entry: drawEntry(); break;
    case Screen::Detail: drawDetail(); break;
  }
  if (dropdownOpen || dropdownH > 0) drawDropdown();
  if (ccOpen || ccH > 0) drawControlCenter();
  if (millis() < toastUntil) {
    cv.setFont(F_MED);
    int w = cv.textWidth(toastText) + 28;
    pillText(Rect{(W - w) / 2, 168, w, 32}, toastText, 0x48484A, kText, F_MED);
  }
}

void present();  // defined with the animation helpers below

// Voice naming -------------------------------------------------------------------
// Records 4 s from the CoreS3 microphones (16 kHz mono) with a live level meter,
// posts a WAV to the server, which transcribes it (Chinese or English).
constexpr int kRecRate = 16000, kRecSeconds = 4, kRecChunk = 1600;  // 100 ms chunks

void drawListening(int level, int msLeft, const char* caption) {
  Rect r{40, 86, 240, 70};
  cv.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 18, rgb(0x1C1C1E));
  cv.setFont(F_TITLE);
  cv.setTextDatum(textdatum_t::middle_left);
  cv.setTextColor(rgb(kText), rgb(0x1C1C1E));
  cv.drawString(caption, r.x + 18, r.y + 22);
  cv.setFont(F_SMALL);
  cv.setTextDatum(textdatum_t::middle_right);
  cv.setTextColor(rgb(kMuted), rgb(0x1C1C1E));
  if (msLeft >= 0) cv.drawString(String(msLeft / 1000.0f, 1) + " s", r.x + r.w - 16, r.y + 22);
  // 20 bars, a crude VU meter
  for (int i = 0; i < 20; i++) {
    int x = r.x + 18 + i * 10;
    bool on = level * 20 / 100 > i;
    cv.fillSmoothRoundRect(x, r.y + 42, 7, 14, 3, rgb(on ? (i > 15 ? kRed : i > 10 ? kYellow : kGreen) : 0x3A3A3C));
  }
}

bool recordAndTranscribe(String& text) {
  if (!M5.Mic.isEnabled()) { toast("No microphone"); return false; }
  const size_t samples = (size_t)kRecRate * kRecSeconds;
  int16_t* pcm = (int16_t*)heap_caps_malloc(samples * 2 + 44, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) pcm = (int16_t*)malloc(samples * 2 + 44);
  if (!pcm) { toast("Out of memory"); return false; }
  uint8_t* wav = (uint8_t*)pcm;
  int16_t* data = (int16_t*)(wav + 44);
  M5.Mic.begin();
  uint32_t t0 = millis();
  int level = 0;
  for (size_t off = 0; off < samples; off += kRecChunk) {
    size_t n = min((size_t)kRecChunk, samples - off);
    M5.Mic.record(data + off, n, kRecRate);
    while (M5.Mic.isRecording()) delay(1);
    int peak = 0;
    for (size_t i = 0; i < n; i++) peak = max(peak, abs((int)data[off + i]));
    level = max(peak * 100 / 8000, level - 12);  // quick attack, slow decay
    drawScreen();
    drawListening(min(level, 100), (int)(kRecSeconds * 1000 - (millis() - t0)), "Listening...");
    present();
  }
  M5.Mic.end();
  // WAV header (PCM16 mono).
  uint32_t dataBytes = samples * 2, rate = kRecRate, byteRate = kRecRate * 2;
  memcpy(wav, "RIFF", 4); uint32_t riff = 36 + dataBytes; memcpy(wav + 4, &riff, 4);
  memcpy(wav + 8, "WAVEfmt ", 8); uint32_t fmtLen = 16; memcpy(wav + 16, &fmtLen, 4);
  uint16_t pcmTag = 1, ch = 1, align = 2, bits = 16;
  memcpy(wav + 20, &pcmTag, 2); memcpy(wav + 22, &ch, 2); memcpy(wav + 24, &rate, 4);
  memcpy(wav + 28, &byteRate, 4); memcpy(wav + 32, &align, 2); memcpy(wav + 34, &bits, 2);
  memcpy(wav + 36, "data", 4); memcpy(wav + 40, &dataBytes, 4);
  drawScreen(); drawListening(0, -1, "Transcribing..."); present();
  String resp;
  int code = httpPostBytes("/api/transcribe", wav, 44 + dataBytes, "audio/wav", &resp);
  free(pcm);
  if (code != 200) {
    JsonDocument e;
    String why = code > 0 ? "HTTP " + String(code) : lastError;
    if (code > 0 && !deserializeJson(e, resp) && !e["error"].isNull()) why = str(e["error"]);
    toast(fit("Voice: " + why, 290), 3500);
    Serial.printf("transcribe failed: %s\r\n", why.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { toast("Voice: bad reply"); return false; }
  text = str(doc["text"]);
  text.trim();
  Serial.printf("transcribed (%s): %s\r\n", str(doc["language"]).c_str(), text.c_str());
  if (!text.length()) { toast("Heard nothing"); return false; }
  return true;
}

// Animation helpers ---------------------------------------------------------

float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

// Slide from the frame in `old` to the frame in `cv`. dir = +1: new comes from the right.
void slideTransition(int dir) {
  const int steps = 7;
  M5.Display.startWrite();
  for (int i = 1; i <= steps; i++) {
    int d = (int)(W * easeOut((float)i / steps));
    oldp->pushSprite(-dir * d, 0);
    cv.pushSprite(dir * (W - d), 0);
  }
  M5.Display.endWrite();
}
void present() { cv.pushSprite(0, 0); }
// Render the current screen into `cv`, keeping the previous frame in `old`.
void render(bool transition = false, int dir = +1) {
  std::swap(cvp, oldp);  // what was on screen is now `old`
  drawScreen();
  if (transition) slideTransition(dir); else present();
  dirty = false;
}

// Dropdown open/close animation (blocking, ~120 ms).
void setDropdown(bool open) {
  if (open && pantries.empty()) fetchPantries();
  dropdownOpen = open;
  int full = dropdownFullH();
  const int steps = 5;
  for (int i = 1; i <= steps; i++) {
    float t = easeOut((float)i / steps);
    dropdownH = open ? (int)(full * t) : (int)(full * (1 - t));
    render();
  }
  dropdownH = open ? full : 0;
  render();
}

void setControlCenter(bool open) {
  ccOpen = open;
  const int steps = 5, full = kCC.h;
  for (int i = 1; i <= steps; i++) {
    float t = easeOut((float)i / steps);
    ccH = open ? (int)(full * t) : (int)(full * (1 - t));
    render();
  }
  ccH = open ? full : 0;
  render();
}
void setBrightnessFromX(int x) {
  int tx0 = kCCSlider.x + 8, tx1 = kCCSlider.x + kCCSlider.w - 8;
  int v = 10 + 245 * constrain(x - tx0, 0, tx1 - tx0) / (tx1 - tx0);
  if (v != settings.bright) { settings.bright = v; M5.Display.setBrightness(v); dirty = true; }
}

// --- Navigation ----------------------------------------------------------

void goHome(bool refresh, bool animate = true) {
  if (screen == Screen::Scan) qrStop();
  bool wasHome = screen == Screen::Home;
  screen = Screen::Home;
  dropdownOpen = false; dropdownH = 0; ccOpen = false; ccH = 0;
  swipedIndex = -1; swipeOffset = 0;
  if (refresh) {
    fetchItems();
    if (scrollPx > maxScroll()) scrollPx = maxScroll();
  }
  render(animate && !wasHome, -1);
}
void goScan() {
  dropdownOpen = false; dropdownH = 0;
  screen = Screen::Scan;
  scanStartedMs = millis();
  render(true, +1);
  qrStart();
}
// Items with this barcode in any pantry; the first is the one expiring soonest.
bool findExisting(const String& barcode, int& pantryId, int& itemId) {
  String body;
  int code = httpRequest("GET", "/api/items?barcode=" + barcode, "", &body);
  JsonDocument doc;
  bool bad = code != 200 || deserializeJson(doc, body);
  JsonArray arr = doc["items"].as<JsonArray>();
  Serial.printf("existing lookup: http %d, %u match(es)\r\n", code, bad ? 0 : (unsigned)arr.size());
  if (bad || arr.size() == 0) return false;
  pantryId = arr[0]["pantry_id"] | 1;
  itemId = arr[0]["id"] | 0;
  return true;
}
void goDetail(int i);
void goEntry(const String& barcode) {
  if (screen == Screen::Scan) qrStop();
  if (today == 0 && !fetchItems()) { toast("Server unreachable"); goHome(false); return; }
  // Already tracked? Show it instead of adding a duplicate (switching pantry if needed).
  int pid = 0, iid = 0;
  if (findExisting(barcode, pid, iid)) {
    if (pid != settings.pantryId) { settings.pantryId = pid; settings.save(); scrollPx = 0; }
    fetchItems();
    for (size_t i = 0; i < items.size(); i++) if (items[i].id == iid) {
      scrollPx = min(maxScroll(), max(0, (int)i * kCardStep));
      toast("Already in " + pantryName(pid));
      goDetail((int)i);
      return;
    }
  }
  if (!fetchProduct(barcode, entryProduct)) { toast("Lookup failed: " + lastError); goHome(false); return; }
  pickDays = 0; pickWeeks = 1; pickMonths = 0;
  entryName = "";
  screen = Screen::Entry;
  render(true, +1);
  if (entryProduct.hasImage) { ensureThumb(barcode, kThumbBig); dirty = true; }
}
void goDetail(int i) {
  detailIndex = i;
  screen = Screen::Detail;
  render(true, +1);
  if (items[i].hasImage) { ensureThumb(items[i].barcode, kThumbBig); dirty = true; }
}
void saveEntry() {
  String err;
  toast("Saving...", 4000);
  render();
  if (postItem(entryProduct.barcode, entryDate(), settings.pantryId, entryName, err)) {
    toast("Added " + (entryProduct.name.length() ? entryProduct.name : entryProduct.barcode));
    goHome(true);
  } else {
    toast("Save failed: " + err);
  }
}
void selectPantry(int id) {
  settings.pantryId = id;
  settings.save();
  if (dropdownOpen) setDropdown(false);
  if (screen == Screen::Home) { scrollPx = 0; fetchItems(); }
  dirty = true;
}

// --- Touch ---------------------------------------------------------------

void onDropdownTap(int x, int y) {
  Rect r = dropdownRect();
  if (!r.has(x, y)) { setDropdown(false); return; }
  int i = (y - r.y - 4) / kRowH;
  if (i < 0 || i >= dropdownRows()) return;
  if (i == (int)pantries.size()) {
    int id = createPantry();
    if (id) { fetchPantries(); selectPantry(id); toast("Created " + pantryName(id)); }
    else toast("Could not create pantry");
    return;
  }
  selectPantry(pantries[i].id);
}

int cardAt(int y);
void animateSwipe(int target);
void onTap(int x, int y) {
  if (screen == Screen::Setup) {
    if (kScanPill.has(x, y)) {
      if (wifiUp()) stopAp();
      else if (settings.ssid.length()) { WiFi.begin(settings.ssid.c_str(), settings.pass.c_str()); staTryMs = millis(); toast("Trying " + settings.ssid); }
    }
    return;
  }
  if (ccOpen) {
    if (!ccRect().has(x, y)) { settings.save(); setControlCenter(false); return; }
    if (y >= kCC.y + 150 && y < kCC.y + kCC.h) {  // WiFi row: reconfigure
      settings.save(); ccOpen = false; ccH = 0; startAp(); toast("Hotspot " + apName + " is open", 4000); return;
    }
    if (kCCBeep.has(x, y) || (y >= kCC.y + 4 && y < kCC.y + 46)) {
      settings.beep = !settings.beep; settings.save(); dirty = true;
      toast(settings.beep ? "Beep on from the next scan" : "Beep off from the next scan", 1800);
    } else if (y >= kCCSlider.y - 10 && y < kCCSlider.y + kCCSlider.h + 10) {
      setBrightnessFromX(x);
    }
    return;
  }
  if (dropdownOpen) { onDropdownTap(x, y); return; }
  switch (screen) {
    case Screen::Home:
      if (kPantryPill.has(x, y)) { fetchPantries(); setDropdown(true); }
      else if (kPlusPill.has(x, y)) toast("Add on the web page: " + settings.server);
      else if (kStatusPill.has(x, y)) setControlCenter(true);
      else if (swipedIndex >= 0) {  // a card shows its trash button: delete or dismiss
        int i = cardAt(y);
        if (i == swipedIndex && x >= 8 + 304 - kTrashW - 4) {
          String name = items[i].name;
          int id = items[i].id;
          swipedIndex = -1; swipeOffset = 0;
          if (deleteItem(id)) { toast("Removed " + name); fetchItems(); if (scrollPx > maxScroll()) scrollPx = maxScroll(); }
          else toast("Delete failed");
          dirty = true;
        } else animateSwipe(0);
      }
      else if (kScanPill.has(x, y)) goScan();
      else if (y >= kListTop) {
        int i = cardAt(y);
        if (i >= 0) goDetail(i);
      }
      break;
    case Screen::Scan:
      if (kScanPantryPill.has(x, y)) { fetchPantries(); setDropdown(true); }
      else goHome(false);
      break;
    case Screen::Entry:
      if (kStatusPill.has(x, y)) setControlCenter(true);
      else if (kPantryPill.has(x, y)) { fetchPantries(); setDropdown(true); }
      else if (kPlusPill.has(x, y)) goHome(false);
      else if (kMicPill.has(x, y)) { String t; if (recordAndTranscribe(t)) entryName = t; dirty = true; }
      else if (kScanPill.has(x, y)) saveEntry();
      else if (y >= kPickY && y < kPickY + kPickH) {
        int i = (x - kPickX0) / (kPickW + kPickGap);
        int inCol = (x - kPickX0) % (kPickW + kPickGap);
        if (x >= kPickX0 && i >= 0 && i < 3 && inCol < kPickW) {
          int delta = (y < kPickY + kPickH / 2) ? +1 : -1;
          int* v = i == 0 ? &pickDays : i == 1 ? &pickWeeks : &pickMonths;
          *v = max(0, min(99, *v + delta));
          dirty = true;
        }
      }
      break;
    case Screen::Detail:
      if (kStatusPill.has(x, y)) setControlCenter(true);
      else if (kPlusPill.has(x, y) || kPantryPill.has(x, y)) goHome(false);
      else if (kMicPill.has(x, y) && detailIndex >= 0 && detailIndex < (int)items.size()) {
        String t;
        if (recordAndTranscribe(t)) {
          if (renameItem(items[detailIndex].id, t)) { toast("Renamed to " + t); fetchItems(); }
          else toast("Rename failed");
        }
        dirty = true;
      }
      else if (kScanPill.has(x, y) && detailIndex >= 0 && detailIndex < (int)items.size()) {
        String name = items[detailIndex].name;
        if (deleteItem(items[detailIndex].id)) { toast("Removed " + name); goHome(true); }
        else toast("Delete failed");
      }
      break;
  }
}

// Vertical flick on HOME scrolls the list by whole cards.
int flickStartX = -1, flickStartY = -1;
void onFlick(int dy) {
  if (screen != Screen::Home || dropdownOpen) return;
  if (dy < -25 && scrollPx < maxScroll()) { scrollPx = min(maxScroll(), scrollPx + kCardStep); dirty = true; }
  if (dy > 25 && scrollPx > 0) { scrollPx = max(0, scrollPx - kCardStep); dirty = true; }
}
// Slide the revealed card in or out (short blocking animation).
void animateSwipe(int target) {
  int from = swipeOffset;
  for (int k = 1; k <= 4; k++) { swipeOffset = from + (target - from) * k / 4; render(); }
  if (target == 0) swipedIndex = -1;
}
int cardAt(int y) {
  if (y < kListTop) return -1;
  int i = (y - kListTop + scrollPx) / kCardStep, within = (y - kListTop + scrollPx) % kCardStep;
  return (i >= 0 && i < (int)items.size() && within < 62) ? i : -1;
}
// Horizontal swipe: left-to-right goes back; right-to-left on a HOME card reveals its trash button.
void onSwipe(int dx, int startY) {
  if (ccOpen) return;
  if (dropdownOpen) { setDropdown(false); return; }
  if (dx > 0) {  // back
    if (screen == Screen::Home) { if (swipedIndex >= 0) animateSwipe(0); }
    else goHome(false);
    return;
  }
  if (screen != Screen::Home) return;
  int i = cardAt(startY);
  if (i < 0) return;
  if (swipedIndex >= 0 && swipedIndex != i) { swipedIndex = -1; swipeOffset = 0; }
  swipedIndex = i;
  animateSwipe(kTrashW);
}

// --- Serial console ------------------------------------------------------

void printStatus() {
  Serial.printf("wifi: %s (%s) ip %s rssi %d\r\n", settings.ssid.c_str(), wifiUp() ? "connected" : "down",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("server: %s (%s) mqtt %s %s\r\n", settings.server.c_str(), serverOk ? "ok" : "unreachable",
                mqttOk ? "connected" : "down", lastError.c_str());
  Serial.printf("qrcode module: %s fw '%s' pins tx %d rx %d\r\n", qrPresent ? "present" : "absent",
                qrVersion.c_str(), settings.qrTx, settings.qrRx);
  Serial.printf("setup page: http://%s/  hotspot: %s\r\n", wifiUp() ? WiFi.localIP().toString().c_str() : "192.168.4.1",
                apMode ? apName.c_str() : "off");
  Serial.printf("pantry: %d '%s'  items: %d  today: %s  screen: %d  battery: %d%%\r\n", settings.pantryId,
                pantryName(settings.pantryId).c_str(), (int)items.size(), today ? fmtDate(today).c_str() : "?",
                (int)screen, M5.Power.getBatteryLevel());
}

void dumpShot() {
  // Dump the canvas as base64 (RGB565 big-endian rows) between SHOT-BEGIN/END.
  const int bytes = W * H * 2;
  const uint8_t* buf = (const uint8_t*)cv.getBuffer();
  Serial.setTxTimeoutMs(500);
  Serial.printf("SHOT-BEGIN %d %d\r\n", W, H);
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char line[80]; int n = 0;
  for (int i = 0; i < bytes; i += 3) {
    uint32_t v = ((uint32_t)buf[i] << 16) | ((i + 1 < bytes ? buf[i + 1] : 0) << 8) | (i + 2 < bytes ? buf[i + 2] : 0);
    line[n++] = tbl[(v >> 18) & 63]; line[n++] = tbl[(v >> 12) & 63];
    line[n++] = i + 1 < bytes ? tbl[(v >> 6) & 63] : '=';
    line[n++] = i + 2 < bytes ? tbl[v & 63] : '=';
    if (n >= 76) { line[n] = 0; Serial.println(line); n = 0; }
  }
  if (n) { line[n] = 0; Serial.println(line); }
  Serial.print("SHOT-END\r\n");
  Serial.setTxTimeoutMs(0);
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();
  if (cmd == "help" || cmd == "?") {
    Serial.print("commands: status | list | pantries | pantry <id> | refresh | scan | barcode <code> | wifi <ssid> <pass> | "
                 "server <url> | qrpins <tx> <rx> | qrinfo | qrprobe [baud] | qrlevels | qrtrig [ms] | qrcmd <hex..> | "
                 "tap <x> <y> | flick <dy> | swipe <dx> <y> | say | beep on|off | cc | hotspot [off] | forget | bright <0-255> | shot | home | reboot\r\n");
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "list") {
    for (auto& it : items) Serial.printf("%4d  %-14s %-28s exp %s (%d) %s\r\n", it.id, it.barcode.c_str(), it.name.c_str(), it.expires.c_str(), it.daysLeft, it.pantry.c_str());
    if (items.empty()) Serial.print("(no items)\r\n");
  } else if (cmd == "pantries") {
    fetchPantries();
    for (auto& p : pantries) Serial.printf("%3d  %-20s %d items%s\r\n", p.id, p.name.c_str(), p.items, p.id == settings.pantryId ? "  <- selected" : "");
  } else if (cmd == "pantry") {
    if (arg.toInt() > 0) { selectPantry(arg.toInt()); Serial.printf("pantry %d selected\r\n", settings.pantryId); }
  } else if (cmd == "refresh") {
    Serial.printf("fetch: %s\r\n", fetchItems() ? "ok" : lastError.c_str());
    dirty = true;
  } else if (cmd == "scan") {
    goScan();
  } else if (cmd == "home") {
    goHome(false);
  } else if (cmd == "barcode") {
    if (arg.length()) { Serial.printf("fake scan: %s\r\n", arg.c_str()); goEntry(arg); }
  } else if (cmd == "wifi") {
    int s2 = arg.indexOf(' ');
    if (s2 < 0) { Serial.print("usage: wifi <ssid> <password>\r\n"); return; }
    settings.ssid = arg.substring(0, s2); settings.pass = arg.substring(s2 + 1);
    settings.save();
    Serial.printf("wifi saved: %s; reconnecting\r\n", settings.ssid.c_str());
    WiFi.disconnect(); WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
  } else if (cmd == "server") {
    if (!arg.startsWith("http")) { Serial.print("usage: server http://host:port\r\n"); return; }
    settings.server = arg;
    settings.save();
    Serial.printf("server saved: %s\r\n", settings.server.c_str());
    thumbs.clear(); thumbMissing.clear();
    fetchItems(); dirty = true;
  } else if (cmd == "qrpins") {
    int s2 = arg.indexOf(' ');
    if (s2 < 0) { Serial.print("usage: qrpins <tx> <rx>\r\n"); return; }
    settings.qrTx = arg.substring(0, s2).toInt();
    settings.qrRx = arg.substring(s2 + 1).toInt();
    settings.save();
    Serial.printf("qr pins saved tx %d rx %d; re-init: %s\r\n", settings.qrTx, settings.qrRx, qrInit() ? "ok" : "module not found");
  } else if (cmd == "qrinfo") {
    qrInfo();
  } else if (cmd == "qrprobe") {
    qrProbe(arg.toInt());
  } else if (cmd == "qrlevels") {
    qrLevels();
  } else if (cmd == "qrtrig") {
    qrTrig(arg.toInt());
  } else if (cmd == "qrcmd") {
    qrCmd(arg);
  } else if (cmd == "tap") {  // inject a touch for testing: tap <x> <y>
    int s2 = arg.indexOf(' ');
    if (s2 > 0) onTap(arg.substring(0, s2).toInt(), arg.substring(s2 + 1).toInt());
  } else if (cmd == "say") {  // record + transcribe now; prints the text
    String t; if (recordAndTranscribe(t)) Serial.printf("heard: %s\r\n", t.c_str());
    dirty = true;
  } else if (cmd == "flick") {
    onFlick(arg.toInt());
  } else if (cmd == "swipe") {  // swipe <dx> <startY>
    int s2 = arg.indexOf(' ');
    onSwipe(arg.substring(0, s2 < 0 ? arg.length() : s2).toInt(), s2 < 0 ? 100 : arg.substring(s2 + 1).toInt());
  } else if (cmd == "bright") {
    settings.bright = constrain(arg.toInt(), 0, 255); settings.save(); M5.Display.setBrightness(settings.bright);
  } else if (cmd == "beep") {
    settings.beep = arg == "on" || arg == "1"; settings.save();
    Serial.printf("beep %s (applies from the next scan)\r\n", settings.beep ? "on" : "off");
  } else if (cmd == "cc") {
    setControlCenter(!ccOpen);
  } else if (cmd == "hotspot") {
    if (arg == "off") stopAp(); else startAp();
  } else if (cmd == "forget") {  // wipe WiFi + server, like a fresh device
    settings.ssid = ""; settings.pass = ""; settings.server = ""; settings.save(); Serial.print("forgotten; rebooting\r\n"); delay(200); ESP.restart();
  } else if (cmd == "shot") {
    dumpShot();
  } else if (cmd == "reboot") {
    ESP.restart();
  } else {
    Serial.printf("unknown command: %s (try help)\r\n", cmd.c_str());
  }
}

void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') { if (line.length()) handleCommand(line); line = ""; }
    else if (line.length() < 200) line += c;
  }
}

// --- Arduino -------------------------------------------------------------

bool makeCanvas(M5Canvas& c) {
  c.setPsram(true);
  c.setColorDepth(16);
  if (c.createSprite(W, H)) return true;
  c.setPsram(false);
  return c.createSprite(W, H);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = false;  // the speaker's I2S DOUT is G13, which the scanner's UART uses
  cfg.internal_mic = true;   // voice naming
  M5.begin(cfg);
  Serial.setTxTimeoutMs(0);  // USB CDC: never block when no host is reading
  M5.Power.setExtOutput(true);  // 5 V to the bus (and so the module) also on battery
  settings.load();
  M5.Display.setBrightness(settings.bright);

  if (!makeCanvas(canvasA) || !makeCanvas(canvasB)) Serial.print("canvas allocation failed\r\n");
  canvasA.setTextWrap(false); canvasB.setTextWrap(false);
  cv.fillSprite(rgb(kBg));
  cv.setFont(F_BIG);
  cv.setTextDatum(textdatum_t::middle_center);
  cv.setTextColor(rgb(kText), rgb(kBg));
  cv.drawString("Pantry", W / 2, 100);
  cv.setFont(F_SMALL);
  cv.setTextColor(rgb(kMuted), rgb(kBg));
  cv.drawString("starting scanner module...", W / 2, 134);
  present();
  oldp->fillSprite(rgb(kBg));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("pantry-tracker");
  staTryMs = millis();
  if (settings.ssid.length()) WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
  else startAp();
  webBegin();

  qrInit();  // ~2 s; WiFi associates meanwhile

  Serial.print("\r\nPantry tracker. Type help.\r\n");
  printStatus();
  dirty = true;
}

void loop() {
  M5.update();
  pollSerial();

  // WiFi came up: first fetch.
  static bool wasUp = false;
  bool up = wifiUp();
  static uint32_t firstFetchAt = 0;
  if (up && !wasUp) {
    Serial.printf("wifi up, ip %s\r\n", WiFi.localIP().toString().c_str());
    firstFetchAt = millis() + 800;  // the first request right after association tends to fail
    MDNS.end(); if (MDNS.begin("pantry-tracker")) MDNS.addService("http", "tcp", 80);
  }
  // Provisioning: web page always; hotspot when the network cannot be joined.
  web.handleClient();
  if (apMode) dns.processNextRequest();
  if (reconnectAt && millis() > reconnectAt) {
    reconnectAt = 0; staTryMs = millis();
    WiFi.disconnect(); WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
    Serial.printf("joining %s\r\n", settings.ssid.c_str());
  }
  if (apMode && up && millis() - apSinceMs > 5000 && serverOk) stopAp();          // configured and working
  if (apMode && up && millis() - apSinceMs > 25000) stopAp();                     // or at least on the network
  if (!apMode && !up && settings.ssid.length() && millis() - staTryMs > 45000 && screen == Screen::Home) startAp();
  if (!up && wasUp) { serverOk = false; dirty = true; }
  wasUp = up;
  if (firstFetchAt && up && millis() > firstFetchAt) {
    firstFetchAt = 0;
    bool ok = fetchPantries() && fetchItems();
    if (!ok) { delay(1500); fetchPantries(); fetchItems(); }
    Serial.printf("first fetch: %s\r\n", serverOk ? "ok" : lastError.c_str());
    if (serverOk) postDevice();
    dirty = true;
  }

  // Touch: taps, vertical flicks (scroll) and horizontal swipes (back / reveal trash).
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) { flickStartX = t.x; flickStartY = t.y; }
  if (t.wasReleased() && flickStartY >= 0) {
    int dx = t.x - flickStartX, dy = t.y - flickStartY, sy = flickStartY;
    flickStartX = flickStartY = -1;
    if (abs(dx) > 40 && abs(dx) > abs(dy)) onSwipe(dx, sy);
    else if (abs(dy) > 25) onFlick(dy);
  }
  if (t.wasClicked()) onTap(t.x, t.y);
  if (ccOpen && t.isPressed() && flickStartY >= kCCSlider.y - 10 && flickStartY < kCCSlider.y + kCCSlider.h + 10) {
    setBrightnessFromX(t.x);  // drag the brightness knob
    flickStartX = t.x;        // and never treat it as a swipe
  }
  // Bezel buttons (the three touch keys under the screen).
  if (M5.BtnB.wasClicked()) { if (screen == Screen::Home) goScan(); else goHome(false); }
  if (M5.BtnA.wasClicked()) onFlick(+40);
  if (M5.BtnC.wasClicked()) onFlick(-40);

  // Scanner.
  if (screen == Screen::Scan) {
    qrService();
    String code;
    if (qrPoll(code)) { Serial.printf("scanned: %s\r\n", code.c_str()); postDevice("scanned", code); goEntry(code); }
    else if (millis() - scanStartedMs > 60000) { toast("Scan timed out"); goHome(false); }
  }

  // Background: periodic refresh on HOME, one thumbnail per pass for visible cards.
  if (screen == Screen::Home && up) {
    if (millis() - lastFetchMs > 60000) { fetchItems(); dirty = true; }
    for (size_t i = 0; i < items.size(); i++) {
      int y = kListTop + (int)i * kCardStep - scrollPx;
      if (y > H || y + 62 < 0) continue;
      if (items[i].hasImage && ensureThumb(items[i].barcode, kThumbList)) { dirty = true; break; }
    }
  }

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 5000) { lastStatus = millis(); dirty = true; }  // battery / wifi indicators
  static uint32_t lastTelemetry = 0;
  if (up && serverOk && screen == Screen::Home && millis() - lastTelemetry > 60000) { lastTelemetry = millis(); postDevice(); }
  if (toastUntil && millis() > toastUntil) { toastUntil = 0; dirty = true; }
  if (screen == Screen::Scan) {  // breathing glow, ~20 fps
    static uint32_t lastFrame = 0;
    if (millis() - lastFrame > 50) { lastFrame = millis(); dirty = true; }
  }
  if (dirty) render();
  delay(5);
}
