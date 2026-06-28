#define BLYNK_TEMPLATE_ID "TMPL3Rt8xa32H"
#define BLYNK_TEMPLATE_NAME "test"
#define BLYNK_AUTH_TOKEN "m75I_c0CplhRFnoQ398bwPbeob7AJD_V"
#define BLYNK_PRINT Serial

#include <BlynkSimpleEsp32.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>

static const uint8_t RELAY_COUNT = 16;
static const int RELAY_PINS[RELAY_COUNT] = {
  2, 4, 5, 12, 13, 14, 15, 16,
  17, 18, 19, 21, 22, 23, 25, 26
};
static const char *DEFAULT_RELAY_NAMES[RELAY_COUNT] = {
  "Hall Fan", "AC", "Living Light", "Bedroom Light",
  "Kitchen Fan", "Gate Light", "Water Pump", "Heater",
  "TV", "Fridge", "Washing Machine", "Socket 1",
  "Socket 2", "Curtain", "Garden Motor", "Spare"
};
static const bool RELAY_ACTIVE_LOW = false;

const char *AP_SSID = "ESP32-Home-Setup";
const char *AP_PASS = "12345678";
const char *LOGIN_UID = "admin";
const char *LOGIN_PASS = "admin@123";
const char *SESSION_COOKIE_NAME = "ESPSESS";
const uint8_t MAX_ACTIVE_SESSIONS = 8;
const byte DNS_PORT = 53;
const char *MDNS_HOST = "esp32";
const long TZ_OFFSET_SEC = 19800;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

bool apModeActive = false;
bool staConnected = false;
String staIP = "";
String staSSID = "";
String activeSessionTokens[MAX_ACTIVE_SESSIONS];

String relayNames[RELAY_COUNT];
bool includeInOnAll[RELAY_COUNT];
bool includeInOffAll[RELAY_COUNT];

struct RelaySchedule {
  bool enabled;
  uint16_t onMin;
  uint16_t offMin;
};
RelaySchedule schedules[RELAY_COUNT];

uint32_t holdUntilMs[RELAY_COUNT];
uint32_t relayOnStartEpoch[RELAY_COUNT];
uint16_t todayMinutes[RELAY_COUNT];
uint16_t yesterdayMinutes[RELAY_COUNT];
bool lastRelayState[RELAY_COUNT];
int currentYDay = -1;

bool timeReady = false;
time_t manualEpochBase = 0;
uint32_t manualEpochMillis = 0;
int lastScheduleMinute = -1;
static const uint8_t TREND_DAYS = 6;
uint16_t dailyTotalHistory[TREND_DAYS];
uint16_t todayTotalCache = 0;
String dailyLabelHistory[TREND_DAYS];
uint16_t remoteTurnOnCountToday = 0;
const uint32_t DASH_STATE_SIGNATURE = 0x484D4442;
const uint32_t DASH_SAVE_INTERVAL_MS = 60000;
uint32_t lastDashSaveMs = 0;
bool dashStateDirty = false;

bool blynkConnected = false;
unsigned long lastBlynkSync = 0;
const unsigned long BLYNK_SYNC_INTERVAL = 3000;

enum RelayChangeSource : uint8_t {
  SRC_REMOTE = 0,
  SRC_SCHEDULE = 1,
  SRC_TIMER = 2,
  SRC_BULK = 3,
  SRC_BLYNK = 5,
  SRC_UNKNOWN = 4
};

static const uint8_t EVENT_HISTORY_MAX = 180;
uint32_t eventEpoch[EVENT_HISTORY_MAX];
uint8_t eventChannel[EVENT_HISTORY_MAX];
bool eventState[EVENT_HISTORY_MAX];
uint8_t eventSource[EVENT_HISTORY_MAX];
uint8_t eventCount = 0;
uint8_t eventNextWrite = 0;

struct StoredEventRecord {
  uint32_t epoch;
  uint8_t channel;
  uint8_t state;
  uint8_t source;
  uint8_t reserved;
};

void markDashStateDirty() { dashStateDirty = true; }

void saveDashState(bool force = false) {
  uint32_t nowMs = millis();
  if (!force) {
    if (!dashStateDirty) return;
    if ((uint32_t)(nowMs - lastDashSaveMs) < DASH_SAVE_INTERVAL_MS) return;
  }
  preferences.begin("dash", false);
  preferences.putUInt("sig", DASH_STATE_SIGNATURE);
  preferences.putInt("yday", currentYDay);
  preferences.putBytes("today", todayMinutes, sizeof(todayMinutes));
  preferences.putBytes("yest", yesterdayMinutes, sizeof(yesterdayMinutes));
  preferences.putBytes("trend", dailyTotalHistory, sizeof(dailyTotalHistory));
  for (uint8_t i = 0; i < TREND_DAYS; i++) {
    String key = "lb" + String(i);
    preferences.putString(key.c_str(), dailyLabelHistory[i]);
  }
  preferences.putUShort("rmon", remoteTurnOnCountToday);
  preferences.putUChar("evcnt", eventCount);
  preferences.putUChar("evnxt", eventNextWrite);
  StoredEventRecord records[EVENT_HISTORY_MAX];
  for (uint16_t i = 0; i < EVENT_HISTORY_MAX; i++) {
    records[i].epoch = eventEpoch[i];
    records[i].channel = eventChannel[i];
    records[i].state = eventState[i] ? 1 : 0;
    records[i].source = eventSource[i];
    records[i].reserved = 0;
  }
  preferences.putBytes("evrec", records, sizeof(records));
  preferences.end();
  dashStateDirty = false;
  lastDashSaveMs = nowMs;
}

void loadDashState() {
  preferences.begin("dash", true);
  uint32_t sig = preferences.getUInt("sig", 0);
  if (sig != DASH_STATE_SIGNATURE) {
    preferences.end();
    return;
  }
  currentYDay = preferences.getInt("yday", currentYDay);
  if (preferences.getBytesLength("today") == sizeof(todayMinutes)) {
    preferences.getBytes("today", todayMinutes, sizeof(todayMinutes));
  }
  if (preferences.getBytesLength("yest") == sizeof(yesterdayMinutes)) {
    preferences.getBytes("yest", yesterdayMinutes, sizeof(yesterdayMinutes));
  }
  if (preferences.getBytesLength("trend") == sizeof(dailyTotalHistory)) {
    preferences.getBytes("trend", dailyTotalHistory, sizeof(dailyTotalHistory));
  }
  for (uint8_t i = 0; i < TREND_DAYS; i++) {
    String key = "lb" + String(i);
    String v = preferences.getString(key.c_str(), dailyLabelHistory[i]);
    dailyLabelHistory[i] = v.length() ? v : "-";
  }
  remoteTurnOnCountToday = preferences.getUShort("rmon", remoteTurnOnCountToday);
  uint8_t loadedCount = preferences.getUChar("evcnt", 0);
  uint8_t loadedNext = preferences.getUChar("evnxt", 0);
  eventCount = (loadedCount > EVENT_HISTORY_MAX) ? EVENT_HISTORY_MAX : loadedCount;
  eventNextWrite = (loadedNext >= EVENT_HISTORY_MAX) ? 0 : loadedNext;
  if (preferences.getBytesLength("evrec") == sizeof(StoredEventRecord) * EVENT_HISTORY_MAX) {
    StoredEventRecord records[EVENT_HISTORY_MAX];
    preferences.getBytes("evrec", records, sizeof(records));
    for (uint16_t i = 0; i < EVENT_HISTORY_MAX; i++) {
      eventEpoch[i] = records[i].epoch;
      eventChannel[i] = (records[i].channel < RELAY_COUNT) ? records[i].channel : 0;
      eventState[i] = (records[i].state == 1);
      eventSource[i] = (records[i].source <= SRC_UNKNOWN) ? records[i].source : SRC_UNKNOWN;
    }
  }
  preferences.end();
  dashStateDirty = false;
  lastDashSaveMs = millis();
}

void setRelay(uint8_t idx, bool on) {
  if (idx >= RELAY_COUNT) return;
  int level = on ? (RELAY_ACTIVE_LOW ? LOW : HIGH) : (RELAY_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(RELAY_PINS[idx], level);
}

bool getRelay(uint8_t idx) {
  if (idx >= RELAY_COUNT) return false;
  int raw = digitalRead(RELAY_PINS[idx]);
  return RELAY_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

const char *sourceLabel(RelayChangeSource source) {
  switch (source) {
    case SRC_REMOTE: return "Remote";
    case SRC_SCHEDULE: return "Schedule";
    case SRC_TIMER: return "Timer";
    case SRC_BULK: return "Bulk";
    case SRC_BLYNK: return "Blynk App";
    default: return "System";
  }
}

void pushHistoryEvent(uint8_t idx, bool on, RelayChangeSource source) {
  if (idx >= RELAY_COUNT) return;
  eventEpoch[eventNextWrite] = (uint32_t)currentEpoch();
  eventChannel[eventNextWrite] = idx;
  eventState[eventNextWrite] = on;
  eventSource[eventNextWrite] = (uint8_t)source;
  eventNextWrite = (eventNextWrite + 1) % EVENT_HISTORY_MAX;
  if (eventCount < EVENT_HISTORY_MAX) eventCount++;
  markDashStateDirty();
}

void setRelayWithSource(uint8_t idx, bool on, RelayChangeSource source) {
  if (idx >= RELAY_COUNT) return;
  bool previous = getRelay(idx);
  setRelay(idx, on);
  if (previous != on) {
    if (on && source == SRC_REMOTE) {
      remoteTurnOnCountToday++;
      markDashStateDirty();
    }
    pushHistoryEvent(idx, on, source);
    if (blynkConnected) {
      Blynk.virtualWrite(idx, on ? 255 : 0);
    }
  }
}

BLYNK_WRITE_DEFAULT() {
  int pin = request.pin;
  int value = param.asInt();
  if (pin >= 0 && pin < RELAY_COUNT) {
    uint8_t relayIdx = pin;
    bool newState = (value > 0);
    if (getRelay(relayIdx) != newState) {
      setRelayWithSource(relayIdx, newState, SRC_BLYNK);
      if (!newState) holdUntilMs[relayIdx] = 0;
    }
  }
}

BLYNK_CONNECTED() {
  blynkConnected = true;
  Serial.println("Blynk connected!");
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    Blynk.syncVirtual(i);
  }
}

void syncBlynkWithSystem() {
  if (!blynkConnected) return;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    Blynk.virtualWrite(i, getRelay(i) ? 255 : 0);
  }
}

String htmlEscape(const String &in) {
  String out = in;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

String jsonEscape(const String &in) {
  String out = in;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  return out;
}

String wifiStrengthLabel(int32_t rssi) {
  if (rssi >= -55) return "Excellent";
  if (rssi >= -67) return "Good";
  if (rssi >= -75) return "Fair";
  if (rssi >= -85) return "Weak";
  return "Very Weak";
}

uint16_t hhmmToMin(const String &v) {
  if (v.length() < 5 || v.charAt(2) != ':') return 0;
  int hh = v.substring(0, 2).toInt();
  int mm = v.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return 0;
  return (uint16_t)(hh * 60 + mm);
}

String minToHHMM(uint16_t total) {
  total %= 1440;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", total / 60, total % 60);
  return String(buf);
}

time_t currentEpoch() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    timeReady = true;
    return now;
  }
  if (manualEpochBase > 0) {
    timeReady = true;
    return manualEpochBase + ((millis() - manualEpochMillis) / 1000);
  }
  timeReady = false;
  return 0;
}

String currentTimeString() {
  time_t now = currentEpoch();
  if (now <= 0) return "Time not set";
  struct tm t;
  localtime_r(&now, &t);
  char out[24];
  strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &t);
  return String(out);
}

String cookieValue(const String &cookieHeader, const String &key) {
  String pattern = key + "=";
  int start = cookieHeader.indexOf(pattern);
  if (start < 0) return "";
  start += pattern.length();
  int end = cookieHeader.indexOf(';', start);
  if (end < 0) end = cookieHeader.length();
  String value = cookieHeader.substring(start, end);
  value.trim();
  return value;
}

String makeSessionToken() {
  return String((uint32_t)millis(), HEX) + String((uint32_t)random(0x7FFFFFFF), HEX);
}

bool hasSessionToken(const String &token) {
  if (token.length() == 0) return false;
  for (uint8_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
    if (activeSessionTokens[i] == token) return true;
  }
  return false;
}

void addSessionToken(const String &token) {
  if (token.length() == 0) return;
  for (uint8_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
    if (activeSessionTokens[i].length() == 0) {
      activeSessionTokens[i] = token;
      return;
    }
  }
  for (uint8_t i = 1; i < MAX_ACTIVE_SESSIONS; i++) {
    activeSessionTokens[i - 1] = activeSessionTokens[i];
  }
  activeSessionTokens[MAX_ACTIVE_SESSIONS - 1] = token;
}

void removeSessionToken(const String &token) {
  if (token.length() == 0) return;
  for (uint8_t i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
    if (activeSessionTokens[i] == token) {
      activeSessionTokens[i] = "";
      return;
    }
  }
}

bool isAuthenticated() {
  if (!server.hasHeader("Cookie")) return false;
  String token = cookieValue(server.header("Cookie"), SESSION_COOKIE_NAME);
  return hasSessionToken(token);
}

bool requireAuth(bool jsonResponse = false) {
  if (isAuthenticated()) return true;
  if (jsonResponse) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
  } else {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
  }
  return false;
}

void loadRelayNames() {
  preferences.begin("names", true);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String key = "n" + String(i);
    relayNames[i] = preferences.getString(key.c_str(), DEFAULT_RELAY_NAMES[i]);
    if (relayNames[i].length() == 0) relayNames[i] = DEFAULT_RELAY_NAMES[i];
  }
  preferences.end();
}

void saveRelayName(uint8_t ch, const String &name) {
  if (ch >= RELAY_COUNT) return;
  relayNames[ch] = name;
  preferences.begin("names", false);
  String key = "n" + String(ch);
  preferences.putString(key.c_str(), relayNames[ch]);
  preferences.end();
}

void loadBulkConfig() {
  preferences.begin("bulk", true);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String kOn = "on" + String(i);
    String kOff = "off" + String(i);
    includeInOnAll[i] = preferences.getBool(kOn.c_str(), true);
    includeInOffAll[i] = preferences.getBool(kOff.c_str(), true);
  }
  preferences.end();
}

void saveBulkConfig() {
  preferences.begin("bulk", false);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String kOn = "on" + String(i);
    String kOff = "off" + String(i);
    preferences.putBool(kOn.c_str(), includeInOnAll[i]);
    preferences.putBool(kOff.c_str(), includeInOffAll[i]);
  }
  preferences.end();
}

void loadSchedules() {
  preferences.begin("sched", true);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String kEn = "en" + String(i);
    String kOn = "on" + String(i);
    String kOff = "off" + String(i);
    schedules[i].enabled = preferences.getBool(kEn.c_str(), false);
    schedules[i].onMin = preferences.getUShort(kOn.c_str(), 480);
    schedules[i].offMin = preferences.getUShort(kOff.c_str(), 1320);
  }
  preferences.end();
}

void saveSchedules() {
  preferences.begin("sched", false);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String kEn = "en" + String(i);
    String kOn = "on" + String(i);
    String kOff = "off" + String(i);
    preferences.putBool(kEn.c_str(), schedules[i].enabled);
    preferences.putUShort(kOn.c_str(), schedules[i].onMin);
    preferences.putUShort(kOff.c_str(), schedules[i].offMin);
  }
  preferences.end();
}

void rotateDayIfNeeded() {
  time_t now = currentEpoch();
  if (now <= 0) return;
  struct tm t;
  localtime_r(&now, &t);
  if (currentYDay == -1) {
    currentYDay = t.tm_yday;
    return;
  }
  if (t.tm_yday != currentYDay) {
    uint16_t finishedDayTotal = 0;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
      finishedDayTotal += todayMinutes[i];
    }
    for (uint8_t i = 1; i < TREND_DAYS; i++) {
      dailyTotalHistory[i - 1] = dailyTotalHistory[i];
      dailyLabelHistory[i - 1] = dailyLabelHistory[i];
    }
    time_t prevDayEpoch = now - 86400;
    struct tm prevDay;
    localtime_r(&prevDayEpoch, &prevDay);
    char label[8];
    strftime(label, sizeof(label), "%b %d", &prevDay);
    dailyTotalHistory[TREND_DAYS - 1] = finishedDayTotal;
    dailyLabelHistory[TREND_DAYS - 1] = String(label);
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
      yesterdayMinutes[i] = todayMinutes[i];
      todayMinutes[i] = 0;
    }
    remoteTurnOnCountToday = 0;
    currentYDay = t.tm_yday;
    markDashStateDirty();
    saveDashState(true);
  }
}

void updateRelayUsageStats() {
  time_t now = currentEpoch();
  rotateDayIfNeeded();
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    bool state = getRelay(i);
    if (state && !lastRelayState[i]) {
      relayOnStartEpoch[i] = (uint32_t)now;
    } else if (!state && lastRelayState[i]) {
      if (relayOnStartEpoch[i] > 0 && now > 0) {
        uint32_t diff = (uint32_t)now - relayOnStartEpoch[i];
        todayMinutes[i] += (uint16_t)(diff / 60);
        markDashStateDirty();
      }
      relayOnStartEpoch[i] = 0;
    }
    if (state && relayOnStartEpoch[i] > 0 && now > 0) {
      uint32_t diff = (uint32_t)now - relayOnStartEpoch[i];
      uint16_t liveMin = (uint16_t)(diff / 60);
      if (liveMin > 0) {
        relayOnStartEpoch[i] = (uint32_t)now;
        todayMinutes[i] += liveMin;
        markDashStateDirty();
      }
    }
    lastRelayState[i] = state;
  }
}

void applySchedules() {
  time_t now = currentEpoch();
  if (now <= 0) return;
  struct tm t;
  localtime_r(&now, &t);
  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  if (minuteOfDay == lastScheduleMinute) return;
  lastScheduleMinute = minuteOfDay;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (!schedules[i].enabled) continue;
    if (minuteOfDay == schedules[i].onMin) setRelayWithSource(i, true, SRC_SCHEDULE);
    if (minuteOfDay == schedules[i].offMin) setRelayWithSource(i, false, SRC_SCHEDULE);
  }
}

void applyHoldTimers() {
  uint32_t nowMs = millis();
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (holdUntilMs[i] > 0 && (int32_t)(nowMs - holdUntilMs[i]) >= 0) {
      setRelayWithSource(i, false, SRC_TIMER);
      holdUntilMs[i] = 0;
    }
  }
}

bool connectToWiFi(const String &ssid, const String &pass, uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    staSSID = WiFi.SSID();
    staIP = WiFi.localIP().toString();
    configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    return true;
  }
  staConnected = false;
  staSSID = "";
  staIP = "";
  return false;
}

void startAPMode() {
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP(AP_SSID, AP_PASS)) {
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    apModeActive = true;
  }
}

void stopAPMode() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apModeActive = false;
}

void saveCredentials(const String &ssid, const String &pass) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
}

void clearCredentials() {
  preferences.begin("wifi", false);
  preferences.remove("ssid");
  preferences.remove("pass");
  preferences.end();
}

void loadCredentials(String &ssid, String &pass) {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();
}

void ensureMDNS() {
  if (staConnected) {
    MDNS.end();
    MDNS.begin(MDNS_HOST);
    MDNS.addService("http", "tcp", 80);
  }
}

void setupRelays() {
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    setRelay(i, false);
    holdUntilMs[i] = 0;
    relayOnStartEpoch[i] = 0;
    todayMinutes[i] = 0;
    yesterdayMinutes[i] = 0;
    lastRelayState[i] = false;
  }
  for (uint8_t i = 0; i < TREND_DAYS; i++) {
    dailyTotalHistory[i] = 0;
    dailyLabelHistory[i] = "-";
  }
  remoteTurnOnCountToday = 0;
  eventCount = 0;
  eventNextWrite = 0;
}


String baseStyles() {
  return R"rawliteral(
<style>
  :root{--bg:#0b1220;--bg2:#1e293b;--card:#0f172a;--line:#334155;--text:#e2e8f0;--muted:#94a3b8;--accent:#22d3ee}
  *{box-sizing:border-box}
  body{
    margin:0;font-family:Segoe UI,Arial,sans-serif;background:linear-gradient(150deg,var(--bg),var(--bg2));color:var(--text);
    animation:fadeBody .45s ease;
  }
  .top{position:sticky;top:0;z-index:10;display:flex;align-items:center;justify-content:space-between;padding:12px 14px;background:#020617dd;border-bottom:1px solid var(--line);backdrop-filter:blur(8px);transition:background .25s ease,border-color .25s ease}
  .menuBtn{background:transparent;border:1px solid var(--line);border-radius:10px;color:var(--text);font-size:18px;padding:7px 11px;cursor:pointer;transition:transform .18s ease,background .2s ease,border-color .2s ease}
  .menuBtn:hover{background:#1e293b;transform:translateY(-1px)}
  .menuBtn:active{transform:scale(.97)}
  .topBrand{display:flex;align-items:center;gap:10px}
  .topLogo{width:34px;height:34px;border-radius:50%;display:flex;align-items:center;justify-content:center;background:linear-gradient(135deg,#22d3ee,#818cf8);color:#02131f;box-shadow:0 0 12px #22d3ee66}
  .title{
    font-family:"Trebuchet MS","Segoe UI",Arial,sans-serif;
    font-weight:800;
    font-size:1.75rem;
    letter-spacing:.5px;
    line-height:1;
    background:linear-gradient(90deg,#67e8f9,#a78bfa,#f0abfc);
    -webkit-background-clip:text;
    -webkit-text-fill-color:transparent;
    text-shadow:0 0 20px #67e8f944;
  }
  .sub{color:var(--muted);font-size:.9rem}
  .layout{display:flex;min-height:calc(100vh - 56px)}
  .sidebar{width:250px;background:#020617;border-right:1px solid var(--line);padding:14px;transition:none}
  .navBtn{display:block;width:100%;text-align:left;margin-bottom:8px;padding:10px;border-radius:10px;border:1px solid var(--line);background:#0f172a;color:var(--text);cursor:pointer;transition:transform .18s ease,background .2s ease,border-color .2s ease}
  .navBtn:hover{transform:translateX(2px);background:#132033}
  .navBtn.active{background:#0b2a3a;border-color:#22d3ee}
  .main{flex:1;padding:14px}
  .page{display:none}
  .page.active{display:block;animation:pageIn .35s ease}
  .card{
    background:#0f172acc;border:1px solid var(--line);border-radius:14px;padding:12px;margin-bottom:12px;
    transition:transform .24s ease,box-shadow .24s ease,border-color .24s ease,background .24s ease;
    animation:cardIn .35s ease both;
  }
  .card:hover{transform:translateY(-1px);box-shadow:0 8px 20px #00000033}
  .grid{display:grid;gap:10px;grid-template-columns:repeat(1,minmax(0,1fr))}
  @media(min-width:800px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
  @media(min-width:1100px){.grid.relays{grid-template-columns:repeat(4,minmax(0,1fr))}}
  .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
  input,select{background:#0b1220;border:1px solid var(--line);color:var(--text);padding:9px;border-radius:10px;transition:border-color .2s ease,box-shadow .2s ease,background .2s ease}
  input:focus,select:focus{outline:none;border-color:#22d3ee;box-shadow:0 0 0 3px #22d3ee22;background:#0d1626}
  input[type='text'],input[type='time'],input[type='number'],select{width:100%}
  .btn{background:linear-gradient(135deg,#22d3ee,#818cf8);border:0;color:#00111a;font-weight:700;padding:9px 12px;border-radius:10px;cursor:pointer;transition:transform .18s ease,box-shadow .2s ease,filter .2s ease}
  .btn2{background:#1e293b;border:1px solid var(--line);color:var(--text);padding:9px 12px;border-radius:10px;cursor:pointer;transition:transform .18s ease,box-shadow .2s ease,background .2s ease}
  .btn:hover,.btn2:hover{transform:translateY(-1px);box-shadow:0 8px 16px #00000035}
  .btn:active,.btn2:active{transform:scale(.98)}
  .relay{display:flex;justify-content:space-between;align-items:center;gap:8px}
  .switch{position:relative;width:52px;height:28px;display:inline-block}.switch input{display:none}
  .slider{position:absolute;inset:0;border-radius:999px;background:#334155}.slider:before{content:"";position:absolute;left:3px;top:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.2s}
  .switch input:checked + .slider{background:#06b6d4}.switch input:checked + .slider:before{transform:translateX(24px)}
  .barWrap{background:#1e293b;border:1px solid var(--line);height:14px;border-radius:999px;overflow:hidden}
  .barOn{height:100%;background:#22c55e}.barOff{height:100%;background:#ef4444}
  .closeMobile{display:none}
  body.lock-scroll{overflow:hidden;touch-action:none}
  #mobileOverlay{
    position:fixed;inset:56px 0 0 0;z-index:11;
    background:rgba(2,6,23,.25);
    backdrop-filter:blur(4px);
    opacity:0;pointer-events:none;
    transition:opacity .24s ease;
  }
  #mobileOverlay.show{opacity:1;pointer-events:auto}
  @keyframes fadeBody{from{opacity:.55}to{opacity:1}}
  @keyframes pageIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
  @keyframes cardIn{from{opacity:0;transform:translateY(10px) scale(.99)}to{opacity:1;transform:translateY(0) scale(1)}}
  @media(max-width:900px){
    .sidebar{
      position:fixed;left:0;top:56px;bottom:0;z-index:12;
      display:block;
      transform:translateX(-108%);
      opacity:.98;
      transition:transform .28s ease;
    }
    .sidebar.open{transform:translateX(0)}
    body.nav-open .main{filter:blur(4px);transition:filter .24s ease}
    .top{padding:10px 12px}
    .title{font-size:1.5rem}
  }
</style>
)rawliteral";
}

void handleLoginPage() {
  if (isAuthenticated()) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>HomeLink Login</title>
<style>:root{--bg:#060d1f;--bg2:#172554;--card:#0f172aee;--line:#334155;--text:#e2e8f0;--muted:#94a3b8;--a:#22d3ee;--b:#a78bfa;}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:14px;font-family:Segoe UI,Arial,sans-serif;color:var(--text);background:radial-gradient(circle at 15% 10%,#0ea5e988,transparent 35%),radial-gradient(circle at 85% 80%,#8b5cf677,transparent 40%),linear-gradient(155deg,var(--bg),var(--bg2));}.card{width:min(420px,100%);border:1px solid var(--line);border-radius:18px;padding:20px;background:var(--card);box-shadow:0 18px 40px #00000066, inset 0 1px 0 #ffffff0a;backdrop-filter:blur(8px);}.brand{display:flex;align-items:center;gap:12px;margin-bottom:10px}.logo{width:44px;height:44px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:20px;background:linear-gradient(140deg,var(--a),var(--b));color:#04111f;box-shadow:0 0 20px #22d3ee55;}h1{margin:0;font-size:1.5rem}.sub{color:var(--muted);font-size:.92rem;margin-bottom:16px}label{display:block;font-size:.86rem;color:#cbd5e1;margin:10px 0 6px}input{width:100%;background:#0b1220;border:1px solid var(--line);color:var(--text);padding:10px;border-radius:10px;transition:border-color .2s ease,box-shadow .2s ease;}input:focus{outline:none;border-color:#22d3ee;box-shadow:0 0 0 3px #22d3ee22}.btn{width:100%;margin-top:14px;border:0;border-radius:10px;padding:11px;font-weight:800;cursor:pointer;color:#02131f;background:linear-gradient(135deg,var(--a),var(--b));transition:transform .15s ease,box-shadow .2s ease;}.btn:hover{transform:translateY(-1px);box-shadow:0 10px 20px #00000035}.btn:active{transform:scale(.98)}.msg{min-height:20px;margin-top:10px;color:#fca5a5;font-size:.9rem}.ok{color:#86efac}</style></head><body><div class="card"><div class="brand"><div class="logo">&#128274;</div><h1>HomeLink Login</h1></div><div class="sub">Sign in to access setup and control dashboard.</div><form id="f"><label>User ID</label><input id="uid" name="uid" type="text" placeholder="Enter User ID" autocomplete="username" required><label>Password</label><input id="pass" name="pass" type="password" placeholder="Enter Password" autocomplete="current-password" required><button class="btn" type="submit">Sign In</button></form><div id="msg" class="msg"></div></div><script>document.getElementById('f').addEventListener('submit', async (e)=>{e.preventDefault();const uid=document.getElementById('uid').value.trim();const pass=document.getElementById('pass').value;const msg=document.getElementById('msg');msg.textContent='Checking...';msg.classList.remove('ok');try{const r=await fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`uid=${encodeURIComponent(uid)}&pass=${encodeURIComponent(pass)}`});const j=await r.json();if(j.ok){msg.textContent='Login successful. Opening dashboard...';msg.classList.add('ok');setTimeout(()=>location.href='/',350);}else{msg.textContent=j.error||'Invalid credentials';}}catch(e){msg.textContent='Request failed';}});</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleLoginSubmit() {
  if (!server.hasArg("uid") || !server.hasArg("pass")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"uid and pass required\"}");
    return;
  }
  String uid = server.arg("uid");
  String pass = server.arg("pass");
  if (uid == LOGIN_UID && pass == LOGIN_PASS) {
    String token = makeSessionToken();
    addSessionToken(token);
    server.sendHeader("Set-Cookie", String(SESSION_COOKIE_NAME) + "=" + token + "; Path=/; HttpOnly; SameSite=Lax");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  server.send(401, "application/json", "{\"ok\":false,\"error\":\"Invalid User ID or Password\"}");
}

void handleLogout() {
  if (server.hasHeader("Cookie")) {
    String token = cookieValue(server.header("Cookie"), SESSION_COOKIE_NAME);
    removeSessionToken(token);
  }
  server.sendHeader("Set-Cookie", String(SESSION_COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  server.sendHeader("Location", "/login", true);
  server.send(302, "text/plain", "");
}

String renderSetupPage() {
  String html = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Setup</title>
)rawliteral";
  html += baseStyles();
  html += R"rawliteral(
</head><body>
<div class="top"><div class="title">Smart Home Setup</div><div class="sub">AP: ESP32-Home-Setup</div></div>
<div class="main">
  <div class="card">
    <div class="sub">1) Connect phone/PC to ESP32-Home-Setup</div>
    <div class="sub">2) Open esp32.local or 192.168.4.1</div>
    <div class="sub">3) Select Wi-Fi and connect</div>
  </div>
  <div class="card">
    <h3>Available Networks</h3>
    <div id="nets" class="sub">Scanning...</div>
    <div class="row"><div style="flex:1"><label>SSID</label><input id="ssid" type="text"></div></div>
    <div class="row"><div style="flex:1"><label>Password</label><input id="pass" type="password"></div></div>
    <div class="row">
      <button class="btn" onclick="connectWiFi()">Connect Wi-Fi</button>
      <button class="btn2" onclick="scan()">Rescan</button>
    </div>
    <div id="msg" class="sub"></div>
  </div>
</div>
<script>
async function scan(){
  const box=document.getElementById('nets'); box.textContent='Scanning...';
  try{
    const r=await fetch('/scan'); const j=await r.json();
    if(!j.networks || !j.networks.length){ box.textContent='No networks found'; return; }
    box.innerHTML=j.networks.map(n=>`<div class="card"><b>${n.ssid||'(hidden)'}</b><div class="sub">${n.secure?'Secured':'Open'} | ${n.rssi}</div><button class="btn2" onclick="pick('${(n.ssid||'').replace(/'/g,"\\'")}')">Select</button></div>`).join('');
  }catch(e){ box.textContent='Scan failed'; }
}
function pick(v){ document.getElementById('ssid').value=v; }
async function connectWiFi(){
  const ssid=document.getElementById('ssid').value.trim();
  const pass=document.getElementById('pass').value;
  const msg=document.getElementById('msg');
  if(!ssid){ msg.textContent='Please enter SSID'; return; }
  msg.textContent='Connecting...';
  try{
    const r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`});
    const j=await r.json();
    if(j.ok){ msg.innerHTML=`Connected. IP: <b>${j.ip}</b>. Open http://${j.ip}`; setTimeout(()=>location.href='/',1000); }
    else msg.textContent='Failed: '+(j.error||'Connect failed');
  }catch(e){ msg.textContent='Request failed'; }
}
scan();
</script></body></html>
)rawliteral";
  return html;
}

String renderAppPage() {
  String settingsCards = "";
  String scheduleOptions = "";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    settingsCards += "<div class='card'><div class='remoteTop'><div class='logoBadge'><i id='si" + String(i) + "' class='fa-solid fa-bolt'></i></div><div><div class='sub'>Relay " + String(i + 1) + "</div><b>" + htmlEscape(relayNames[i]) + "</b></div></div><input type='text' id='nm" + String(i) + "' value='" + htmlEscape(relayNames[i]) + "'><div class='row'><button class='btn2' onclick='saveName(" + String(i) + ")'>Save Name</button></div></div>";
    scheduleOptions += "<option value='" + String(i) + "'>" + htmlEscape(relayNames[i]) + "</option>";
  }

  String html = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Home Automation</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.7.2/css/all.min.css">
)rawliteral";
  html += baseStyles();
  html += R"rawliteral(
<style>
  .remoteCard{position:relative;overflow:hidden}
  .remoteCard{transition:transform .22s ease, box-shadow .25s ease, border-color .25s ease}
  .remoteCard.is-on{border-color:#22c55e;box-shadow:0 0 0 1px #22c55e44,0 0 20px #22c55e22;transform:translateY(-1px)}
  .remoteCard.is-off{border-color:#334155;box-shadow:none;transform:translateY(0)}
  .remoteTop{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
  .remoteMain{display:flex;justify-content:space-between;align-items:center;gap:10px}
  .remoteLeft{display:flex;flex-direction:column;align-items:flex-start;gap:8px;min-width:0}
  .nameUnder{display:inline-block;padding-bottom:4px;border-bottom:3px solid #38bdf8;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:150px}
  .remoteCard.is-on .nameUnder{border-bottom-color:#22c55e}
  .remoteCard.is-off .nameUnder{border-bottom-color:#64748b}
  .logoBadge{
    width:72px;height:72px;border-radius:50%;display:flex;align-items:center;justify-content:center;
    background:#1e293b;border:1px solid #334155;font-size:.75rem;font-weight:700;cursor:pointer;
    transition:transform .25s ease, box-shadow .25s ease, background .25s ease, border-color .25s ease;
    outline:none;-webkit-tap-highlight-color:transparent;tap-highlight-color:transparent;user-select:none;
  }
  .logoBadge:focus,.logoBadge:active{outline:none;border-radius:50%}
  .logoBadge i{font-size:30px;color:#93c5fd;transition:color .25s ease}
  .logoBadge:hover{transform:scale(1.05)}
  .logoBadge.is-on{
    transform:scale(1.12);
    border-color:#22c55e;
    background:radial-gradient(circle at 35% 30%, #22c55e, #0f172a 70%);
    box-shadow:0 0 14px #22c55e99, 0 0 30px #22c55e33;
    animation:logoPulse 1.15s ease-in-out infinite;
  }
  .logoBadge.is-on i{color:#ecfeff}
  .logoBadge.is-off{
    border-color:#334155;
    background:radial-gradient(circle at 35% 30%, #334155, #0f172a 70%);
    box-shadow:none;
    animation:none;
  }
  .logoBadge.is-off i{color:#94a3b8}
  .statusLamp{width:12px;height:12px;border-radius:50%;display:inline-block}
  .statusLamp.on{background:#22c55e;box-shadow:0 0 10px #22c55e;animation:lampPulse 1.1s ease-in-out infinite}
  .statusLamp.off{background:#ef4444;box-shadow:0 0 10px #ef4444;animation:none}
  .miniGrid{display:grid;gap:10px;grid-template-columns:repeat(1,minmax(0,1fr))}
  @media(min-width:700px){.miniGrid{grid-template-columns:repeat(2,minmax(0,1fr))}}
  .tiny{font-size:.82rem}
  .plusBtn{width:34px;height:34px;border-radius:50%;padding:0;font-size:20px;line-height:34px;text-align:center}
  .chartCard .barWrap{margin-top:6px}
  .remoteControl{display:flex;justify-content:center;align-items:center;margin-top:8px}
  .chartDonut{width:130px;height:130px;border-radius:50%;margin:8px auto}
  .chartCircle{width:130px;height:130px;border-radius:50%;margin:8px auto;position:relative;background:#1e293b;border:1px solid #334155}
  .chartCircle::after{content:"";position:absolute;inset:18px;border-radius:50%;background:#0f172a}
  .chartCircleFill{position:absolute;inset:0;border-radius:50%}
  .smallTable{width:100%;border-collapse:collapse;font-size:.82rem}
  .smallTable th,.smallTable td{border:1px solid #334155;padding:6px;text-align:center}
  .smallTable th{background:#1e293b}
  .card h3,.card h4{margin:0 0 8px}
  .card b{line-height:1.25}
  .main .card{display:block}
  .switch .slider,.switch .slider:before{transition:all .25s ease}
  .switch input:checked + .slider{box-shadow:0 0 12px #06b6d488}
  @keyframes lampPulse{0%{transform:scale(1)}50%{transform:scale(1.2)}100%{transform:scale(1)}}
  @keyframes logoPulse{0%{transform:scale(1.1)}50%{transform:scale(1.16)}100%{transform:scale(1.1)}}
  .switch{transform:translateZ(0)}
  .switch .slider{background:#334155}
  .switch input:checked + .slider{background:linear-gradient(135deg,#06b6d4,#8b5cf6)}
  .switch .slider:before{box-shadow:0 2px 8px #00000055}
  .remoteCard.animating .logoBadge{animation:clickPop .28s ease}
  .remoteCard.animating .statusLamp{animation:lampPulse .6s ease}
  @keyframes clickPop{0%{transform:scale(1)}45%{transform:scale(1.2)}100%{transform:scale(1.12)}}
</style>
</head><body>
<div class="top">
  <div class="topBrand">
    <div class="topLogo"><i class="fa-solid fa-house-signal"></i></div>
    <div class="title">HomeLink</div>
  </div>
  <button class="menuBtn" onclick="toggleSidebar()">&#9776;</button>
</div>
<div class="layout">
  <div id="mobileOverlay" onclick="toggleSidebar()"></div>
  <aside class="sidebar" id="sidebar">
    <button class="navBtn active" data-page="remote" onclick="showPage('remote')">Remote</button>
    <button class="navBtn" data-page="dashboard" onclick="showPage('dashboard')">Dashboard</button>
    <button class="navBtn" data-page="timer" onclick="showPage('timer')">Timer</button>
    <button class="navBtn" data-page="time" onclick="showPage('time')">Time</button>
    <button class="navBtn" data-page="settings" onclick="showPage('settings')">Settings</button>
  </aside>
  <main class="main">
    <section id="page-remote" class="page active">
      <div class="card">
        <h3>Remote Control</h3>
        <div class="sub">Top-left switch and right logo both can control appliance.</div>
      </div>
      <div class="grid relays" id="remoteCards"></div>
      <div class="card">
        <div class="row" style="justify-content:center">
          <button class="btn" onclick="bulkAction('on')"><i class="fa-solid fa-power-off"></i> ON-ALL</button>
          <button class="btn2" onclick="bulkAction('off')"><i class="fa-solid fa-circle-stop"></i> OFF-ALL</button>
        </div>
      </div>
    </section>

    <section id="page-dashboard" class="page">
      <div class="card">
        <h3>Dashboard</h3>
        <div class="sub">Current device time: <b id="clockText">Loading...</b></div>
        <div class="sub">Network: <b id="dashSsid">-</b> | IP: <b id="dashIp">-</b></div>
        <div class="row">
          <button class="btn" onclick="syncBrowserTime()">Sync Time</button>
          <button class="btn2" onclick="refreshStatus()">Refresh</button>
          <button class="btn2" onclick="openSetup()">Open Setup AP</button>
          <button class="btn2" onclick="resetWiFi()">Forget Wi-Fi</button>
        </div>
      </div>
      <div class="miniGrid">
        <div class="card chartCard">
          <div><b>Graph 1: Appliance Status Pie</b></div>
          <div class="tiny">ON: <span id="onCount">0</span> | OFF: <span id="offCount">0</span></div>
          <div class="chartDonut" id="statusPie"></div>
          <div class="tiny">Set Time: <span id="setTimeCount">0</span> | Running Timer: <span id="timerRunCount">0</span> | Remote ON: <span id="remoteOnCount">0</span></div>
        </div>
        <div class="card chartCard">
          <div><b>Graph 2: Top 3 Electricity Use Today</b></div>
          <div id="topUsageGraph" class="tiny">Loading...</div>
        </div>
        <div class="card chartCard">
          <div><b>Graph 3: Last 6 Days Consumption Trend</b></div>
          <div id="trendLineGraph" class="tiny">Loading...</div>
        </div>
      </div>
      <div class="card">
        <h4>Today Usage Time</h4>
        <div style="overflow:auto;max-height:320px">
          <table class="smallTable">
            <thead><tr><th>Appliances</th><th>Time</th></tr></thead>
            <tbody id="todayUsageBody"><tr><td colspan="2">Loading usage...</td></tr></tbody>
          </table>
        </div>
      </div>
      <div class="card">
        <h4>Appliance ON/OFF History</h4>
        <div class="sub">Recent switching records with source</div>
        <div style="overflow:auto;max-height:320px">
          <table class="smallTable">
            <thead><tr><th>Time</th><th>Appliance</th><th>State</th><th>Source</th></tr></thead>
            <tbody id="historyTableBody"></tbody>
          </table>
        </div>
      </div>
    </section>

    <section id="page-timer" class="page">
      <div class="card">
        <div class="row" style="justify-content:space-between">
          <div>
            <h3>Holding Timer</h3>
            <div class="sub">Set hold time in minutes. Each row is a timer task.</div>
          </div>
          <button class="btn plusBtn" onclick="addHoldRow()" title="Add timer">+</button>
        </div>
        <div id="holdRows" class="miniGrid"></div>
        <div id="holdMsg" class="sub"></div>
      </div>
    </section>

    <section id="page-time" class="page">
      <div class="card">
        <div class="row" style="justify-content:space-between">
          <div>
            <h3>Turn ON / OFF Time</h3>
            <div class="sub">Add multiple schedule rows by location.</div>
          </div>
          <button class="btn plusBtn" onclick="addScheduleRow()" title="Add schedule">+</button>
        </div>
        <div id="scheduleRows" class="miniGrid"></div>
        <div id="schMsg" class="sub"></div>
      </div>
    </section>

    <section id="page-settings" class="page">
      <div class="card"><h3>Settings</h3><div class="sub">Map appliance names to relay channels (example: Hall Fan, Kitchen Fan).</div></div>
      <div class="card">
        <h3>ON-ALL / OFF-ALL Selection</h3>
        <div class="sub">Choose which appliances respond to ON-ALL and OFF-ALL.</div>
        <div id="bulkConfigBox" class="miniGrid"></div>
        <div class="row"><button class="btn" onclick="saveBulkConfig()">Save ON-ALL/OFF-ALL Settings</button></div>
      </div>
      <div class="grid">
)rawliteral";
  html += settingsCards;
  html += R"rawliteral(
      </div>
    </section>
  </main>
</div>

<script>
const relayCount=16;
let latestNames=[];
let latestRelays=[];
let latestOnAllMask=[];
let latestOffAllMask=[];
let latestYesterday=[];
let latestToday=[];
let lastRemoteRenderKey='';
let lastBulkRenderKey='';
let lastYesterdayRenderKey='';
let lastTopUsageRenderKey='';
let lastTrendKey='';
let lastHistoryKey='';
const optionsHtml=`)rawliteral";
  html += scheduleOptions;
  html += R"rawliteral(`;

function updateMobileScrollLock(){
  const isMobile=window.matchMedia('(max-width:900px)').matches;
  const open=document.getElementById('sidebar').classList.contains('open');
  document.body.classList.toggle('lock-scroll', isMobile && open);
  document.body.classList.toggle('nav-open', isMobile && open);
  const ov=document.getElementById('mobileOverlay');
  if(ov) ov.classList.toggle('show', isMobile && open);
}

function toggleSidebar(){
  document.getElementById('sidebar').classList.toggle('open');
  updateMobileScrollLock();
}

function showPage(id){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  const target=document.getElementById('page-'+id);
  target.classList.add('active');
  target.style.animation='none';
  void target.offsetWidth;
  target.style.animation='pageIn .35s ease';
  document.querySelectorAll('.navBtn').forEach(b=>b.classList.remove('active'));
  document.querySelector(`.navBtn[data-page="${id}"]`).classList.add('active');
  document.getElementById('sidebar').classList.remove('open');
  updateMobileScrollLock();
}

function iconClassFor(name){
  const n=(name||'').toLowerCase();
  if(n.includes('fan')) return 'fa-fan';
  if(n.includes('ac') || n.includes('air')) return 'fa-snowflake';
  if(n.includes('light')) return 'fa-lightbulb';
  if(n.includes('bed')) return 'fa-bed';
  if(n.includes('kitchen')) return 'fa-kitchen-set';
  if(n.includes('gate') || n.includes('door')) return 'fa-door-open';
  if(n.includes('pump') || n.includes('water')) return 'fa-faucet';
  if(n.includes('heater')) return 'fa-fire';
  if(n.includes('tv')) return 'fa-tv';
  if(n.includes('fridge')) return 'fa-box';
  if(n.includes('wash')) return 'fa-shirt';
  if(n.includes('socket') || n.includes('plug')) return 'fa-plug';
  if(n.includes('curtain') || n.includes('window')) return 'fa-window-maximize';
  if(n.includes('garden')) return 'fa-seedling';
  if(n.includes('motor')) return 'fa-gear';
  return 'fa-bolt';
}

function renderRemote(relays,names){
  const key=JSON.stringify({r:relays||[],n:names||[]});
  if(key===lastRemoteRenderKey) return;
  lastRemoteRenderKey=key;
  const box=document.getElementById('remoteCards');
  box.innerHTML=(names||[]).map((n,i)=>`
    <div class='card remoteCard ${relays[i]?'is-on':'is-off'}'>
      <div class='remoteTop'>
        <label class='switch'><input type='checkbox' ${relays[i]?'checked':''} onchange='toggleRelay(${i},this.checked)'><span class='slider'></span></label>
        <span class='statusLamp ${relays[i]?'on':'off'}'></span>
      </div>
      <div class='remoteMain'>
        <div class='remoteLeft'>
          <div class='nameUnder'>${n}</div>
          <div class='sub'>${relays[i]?'ON':'OFF'} | Relay ${i+1}</div>
          <div class='tiny'>Top-left switch or logo</div>
        </div>
        <div class='logoBadge ${relays[i]?'is-on':'is-off'}' onclick='toggleFromLogo(${i})'><i class='fa-solid ${iconClassFor(n)}'></i></div>
      </div>
    </div>`).join('');
}

function toggleFromLogo(i){
  const current=!!latestRelays[i];
  toggleRelay(i,!current,true);
}

function animateRemoteCard(i, state){
  const cards=document.querySelectorAll('#remoteCards .remoteCard');
  const card=cards[i];
  if(!card) return;
  card.classList.remove('is-on','is-off');
  card.classList.add(state?'is-on':'is-off','animating');
  const lamp=card.querySelector('.statusLamp');
  if(lamp){
    lamp.classList.remove('on','off');
    lamp.classList.add(state?'on':'off');
  }
  const logo=card.querySelector('.logoBadge');
  if(logo){
    logo.classList.remove('is-on','is-off');
    logo.classList.add(state?'is-on':'is-off');
  }
  const sw=card.querySelector('.switch input');
  if(sw) sw.checked=!!state;
  const stateText=card.querySelector('.sub');
  if(stateText) stateText.textContent=`${state?'ON':'OFF'} | Relay ${i+1}`;
  setTimeout(()=>card.classList.remove('animating'),300);
}

function updateSettingsIcons(){
  for(let i=0;i<relayCount;i++){
    const nm=document.getElementById('nm'+i);
    const icon=document.getElementById('si'+i);
    if(nm && icon){
      icon.className='fa-solid '+iconClassFor(nm.value||'');
    }
  }
}

function renderBulkConfig(names,onMask,offMask){
  const key=JSON.stringify({n:names||[],on:onMask||[],off:offMask||[]});
  if(key===lastBulkRenderKey) return;
  lastBulkRenderKey=key;
  const box=document.getElementById('bulkConfigBox');
  if(!box) return;
  box.innerHTML=(names||[]).map((n,i)=>`
    <div class='card'>
      <div class='remoteTop'>
        <div class='logoBadge'><i class='fa-solid ${iconClassFor(n)}'></i></div>
        <div style='flex:1;min-width:0'><b>${n}</b><div class='tiny'>Relay ${i+1}</div></div>
      </div>
      <div class='row tiny'>
        <label><input type='checkbox' class='cfgOn' data-i='${i}' ${onMask[i]?'checked':''}> Include in ON-ALL</label>
      </div>
      <div class='row tiny'>
        <label><input type='checkbox' class='cfgOff' data-i='${i}' ${offMask[i]?'checked':''}> Include in OFF-ALL</label>
      </div>
    </div>`).join('');
}

function formatUsageTime(totalSeconds){
  const sec=Math.max(0, Number(totalSeconds)||0);
  const hh=Math.floor(sec/3600);
  const mm=Math.floor((sec%3600)/60);
  const ss=sec%60;
  if(hh>0) return `${hh} hr ${mm} min ${ss} sec`;
  if(mm>0) return `${mm} min ${ss} sec`;
  return `${ss} sec`;
}

function renderTodayUsage(names,mins){
  const key=JSON.stringify({n:names||[],m:mins||[]});
  if(key===lastYesterdayRenderKey) return;
  lastYesterdayRenderKey=key;
  const body=document.getElementById('todayUsageBody');
  if(!mins || !mins.length){ body.innerHTML='<tr><td colspan="2">No data</td></tr>'; return; }
  body.innerHTML=mins.map((m,i)=>{
    const totalSec=Math.max(0, Number(m||0))*60;
    const applianceName=names[i]||`Relay ${i+1}`;
    return `<tr>
      <td style="text-align:left">
        <i class="fa-solid ${iconClassFor(applianceName)}" style="margin-right:8px;color:#93c5fd"></i>${applianceName}
      </td>
      <td><b>${formatUsageTime(totalSec)}</b></td>
    </tr>`;
  }).join('');
}

function renderTopUsageGraph(names,mins){
  const key=JSON.stringify({n:names||[],m:mins||[]});
  if(key===lastTopUsageRenderKey) return;
  lastTopUsageRenderKey=key;
  const pairs=(mins||[]).map((m,i)=>({name:names[i],min:m})).sort((a,b)=>b.min-a.min).slice(0,3);
  const max=Math.max(1,...pairs.map(p=>p.min));
  const box=document.getElementById('topUsageGraph');
  if(!pairs.length){ box.textContent='No data'; return; }
  box.innerHTML=pairs.map(p=>`<div class='tiny'>${p.name} (${p.min}m)</div><div class='barWrap'><div class='barOn' style='width:${(p.min*100/max).toFixed(0)}%'></div></div>`).join('');
}

function renderStatusPie(parts){
  const total=Math.max(1, parts.reduce((a,b)=>a+Number(b.value||0),0));
  let progress=0;
  const segments=parts.map(p=>{
    const pct=(Number(p.value||0)*100/total);
    const from=progress;
    progress+=pct;
    return `${p.color} ${from.toFixed(2)}% ${progress.toFixed(2)}%`;
  });
  document.getElementById('statusPie').style.background=`conic-gradient(${segments.join(',')})`;
}

function renderTrendGraph(labels,values){
  const key=JSON.stringify({l:labels||[],v:values||[]});
  if(key===lastTrendKey) return;
  lastTrendKey=key;
  const box=document.getElementById('trendLineGraph');
  if(!values || !values.length){ box.textContent='No data'; return; }
  const data=values.map(v=>Number(v||0));
  const max=Math.max(1,...data);
  const w=360;
  const h=160;
  const padX=20;
  const padY=18;
  const innerW=w-padX*2;
  const innerH=h-padY*2;
  const step=(data.length>1)?(innerW/(data.length-1)):0;
  const points=data.map((v,i)=>{
    const x=padX+(step*i);
    const y=padY+innerH-(v*innerH/max);
    return {x,y,v,label:labels[i]||('-'+(data.length-i-1)+'d')};
  });
  const polyline=points.map(p=>`${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(' ');
  const circles=points.map(p=>`<circle cx="${p.x.toFixed(1)}" cy="${p.y.toFixed(1)}" r="3.5" fill="#22d3ee"></circle>`).join('');
  const xLabels=points.map(p=>`<text x="${p.x.toFixed(1)}" y="${h-2}" text-anchor="middle" font-size="9" fill="#94a3b8">${p.label}</text>`).join('');
  const yGrid=[0.25,0.5,0.75].map(r=>{
    const y=(padY+innerH-(innerH*r)).toFixed(1);
    return `<line x1="${padX}" y1="${y}" x2="${w-padX}" y2="${y}" stroke="#334155" stroke-width="1" stroke-dasharray="3 3"></line>`;
  }).join('');
  box.innerHTML=`
    <svg viewBox="0 0 ${w} ${h}" width="100%" height="170" style="display:block;background:#0b1220;border:1px solid #334155;border-radius:10px;padding:6px">
      ${yGrid}
      <line x1="${padX}" y1="${padY+innerH}" x2="${w-padX}" y2="${padY+innerH}" stroke="#475569" stroke-width="1.2"></line>
      <polyline points="${polyline}" fill="none" stroke="#22d3ee" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"></polyline>
      ${circles}
      ${xLabels}
    </svg>
    <div class="tiny" style="margin-top:6px">Peak: ${max} min</div>
  `;
}

function renderHistoryTable(history){
  const key=JSON.stringify(history||[]);
  if(key===lastHistoryKey) return;
  lastHistoryKey=key;
  const body=document.getElementById('historyTableBody');
  if(!history || !history.length){
    body.innerHTML='<tr><td colspan="4">No history yet</td></tr>';
    return;
  }
  body.innerHTML=history.map(h=>`<tr><td>${h.time||'-'}</td><td>${h.name||'-'}</td><td>${h.state||'-'}</td><td>${h.source||'-'}</td></tr>`).join('');
}

function addHoldRow(defaultCh='0', defaultMin='10'){
  const row=document.createElement('div');
  row.className='card holdRow';
  row.innerHTML=`
    <div class='row'>
      <div style='flex:2;min-width:180px'><label>Appliance</label><select class='hCh'>${optionsHtml}</select></div>
      <div style='flex:1;min-width:120px'><label>Minutes</label><input class='hMin' type='number' min='1' max='1440' value='${defaultMin}'></div>
    </div>
    <div class='row'>
      <button class='btn' onclick='startHoldRow(this)'>Start</button>
      <button class='btn2' onclick='cancelHoldRow(this)'>Cancel</button>
      <button class='btn2' onclick='removeRow(this)'>Remove</button>
    </div>`;
  document.getElementById('holdRows').appendChild(row);
  row.querySelector('.hCh').value=defaultCh;
}

function addScheduleRow(defaultCh='0', on='08:00', off='22:00', en=true){
  const row=document.createElement('div');
  row.className='card schRow';
  row.innerHTML=`
    <div class='row'>
      <div style='flex:2;min-width:180px'><label>Location</label><select class='sCh'>${optionsHtml}</select></div>
      <div style='flex:1;min-width:120px'><label>ON</label><input class='sOn' type='time' value='${on}'></div>
      <div style='flex:1;min-width:120px'><label>OFF</label><input class='sOff' type='time' value='${off}'></div>
    </div>
    <div class='row'>
      <label><input class='sEn' type='checkbox' ${en?'checked':''}> Enable</label>
      <button class='btn' onclick='saveScheduleRow(this)'>Save</button>
      <button class='btn2' onclick='removeRow(this)'>Remove</button>
    </div>`;
  document.getElementById('scheduleRows').appendChild(row);
  row.querySelector('.sCh').value=defaultCh;
}

function removeRow(btn){ btn.closest('.card').remove(); }

async function startHoldRow(btn){
  const row=btn.closest('.card');
  const ch=row.querySelector('.hCh').value;
  const min=row.querySelector('.hMin').value;
  try{
    const r=await fetch(`/set-hold?ch=${ch}&minutes=${encodeURIComponent(min)}`);
    const j=await r.json();
    if(j.ok){
      if(!row.querySelector('.countdown')){
        const d=document.createElement('div');
        d.className='sub countdown';
        d.style.marginTop='6px';
        row.appendChild(d);
      }
      row.dataset.ch=ch;
      row.dataset.remaining=(Number(min)||0)*60;
      document.getElementById('holdMsg').textContent='Hold timer started';
    } else document.getElementById('holdMsg').textContent='Failed to start timer';
    refreshStatus();
  }catch(e){ document.getElementById('holdMsg').textContent='Request failed'; }
}

async function cancelHoldRow(btn){
  const row=btn.closest('.card');
  const ch=row.querySelector('.hCh').value;
  try{
    const r=await fetch(`/set-hold?ch=${ch}&minutes=0`);
    const j=await r.json();
    document.getElementById('holdMsg').textContent=j.ok?'Hold timer cancelled':'Failed';
    row.dataset.remaining='0';
  }catch(e){}
}

async function saveScheduleRow(btn){
  const row=btn.closest('.card');
  const ch=row.querySelector('.sCh').value;
  const on=row.querySelector('.sOn').value||'00:00';
  const off=row.querySelector('.sOff').value||'00:00';
  const en=row.querySelector('.sEn').checked?1:0;
  try{
    const r=await fetch(`/set-schedule?ch=${ch}&on=${encodeURIComponent(on)}&off=${encodeURIComponent(off)}&en=${en}`);
    const j=await r.json();
    document.getElementById('schMsg').textContent=j.ok?'Schedule saved':'Save failed';
  }catch(e){ document.getElementById('schMsg').textContent='Request failed'; }
}

async function refreshStatus(){
  try{
    const r=await fetch('/status');
    const j=await r.json();
    const newRelays=j.relays||[];
    const newNames=j.names||[];
    const newYesterday=j.yesterday_minutes||[];
    const newToday=j.today_minutes||[];
    const newOnAllMask=j.on_all_mask||[];
    const newOffAllMask=j.off_all_mask||[];
    const namesChanged=(JSON.stringify(newNames)!==JSON.stringify(latestNames));
    const relaysChanged=(JSON.stringify(newRelays)!==JSON.stringify(latestRelays));
    const todayChanged=(JSON.stringify(newToday)!==JSON.stringify(latestToday));
    const onMaskChanged=(JSON.stringify(newOnAllMask)!==JSON.stringify(latestOnAllMask));
    const offMaskChanged=(JSON.stringify(newOffAllMask)!==JSON.stringify(latestOffAllMask));
    latestRelays=newRelays;
    latestNames=newNames;
    latestYesterday=newYesterday;
    latestToday=newToday;
    latestOnAllMask=newOnAllMask;
    latestOffAllMask=newOffAllMask;
    document.getElementById('clockText').textContent=j.now||'Time not set';
    document.getElementById('dashSsid').textContent=j.ssid||'Not connected';
    document.getElementById('dashIp').textContent=j.ip||'No IP';
    const on=latestRelays.filter(Boolean).length;
    const off=relayCount-on;
    document.getElementById('onCount').textContent=on;
    document.getElementById('offCount').textContent=off;
    const setTimeCount=Number(j.schedule_enabled_count||0);
    const timerRunCount=Number(j.timer_running_count||0);
    const remoteOnCount=Number(j.remote_turn_on_today||0);
    document.getElementById('setTimeCount').textContent=setTimeCount;
    document.getElementById('timerRunCount').textContent=timerRunCount;
    document.getElementById('remoteOnCount').textContent=remoteOnCount;
    renderStatusPie([
      {label:'ON', value:on, color:'#22c55e'},
      {label:'OFF', value:off, color:'#ef4444'},
      {label:'Set Time', value:setTimeCount, color:'#06b6d4'},
      {label:'Timer', value:timerRunCount, color:'#eab308'},
      {label:'Remote ON', value:remoteOnCount, color:'#8b5cf6'}
    ]);
    if(namesChanged || relaysChanged) renderRemote(latestRelays,latestNames);
    if(namesChanged) updateSettingsIcons();
    if(namesChanged || onMaskChanged || offMaskChanged) renderBulkConfig(latestNames, latestOnAllMask, latestOffAllMask);
    if(namesChanged || todayChanged) renderTodayUsage(latestNames,latestToday);
    if(namesChanged || todayChanged) renderTopUsageGraph(latestNames,latestToday);
    renderTrendGraph(j.trend_labels||[], j.trend_values||[]);
    renderHistoryTable(j.history||[]);
    updateCountdownRows(j.hold_remaining_sec||[]);
  }catch(e){}
}

async function bulkAction(action){
  try{
    const r=await fetch(`/bulk?action=${action}`);
    const j=await r.json();
    if(!j.ok) alert('Bulk action failed');
    refreshStatus();
  }catch(e){}
}

async function saveBulkConfig(){
  const onMask=Array.from(document.querySelectorAll('.cfgOn')).sort((a,b)=>Number(a.dataset.i)-Number(b.dataset.i)).map(el=>el.checked?'1':'0').join('');
  const offMask=Array.from(document.querySelectorAll('.cfgOff')).sort((a,b)=>Number(a.dataset.i)-Number(b.dataset.i)).map(el=>el.checked?'1':'0').join('');
  try{
    const r=await fetch('/bulk-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`onMask=${onMask}&offMask=${offMask}`});
    const j=await r.json();
    if(!j.ok) alert('Save failed');
    else alert('ON-ALL/OFF-ALL settings saved');
    refreshStatus();
  }catch(e){ alert('Save failed'); }
}

function updateCountdownRows(holdRemaining){
  document.querySelectorAll('.holdRow').forEach(row=>{
    const ch=Number(row.querySelector('.hCh').value||0);
    const remain=Number(holdRemaining[ch]||0);
    let node=row.querySelector('.countdown');
    if(!node){
      node=document.createElement('div');
      node.className='sub countdown';
      row.appendChild(node);
    }
    if(remain>0){
      const mm=Math.floor(remain/60);
      const ss=remain%60;
      node.textContent=`Reverse timer: ${String(mm).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;
    }else{
      node.textContent='Reverse timer: 00:00 (appliance OFF)';
    }
  });
}

async function toggleRelay(i,state,fromLogo=false){
  latestRelays[i]=!!state;
  animateRemoteCard(i, !!state);
  try{
    await fetch(`/relay?ch=${i}&state=${state?1:0}`);
    refreshStatus();
  }catch(e){
    latestRelays[i]=!state;
    animateRemoteCard(i, !state);
  }
}

async function saveName(ch){
  const name=document.getElementById('nm'+ch).value.trim();
  if(!name) return;
  try{
    const r=await fetch('/set-name',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`ch=${ch}&name=${encodeURIComponent(name)}`});
    const j=await r.json();
    if(j.ok){
      const op=document.querySelectorAll('.hCh option[value="'+ch+'"], .sCh option[value="'+ch+'"]');
      op.forEach(o=>o.text=name);
      updateSettingsIcons();
      refreshStatus();
    }
  }catch(e){}
}

async function syncBrowserTime(){
  const epoch=Math.floor(Date.now()/1000);
  await fetch('/sync-time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`epoch=${epoch}`});
  refreshStatus();
}
async function openSetup(){ await fetch('/start-ap'); }
async function resetWiFi(){ if(confirm('Forget Wi-Fi and reboot?')) await fetch('/reset-wifi'); }

addHoldRow();
addScheduleRow();
updateMobileScrollLock();
window.addEventListener('resize', updateMobileScrollLock);
refreshStatus();
setInterval(refreshStatus,5000);
</script>
</body></html>
)rawliteral";
  return html;
}

void handleRoot() {
  if (!requireAuth(false)) return;
  if (staConnected) server.send(200, "text/html", renderAppPage());
  else server.send(200, "text/html", renderSetupPage());
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    String ssid = jsonEscape(WiFi.SSID(i));
    bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":\"" + wifiStrengthLabel(WiFi.RSSI(i)) + " " + String(WiFi.RSSI(i)) + " dBm\",\"secure\":";
    json += secure ? "true" : "false";
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  if (!server.hasArg("ssid")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  bool ok = connectToWiFi(ssid, pass, 25000);
  if (ok) {
    saveCredentials(ssid, pass);
    ensureMDNS();
    server.send(200, "application/json", "{\"ok\":true,\"ip\":\"" + staIP + "\"}");
  } else {
    startAPMode();
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Connection timeout or wrong password\"}");
  }
}

void handleStatus() {
  uint8_t scheduleEnabledCount = 0;
  uint8_t timerRunningCount = 0;
  uint32_t nowMs = millis();
  uint16_t todayTotal = 0;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (schedules[i].enabled) scheduleEnabledCount++;
    if (holdUntilMs[i] > 0 && (int32_t)(holdUntilMs[i] - nowMs) > 0) timerRunningCount++;
    todayTotal += todayMinutes[i];
  }
  todayTotalCache = todayTotal;
  time_t nowEpoch = currentEpoch();
  if (nowEpoch > 0) {
    struct tm nowTm;
    localtime_r(&nowEpoch, &nowTm);
    char label[8];
    strftime(label, sizeof(label), "%b %d", &nowTm);
    dailyLabelHistory[TREND_DAYS - 1] = String(label);
  }

  String json = "{";
  json += "\"connected\":" + String(staConnected ? "true" : "false");
  json += ",\"ssid\":\"" + jsonEscape(staSSID) + "\"";
  json += ",\"ip\":\"" + staIP + "\"";
  json += ",\"ap\":" + String(apModeActive ? "true" : "false");
  json += ",\"time_ready\":" + String(timeReady ? "true" : "false");
  json += ",\"now\":\"" + jsonEscape(currentTimeString()) + "\"";
  json += ",\"relays\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += getRelay(i) ? "true" : "false";
  }
  json += "],\"names\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += "\"" + jsonEscape(relayNames[i]) + "\"";
  }
  json += "],\"schedules\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += "{\"en\":";
    json += schedules[i].enabled ? "true" : "false";
    json += ",\"on\":\"" + minToHHMM(schedules[i].onMin) + "\",\"off\":\"" + minToHHMM(schedules[i].offMin) + "\"}";
  }
  json += "],\"yesterday_minutes\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += String(yesterdayMinutes[i]);
  }
  json += "],\"today_minutes\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += String(todayMinutes[i]);
  }
  json += "],\"hold_remaining_sec\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    if (holdUntilMs[i] > 0 && (int32_t)(holdUntilMs[i] - nowMs) > 0) {
      json += String((holdUntilMs[i] - nowMs) / 1000);
    } else {
      json += "0";
    }
  }
  json += "],\"on_all_mask\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += includeInOnAll[i] ? "true" : "false";
  }
  json += "],\"off_all_mask\":[";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i) json += ",";
    json += includeInOffAll[i] ? "true" : "false";
  }
  json += "],\"schedule_enabled_count\":" + String(scheduleEnabledCount);
  json += ",\"timer_running_count\":" + String(timerRunningCount);
  json += ",\"remote_turn_on_today\":" + String(remoteTurnOnCountToday);
  json += ",\"trend_labels\":[";
  for (uint8_t i = 0; i < TREND_DAYS; i++) {
    if (i) json += ",";
    json += "\"" + jsonEscape(dailyLabelHistory[i]) + "\"";
  }
  json += "],\"trend_values\":[";
  for (uint8_t i = 0; i < TREND_DAYS; i++) {
    if (i) json += ",";
    if (i == TREND_DAYS - 1) {
      json += String(todayTotalCache);
    } else {
      json += String(dailyTotalHistory[i]);
    }
  }
  json += "],\"history\":[";
  for (uint8_t i = 0; i < eventCount; i++) {
    uint8_t idx = (eventNextWrite + EVENT_HISTORY_MAX - 1 - i) % EVENT_HISTORY_MAX;
    if (i) json += ",";
    time_t eventTs = (time_t)eventEpoch[idx];
    String eventTime = "-";
    if (eventTs > 0) {
      struct tm eventTm;
      localtime_r(&eventTs, &eventTm);
      char timeBuf[24];
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &eventTm);
      eventTime = String(timeBuf);
    }
    uint8_t ch = eventChannel[idx];
    const char *src = sourceLabel((RelayChangeSource)eventSource[idx]);
    json += "{\"time\":\"" + jsonEscape(eventTime) + "\",\"name\":\"" + jsonEscape(relayNames[ch]) + "\",\"state\":\"";
    json += eventState[idx] ? "ON" : "OFF";
    json += "\",\"source\":\"" + String(src) + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleRelay() {
  if (!server.hasArg("ch") || !server.hasArg("state")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ch and state required\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  int state = server.arg("state").toInt();
  if (ch < 0 || ch >= RELAY_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid channel\"}");
    return;
  }
  setRelayWithSource((uint8_t)ch, state == 1, SRC_REMOTE);
  if (state == 0) holdUntilMs[ch] = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetSchedule() {
  if (!server.hasArg("ch") || !server.hasArg("on") || !server.hasArg("off") || !server.hasArg("en")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ch,on,off,en required\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch >= RELAY_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid channel\"}");
    return;
  }
  schedules[ch].onMin = hhmmToMin(server.arg("on"));
  schedules[ch].offMin = hhmmToMin(server.arg("off"));
  schedules[ch].enabled = (server.arg("en").toInt() == 1);
  saveSchedules();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetHold() {
  if (!server.hasArg("ch") || !server.hasArg("minutes")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ch and minutes required\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  int minutes = server.arg("minutes").toInt();
  if (ch < 0 || ch >= RELAY_COUNT || minutes < 0 || minutes > 1440) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid values\"}");
    return;
  }
  if (minutes == 0) {
    holdUntilMs[ch] = 0;
  } else {
    setRelayWithSource((uint8_t)ch, true, SRC_TIMER);
    holdUntilMs[ch] = millis() + (uint32_t)minutes * 60000UL;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetName() {
  if (!server.hasArg("ch") || !server.hasArg("name")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ch and name required\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch >= RELAY_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid channel\"}");
    return;
  }
  String name = server.arg("name");
  name.trim();
  if (name.length() == 0) name = DEFAULT_RELAY_NAMES[ch];
  saveRelayName((uint8_t)ch, name);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleBulkAction() {
  if (!server.hasArg("action")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"action required\"}");
    return;
  }
  String action = server.arg("action");
  bool turnOn = (action == "on");
  if (!turnOn && action != "off") {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid action\"}");
    return;
  }
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    bool include = turnOn ? includeInOnAll[i] : includeInOffAll[i];
    if (!include) continue;
    setRelayWithSource(i, turnOn, SRC_BULK);
    if (!turnOn) holdUntilMs[i] = 0;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleBulkConfig() {
  if (!server.hasArg("onMask") || !server.hasArg("offMask")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"onMask and offMask required\"}");
    return;
  }
  String onMask = server.arg("onMask");
  String offMask = server.arg("offMask");
  if (onMask.length() != RELAY_COUNT || offMask.length() != RELAY_COUNT) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"mask length mismatch\"}");
    return;
  }
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    includeInOnAll[i] = (onMask.charAt(i) == '1');
    includeInOffAll[i] = (offMask.charAt(i) == '1');
  }
  saveBulkConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSyncTime() {
  if (!server.hasArg("epoch")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"epoch required\"}");
    return;
  }
  manualEpochBase = (time_t)server.arg("epoch").toInt();
  manualEpochMillis = millis();
  timeReady = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStartAP() {
  if (!apModeActive) startAPMode();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetWiFi() {
  saveDashState(true);
  clearCredentials();
  server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

void setupServer() {
  const char *headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/login", HTTP_POST, handleLoginSubmit);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, []() { if (!requireAuth(true)) return; handleScan(); });
  server.on("/connect", HTTP_POST, []() { if (!requireAuth(true)) return; handleConnect(); });
  server.on("/status", HTTP_GET, []() { if (!requireAuth(true)) return; handleStatus(); });
  server.on("/relay", HTTP_GET, []() { if (!requireAuth(true)) return; handleRelay(); });
  server.on("/set-schedule", HTTP_GET, []() { if (!requireAuth(true)) return; handleSetSchedule(); });
  server.on("/set-hold", HTTP_GET, []() { if (!requireAuth(true)) return; handleSetHold(); });
  server.on("/set-name", HTTP_POST, []() { if (!requireAuth(true)) return; handleSetName(); });
  server.on("/bulk", HTTP_GET, []() { if (!requireAuth(true)) return; handleBulkAction(); });
  server.on("/bulk-config", HTTP_POST, []() { if (!requireAuth(true)) return; handleBulkConfig(); });
  server.on("/sync-time", HTTP_POST, []() { if (!requireAuth(true)) return; handleSyncTime(); });
  server.on("/start-ap", HTTP_GET, []() { if (!requireAuth(true)) return; handleStartAP(); });
  server.on("/reset-wifi", HTTP_GET, []() { if (!requireAuth(true)) return; handleResetWiFi(); });
  server.onNotFound([]() {
    if (!isAuthenticated()) {
      server.sendHeader("Location", "/login", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  setupRelays();
  loadRelayNames();
  loadBulkConfig();
  loadSchedules();
  loadDashState();
  startAPMode();
  String savedSSID, savedPASS;
  loadCredentials(savedSSID, savedPASS);
  if (savedSSID.length() > 0) {
    if (connectToWiFi(savedSSID, savedPASS, 18000)) {
      ensureMDNS();
      Blynk.config(BLYNK_AUTH_TOKEN);
      Blynk.connect();
    }
  }
  setupServer();
}

void loop() {
  if (apModeActive) dnsServer.processNextRequest();
  if (staConnected) {
    Blynk.run();
    if (millis() - lastBlynkSync > BLYNK_SYNC_INTERVAL) {
      syncBlynkWithSystem();
      lastBlynkSync = millis();
    }
    blynkConnected = Blynk.connected();
  }
  if (WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    staSSID = WiFi.SSID();
    staIP = WiFi.localIP().toString();
  } else if (!apModeActive) {
    staConnected = false;
    staSSID = "";
    staIP = "";
  }
  applySchedules();
  applyHoldTimers();
  updateRelayUsageStats();
  saveDashState(false);
  server.handleClient();
}
