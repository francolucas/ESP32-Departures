// S-Bahn departure board: Donnersbergerbrücke -> Vaterstetten
// Hardware: ESP32-C3 Super Mini + 0.96" I2C OLED (SSD1306, 128x64)
// Board: "Nologo ESP32C3 Super Mini", USB CDC On Boot: Enabled (default)
// Wiring: OLED SDA -> GPIO0, SCL -> GPIO1, VCC -> 3.3V, GND -> G
//
// Data: MVG API (www.mvg.de/api/bgw-pt/v3/departures), polled every 30 s.
// Shows the next S-Bahns that actually reach Vaterstetten (S4/S6): filtered by
// destination, since some eastbound S4s terminate at Trudering.

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <time.h>

#include "certs.h"
#include "secrets.h"  // WiFi credentials (see secrets.example.h)

// --------------------------------------------------
// Configuration
// --------------------------------------------------

constexpr uint8_t OLED_SDA = 0;
constexpr uint8_t OLED_SCL = 1;
constexpr uint8_t OLED_ADDRESS = 0x3C;

// Donnersbergerbrücke (find others via /api/bgw-pt/v3/locations?query=...)
const char* STATION_ID = "de:09162:8";
// Title in the yellow header, next to the clock (16 chars max at this font).
const char* HEADER_TITLE = "Departures";

// Every S-Bahn stop from Vaterstetten to the end of the east branch. An
// S-Bahn (S4/S6) whose destination is one of these passes Vaterstetten;
// westbound trains and S4s ending at Trudering/Haar never match.
// Matched as substrings: the API writes e.g. "Grafing-Bahnhof", and "Grafing"
// covers both Grafing Bahnhof and Grafing Stadt.
const char* DESTINATIONS[] = {
    "Vaterstetten", "Baldham", "Zorneding", "Eglharting",
    "Kirchseeon",   "Grafing", "Ebersberg",
};

// A page holds the 20 departures (buses included) with the earliest *planned*
// times among those leaving after the offset, so it only covers ~10-15 minutes
// at this busy station, and less when late trains pile up. We page through the
// timetable until the SHOW_TRAINS next matching trains are certain, stepping
// at most PAGE_STEP_MIN and never past what the previous page covered.
constexpr int SHOW_TRAINS = 3;
constexpr int PAGE_STEP_MIN = 10;
constexpr int LOOKAHEAD_MIN = 120;
constexpr int MAX_PAGES = 20;

// Show built-in sample trains instead of live data, to preview the layout.
constexpr bool DEMO_MODE = false;

constexpr uint32_t FETCH_INTERVAL_MS = 30 * 1000;
constexpr uint32_t STALE_AFTER_S = 3 * 60;  // flag data older than this

const char* TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Berlin
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

// --------------------------------------------------

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

struct Departure {
  char line[4];
  char destination[24];
  time_t planned;  // scheduled departure (epoch seconds), used to de-duplicate
  time_t when;     // real-time departure (epoch seconds)
  int delayMin;
  bool cancelled;
};

constexpr int MAX_DEPARTURES = 8;
Departure departures[MAX_DEPARTURES];
int departureCount = 0;
time_t lastFetchOk = 0;
char statusMsg[32] = "Starting...";
bool fetching = false;  // a refresh is in progress

bool destinationMatches(const char* dest) {
  for (const char* d : DESTINATIONS)
    if (strstr(dest, d)) return true;
  return false;
}

bool clockValid() { return time(nullptr) > 1700000000; }

// Home is the default; the office network is optional (define WIFI_SSID_OFFICE
// in secrets.h). Networks are joined by name, in order, without relying on a
// scan first (scans can miss networks, e.g. hidden ones).
struct WifiNetwork { const char* ssid; const char* password; };
const WifiNetwork WIFI_NETWORKS[] = {
    {WIFI_SSID, WIFI_PASSWORD},
#ifdef WIFI_SSID_OFFICE
    {WIFI_SSID_OFFICE, WIFI_PASSWORD_OFFICE},
#endif
};

constexpr int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;

// The network that worked last time is tried first (kept across reboots), so
// the board doesn't wait for the home network to time out at the office.
Preferences prefs;
int lastWifi = 0;

void setupWifi() {
  prefs.begin("board", false);
  lastWifi = prefs.getUChar("wifi", 0);
  if (lastWifi >= WIFI_NETWORK_COUNT) lastWifi = 0;
}

bool tryWifi(const char* ssid, const char* password, uint32_t timeoutMs) {
  Serial.printf("[WIFI] Trying %s...\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  // ESP32-C3 Super Mini: the onboard antenna is poorly matched and connections
  // fail (status 6) at full TX power. Must be set after begin().
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) return true;
  // 1 = SSID not found, 4 = connect failed (e.g. wrong password), 6 = disconnected
  Serial.printf("[WIFI] %s failed, status %d\n", ssid, WiFi.status());
  // Switch the radio off to really abort this attempt: a plain disconnect
  // leaves it connecting, and the next begin() is then rejected.
  WiFi.disconnect(true, true);
  delay(200);
  return false;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  for (int k = 0; k < WIFI_NETWORK_COUNT; k++) {
    int i = (lastWifi + k) % WIFI_NETWORK_COUNT;
    if (tryWifi(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password, WIFI_TIMEOUT_MS)) {
      Serial.printf("[WIFI] Connected to %s, IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      if (i != lastWifi) {
        lastWifi = i;
        prefs.putUChar("wifi", i);
      }
      return;
    }
  }
  Serial.println("[WIFI] No known network reachable");
}

// Fetches one page of departures and merges matching ones into `out`.
// `http`/`client` are shared across pages so the TLS connection is reused.
// `lastPlanned` is set to the latest planned time on the page (0 if empty):
// trains planned after it may have been cut off.
bool fetchPage(HTTPClient& http, NetworkClientSecure& client, int offsetMin, Departure* out,
               int& n, time_t& lastPlanned) {
  lastPlanned = 0;
  String url = String("https://www.mvg.de/api/bgw-pt/v3/departures?globalId=") + STATION_ID +
               "&limit=20&transportTypes=SBAHN&offsetInMinutes=" + offsetMin;
  if (!http.begin(client, url)) {
    snprintf(statusMsg, sizeof(statusMsg), "HTTP begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char tlsErr[100] = "";
    client.lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("[HTTP] GET +%d min failed: %d %s | TLS: %s | heap free %u, max block %u\n",
                  offsetMin, code, http.errorToString(code).c_str(), tlsErr, ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    snprintf(statusMsg, sizeof(statusMsg), "HTTP %d", code);
    http.end();
    return false;
  }

  // Read the whole body (~8 KB) before parsing: stream-parsing over TLS timed
  // out mid-response on slow reads ("IncompleteInput").
  String body = http.getString();
  http.end();

  // Only keep the fields we use: keeps memory use small.
  JsonDocument filter;
  filter[0]["label"] = true;
  filter[0]["destination"] = true;
  filter[0]["plannedDepartureTime"] = true;
  filter[0]["realtimeDepartureTime"] = true;
  filter[0]["delayInMinutes"] = true;
  filter[0]["cancelled"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[JSON] +%d min: %s (%u bytes)\n", offsetMin, err.c_str(), body.length());
    snprintf(statusMsg, sizeof(statusMsg), "JSON: %s", err.c_str());
    return false;
  }

  for (JsonObject d : doc.as<JsonArray>()) {
    const char* line = d["label"] | "";
    const char* dest = d["destination"] | "";
    time_t planned = (time_t)(d["plannedDepartureTime"].as<int64_t>() / 1000);
    if (planned > lastPlanned) lastPlanned = planned;
    if (!destinationMatches(dest)) continue;

    bool dup = false;
    for (int i = 0; i < n && !dup; i++)
      dup = out[i].planned == planned && strcmp(out[i].line, line) == 0;
    if (dup || n >= MAX_DEPARTURES) continue;

    Departure& dep = out[n++];
    strlcpy(dep.line, line, sizeof(dep.line));
    strlcpy(dep.destination, dest, sizeof(dep.destination));
    dep.planned = planned;
    dep.when = (time_t)(d["realtimeDepartureTime"].as<int64_t>() / 1000);
    dep.delayMin = d["delayInMinutes"] | 0;
    dep.cancelled = d["cancelled"] | false;
  }
  return true;
}

void drawBoard();

// Sort by real-time departure (insertion sort, n is tiny)
void sortDepartures(Departure* found, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && found[j].when < found[j - 1].when; j--) {
      Departure t = found[j]; found[j] = found[j - 1]; found[j - 1] = t;
    }
}

void publish(Departure* found, int n) {
  sortDepartures(found, n);
  memcpy(departures, found, sizeof(Departure) * n);
  departureCount = n;
}

bool fetchDepartures() {
  NetworkClientSecure client;
  client.setCACert(MVG_ROOT_CA);
  HTTPClient http;
  http.setReuse(true);  // keep-alive: one TLS handshake for all pages
  http.setTimeout(15000);
  http.setUserAgent("esp32-departure-board/1.0");

  // On the very first load, show trains as soon as each page arrives. On later
  // refreshes, keep showing the previous list until the new one is complete.
  bool progressive = !lastFetchOk;

  Departure found[MAX_DEPARTURES];
  int n = 0;
  int pages = 0;
  int off = 0;
  while (off <= LOOKAHEAD_MIN && pages < MAX_PAGES) {
    // The API occasionally times out: retry each page once.
    time_t lastPlanned = 0;
    bool ok = fetchPage(http, client, off, found, n, lastPlanned) ||
              fetchPage(http, client, off, found, n, lastPlanned);
    if (!ok) {
      if (pages == 0) return false;  // nothing usable: keep the previous data
      break;                         // partial result is still better than stale
    }
    pages++;
    if (progressive) {
      publish(found, n);
      lastFetchOk = time(nullptr);
      drawBoard();
    }

    // The page is cut off by planned time, so a train planned after
    // `lastPlanned` can be missing even if it leaves before the late trains
    // that are on the page. Start the next page there (1 min margin).
    time_t now = time(nullptr);
    int next = off + PAGE_STEP_MIN;
    if (lastPlanned) {
      int coveredMin = (int)((lastPlanned - now) / 60) - 1;
      if (coveredMin < next) next = coveredMin;
    }
    if (next <= off) next = off + 1;
    off = next;

    // Done once the trains on screen all leave before the next page starts:
    // nothing still unseen can come before them.
    sortDepartures(found, n);
    if (n >= SHOW_TRAINS && found[SHOW_TRAINS - 1].when < now + off * 60) break;
  }
  http.end();
  publish(found, n);

  Serial.printf("[HTTP] %d matching departures (%d pages) | heap free %u, max block %u\n", n,
                pages, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  for (int i = 0; i < n; i++)
    Serial.printf("  %s %-16s in %ld min (+%d)%s\n", departures[i].line, departures[i].destination,
                  (long)(departures[i].when - time(nullptr)) / 60, departures[i].delayMin,
                  departures[i].cancelled ? " CANCELLED" : "");
  return true;
}

// --------------------------------------------------
// Drawing
//
// The 0.96" module is two-colour: rows 0-15 are yellow, rows 16-63 blue, with
// a physical gap (no pixels lost) between them. So: header lives entirely in
// rows 0-15, and the blue area holds 3 departure rows of 16 px each.
// --------------------------------------------------

constexpr int HEADER_H = 16;
constexpr int ROW_H = 16;

void drawCentered(const char* s, int y) {
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(s)) / 2, y, s);
}

// Same font as the departure rows; baseline 11 centres it in the 16 px band.
void drawHeader(const char* right) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(0, 11, HEADER_TITLE);
  if (right) u8g2.drawStr(128 - u8g2.getStrWidth(right), 11, right);
}

void drawMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  drawHeader(nullptr);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCentered(line1, HEADER_H + 20);
  if (line2) drawCentered(line2, HEADER_H + 34);
  u8g2.sendBuffer();
}

// Short display name: the API's "Grafing-Bahnhof" doesn't fit next to the
// time (room for 10 characters), so "Bahnhof" becomes "Bf".
void displayName(const char* dest, char* out, size_t len) {
  strlcpy(out, dest, len);
  char* p = strstr(out, "Bahnhof");
  if (!p || p[7] != '\0') return;
  if (p > out && p[-1] == '-') p[-1] = ' ';
  strcpy(p, "Bf");
}

void drawBoard() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[24];

  u8g2.clearBuffer();

  // Yellow header: departure station + clock. If the data is stale, the clock
  // blinks (no room for an extra marker next to the station name).
  bool stale = !lastFetchOk || now - lastFetchOk > STALE_AFTER_S;
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  drawHeader(stale && (now % 2) ? nullptr : buf);

  // Blue area: up to 3 upcoming departures
  int row = 0;
  for (int i = 0; i < departureCount && row < SHOW_TRAINS; i++) {
    const Departure& d = departures[i];
    if (d.when < now - 30) continue;  // already gone
    long mins = (long)(d.when - now) / 60;
    int top = HEADER_H + row * ROW_H;
    int base = top + 12;

    // Line badge: inverted rounded box with "S4"/"S6"
    u8g2.drawRBox(0, top + 2, 17, 12, 2);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(3, base, d.line);
    u8g2.setDrawColor(1);

    // Right side first (its width decides how much room the name gets):
    // "cancelled", or minutes until the real-time departure (delay included).
    constexpr int NAME_X = 21;
    int nameEnd;
    if (d.cancelled) {
      u8g2.setFont(u8g2_font_5x7_tf);
      const char* s = "cancelled";
      int x = 128 - u8g2.getStrWidth(s);
      u8g2.drawStr(x, base - 1, s);
      nameEnd = x - 2;
    } else {
      if (mins <= 0) snprintf(buf, sizeof(buf), "now");
      else snprintf(buf, sizeof(buf), "%ld min", mins);
      u8g2.setFont(u8g2_font_7x13B_tf);
      int x = 128 - u8g2.getStrWidth(buf);
      u8g2.drawStr(x, base + 1, buf);
      nameEnd = x - 4;
    }

    // Destination, clipped so it never runs into the right-hand column
    char name[24];
    displayName(d.destination, name, sizeof(name));
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setClipWindow(NAME_X, top, nameEnd, top + ROW_H);
    u8g2.drawUTF8(NAME_X, base, name);
    u8g2.setMaxClipWindow();
    if (d.cancelled) {
      int w = u8g2.getUTF8Width(name);
      u8g2.drawHLine(NAME_X, base - 4, w < nameEnd - NAME_X ? w : nameEnd - NAME_X);
    }
    row++;
  }

  if (row == 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    if (fetching && lastFetchOk) {
      drawCentered("Loading...", HEADER_H + 26);
    } else if (lastFetchOk) {
      drawCentered("No trains to", HEADER_H + 20);
      drawCentered("Vaterstetten in 2 h", HEADER_H + 34);
    } else {
      drawCentered(statusMsg, HEADER_H + 26);
    }
  }

  u8g2.sendBuffer();
}

// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[SETUP] S-Bahn board Donnersbergerbruecke -> Vaterstetten");

  u8g2.setI2CAddress(OLED_ADDRESS << 1);
  u8g2.begin();
  u8g2.enableUTF8Print();

  drawMessage("Connecting to WiFi", nullptr);
  setupWifi();
  for (int i = 0; i < 3 && WiFi.status() != WL_CONNECTED; i++) ensureWifi();

  configTzTime(TIME_ZONE, NTP_SERVER_1, NTP_SERVER_2);
  drawMessage("Syncing clock...", nullptr);
  for (int i = 0; i < 40 && !clockValid(); i++) delay(250);
  Serial.println(clockValid() ? "[NTP] Clock synced" : "[NTP] Clock not synced yet");
  drawMessage("Loading departures", nullptr);
}

// Sample trains for previewing the layout (DEMO_MODE). Times are relative to
// now, so they count down like real ones; regenerated on every "fetch".
void loadDemoDepartures() {
  static const struct { const char* line; const char* dest; int inSec; int delay; } demo[] = {
      {"S6", "Ebersberg", 20, 0},          // "now"
      {"S4", "Zorneding", 7 * 60 + 30, 3},  // delayed by 3 min
      {"S6", "Grafing-Bahnhof", 23 * 60 + 30, 0},
  };
  time_t now = time(nullptr);
  int n = 0;
  for (const auto& d : demo) {
    Departure& dep = departures[n++];
    strlcpy(dep.line, d.line, sizeof(dep.line));
    strlcpy(dep.destination, d.dest, sizeof(dep.destination));
    dep.when = now + d.inSec;
    dep.planned = dep.when - d.delay * 60;
    dep.delayMin = d.delay;
    dep.cancelled = false;
  }
  departureCount = n;
}

// Self-healing: the ESP32 network stack can get stuck after a WiFi hiccup,
// failing every request ("connection refused") until reset. Reconnect WiFi
// after 3 failed refreshes in a row (~1.5 min), reboot after 10 (~5 min).
int consecutiveFailures = 0;

void onFetchFailed() {
  consecutiveFailures++;
  IPAddress ip;
  bool dns = WiFi.status() == WL_CONNECTED && WiFi.hostByName("www.mvg.de", ip);
  Serial.printf("[NET] Refresh failed (%d in a row), WiFi %s, DNS %s\n", consecutiveFailures,
                WiFi.status() == WL_CONNECTED ? "up" : "down",
                dns ? ip.toString().c_str() : "FAILED");
  if (consecutiveFailures >= 10) {
    Serial.println("[NET] Still failing: restarting");
    delay(100);
    ESP.restart();
  }
  if (consecutiveFailures % 3 == 0) {
    Serial.println("[NET] Reconnecting WiFi");
    WiFi.disconnect();
    delay(500);
    ensureWifi();
  }
}

void loop() {
  static uint32_t lastFetchAttempt = 0;
  static bool firstFetch = true;

  if (firstFetch || millis() - lastFetchAttempt >= FETCH_INTERVAL_MS) {
    firstFetch = false;
    lastFetchAttempt = millis();
    ensureWifi();
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(statusMsg, sizeof(statusMsg), "No WiFi");
      onFetchFailed();
    } else if (!clockValid()) {
      snprintf(statusMsg, sizeof(statusMsg), "Clock not synced");
    } else if (DEMO_MODE) {
      loadDemoDepartures();
      lastFetchOk = time(nullptr);
    } else {
      fetching = true;
      if (fetchDepartures()) {
        lastFetchOk = time(nullptr);
        consecutiveFailures = 0;
      } else {
        onFetchFailed();
      }
      fetching = false;
    }
  }

  if (clockValid()) drawBoard();
  else drawMessage(statusMsg, nullptr);
  delay(1000);
}
