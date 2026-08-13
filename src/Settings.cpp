#include "Settings.h"
#include "Platform.h"   // platformChipId() for the unique default hostname
#include <LittleFS.h>

static const char* CONFIG_PATH = "/config.json";

// ===========================================================================
// Ticker slice
// ===========================================================================
static const char* srcToStr(uint8_t s) {
  return (s == SRC_YAHOO) ? "yahoo"
       : (s == SRC_CASH)  ? "cash"
       : (s == SRC_GHUB)  ? "github"
       : (s == SRC_SA)    ? "sa" : "webhook";
}
static uint8_t srcFromStr(const String& s) {
  return s.equalsIgnoreCase("yahoo")  ? SRC_YAHOO
       : s.equalsIgnoreCase("cash")   ? SRC_CASH
       : s.equalsIgnoreCase("github") ? SRC_GHUB
       : s.equalsIgnoreCase("sa")     ? SRC_SA : SRC_WEBHOOK;
}

void TickerSettings::setDefaults() {
  webhookUrl = "";
  range = DEFAULT_RANGE;
  points = DEFAULT_POINTS;
  pollSec = DEFAULT_POLL_SEC;
  rotateSec = DEFAULT_ROTATE_SEC;
  alertPct = 5;           // a 5% day is news on most tickers; 0 turns it off
  colorInverted = false;
  changeOnRange = true;

  showName = true;
  showPrice = true;
  showChange = true;
  showChart = true;
  showRangeLabel = true;
  showUpdatedAgo = false;
  showPageDots = true;
  showPortfolio = true;   // only visible once a symbol has qty+cost set

  symbolCount = 0;
  for (uint8_t i = 0; i < MAX_SYMBOLS; i++) {
    symbols[i].symbol[0] = 0;
    symbols[i].name[0] = 0;
    symbols[i].source = DEFAULT_SOURCE;
    symbols[i].qty = 0;
    symbols[i].cost = 0;
  }
}

void TickerSettings::toJson(JsonObject o) const {
  o["webhookUrl"]     = webhookUrl;
  o["range"]          = range;
  o["points"]         = points;
  o["pollSec"]        = pollSec;
  o["rotateSec"]      = rotateSec;
  o["alertPct"]       = alertPct;
  o["colorInverted"]  = colorInverted;
  o["changeOnRange"]  = changeOnRange;
  o["showName"]       = showName;
  o["showPrice"]      = showPrice;
  o["showChange"]     = showChange;
  o["showChart"]      = showChart;
  o["showRangeLabel"] = showRangeLabel;
  o["showUpdatedAgo"] = showUpdatedAgo;
  o["showPageDots"]   = showPageDots;
  o["showPortfolio"]  = showPortfolio;

  JsonArray arr = o["symbols"].to<JsonArray>();
  for (uint8_t i = 0; i < symbolCount; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["symbol"] = symbols[i].symbol;
    e["name"]   = symbols[i].name;
    e["source"] = srcToStr(symbols[i].source);
    e["qty"]    = symbols[i].qty;
    e["cost"]   = symbols[i].cost;
  }
}

void TickerSettings::fromJson(JsonObjectConst o) {
  // Legacy (pre-2.4) configs carried one global "source"; it becomes the
  // default for any symbol that doesn't carry its own.
  uint8_t legacySrc = DEFAULT_SOURCE;
  if (o["source"].is<const char*>()) legacySrc = srcFromStr(o["source"].as<String>());

  if (o["webhookUrl"].is<const char*>()) webhookUrl = o["webhookUrl"].as<String>();
  if (o["range"].is<const char*>())      range = o["range"].as<String>();
  // Below 2 is a HEAL to the default, not a clamp to 2: a legacy config with
  // "points":0 (the old empty-field save bug) clamped to a 2-point chart -
  // which drew exactly two straight lines on a screen that used to show a
  // whole day of movement, while looking configured. An impossible value
  // means "give me the chart back", not "give me the worst legal chart".
  if (o["points"].is<int>()) {
    const int p = (int)o["points"];
    points = p < 2 ? DEFAULT_POINTS : (p > MAX_SPARK_POINTS ? MAX_SPARK_POINTS : (uint16_t)p);
  }
  if (o["pollSec"].is<int>())            pollSec = max(10, (int)o["pollSec"]);
  if (o["rotateSec"].is<int>())          rotateSec = max(2, (int)o["rotateSec"]);
  if (o["alertPct"].is<int>())           alertPct = (uint8_t)constrain((int)o["alertPct"], 0, 50);
  if (o["colorInverted"].is<bool>())     colorInverted = o["colorInverted"];
  if (o["changeOnRange"].is<bool>())     changeOnRange = o["changeOnRange"];

  if (o["showName"].is<bool>())       showName = o["showName"];
  if (o["showPrice"].is<bool>())      showPrice = o["showPrice"];
  if (o["showChange"].is<bool>())     showChange = o["showChange"];
  if (o["showChart"].is<bool>())      showChart = o["showChart"];
  if (o["showRangeLabel"].is<bool>()) showRangeLabel = o["showRangeLabel"];
  if (o["showUpdatedAgo"].is<bool>()) showUpdatedAgo = o["showUpdatedAgo"];
  if (o["showPageDots"].is<bool>())   showPageDots = o["showPageDots"];
  if (o["showPortfolio"].is<bool>())  showPortfolio = o["showPortfolio"];

  if (o["symbols"].is<JsonArrayConst>()) {
    JsonArrayConst arr = o["symbols"].as<JsonArrayConst>();
    symbolCount = 0;
    for (JsonObjectConst e : arr) {
      if (symbolCount >= MAX_SYMBOLS) break;
      const char* sym = e["symbol"] | "";
      if (!sym[0]) continue;                 // skip blank rows
      SymbolCfg& dst = symbols[symbolCount];
      strlcpy(dst.symbol, sym, MAX_SYMBOL_LEN);
      strlcpy(dst.name, e["name"] | "", MAX_NAME_LEN);
      dst.source = e["source"].is<const char*>()
                     ? srcFromStr(e["source"].as<String>()) : legacySrc;
      dst.qty  = e["qty"].as<float>();     // absent -> 0
      dst.cost = e["cost"].as<float>();
      if (dst.qty < 0)  dst.qty = 0;
      if (dst.cost < 0) dst.cost = 0;
      symbolCount++;
    }
  }
}

// ===========================================================================
// Usage slice
// ===========================================================================
void UsageSettings::setDefaults() {
  usageUrl = "";
  pollSec = DEFAULT_POLL_SEC;
}

void UsageSettings::toJson(JsonObject o) const {
  o["usageUrl"] = usageUrl;
  o["pollSec"]  = pollSec;
}

void UsageSettings::fromJson(JsonObjectConst o) {
  if (o["usageUrl"].is<const char*>()) usageUrl = o["usageUrl"].as<String>();
  if (o["pollSec"].is<int>())          pollSec = max(10, (int)o["pollSec"]);
}

// ===========================================================================
// Clock / night mode slice
// ===========================================================================
// "HH:MM" -> minutes since midnight. Hand-rolled for the same reason as the
// version parse in OtaUpdate.cpp: sscanf costs 16 KB of flash and this needs
// two integers and a colon.
static uint16_t hhmmToMin(const char* s, uint16_t fallback) {
  if (!s || !s[0]) return fallback;
  int h = 0, m = 0, seen = 0;
  const char* p = s;
  while (*p >= '0' && *p <= '9') { h = h * 10 + (*p++ - '0'); seen++; }
  if (!seen || *p != ':') return fallback;
  p++;
  seen = 0;
  while (*p >= '0' && *p <= '9') { m = m * 10 + (*p++ - '0'); seen++; }
  if (!seen) return fallback;
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return (uint16_t)(h * 60 + m);
}
static String minToHhmm(uint16_t v) {
  if (v > 1439) v = 0;
  char b[6];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(v / 60), (unsigned)(v % 60));
  return String(b);
}

void ClockSettings::setDefaults() {
  tz            = DEFAULT_TZ_NAME;
  tzPosix       = DEFAULT_TZ_POSIX;
  nightEnabled  = DEFAULT_NIGHT_ENABLED;
  nightStartMin = DEFAULT_NIGHT_START_MIN;
  nightEndMin   = DEFAULT_NIGHT_END_MIN;
  nightLevel    = DEFAULT_NIGHT_LEVEL;
  nightMining   = false;
  face          = CLOCK_FACE_PIXEL;
  mode12h       = false;
  showSeconds   = true;
  showDate      = true;
}

void ClockSettings::toJson(JsonObject o) const {
  o["tz"]           = tz;
  o["tzPosix"]      = tzPosix;
  o["nightEnabled"] = nightEnabled;
  o["nightStart"]   = minToHhmm(nightStartMin);
  o["nightEnd"]     = minToHhmm(nightEndMin);
  o["nightLevel"]   = nightLevel;
  o["nightMining"]  = nightMining;
  o["face"]         = face;
  o["mode12h"]      = mode12h;
  o["showSeconds"]  = showSeconds;
  o["showDate"]     = showDate;
}

void ClockSettings::fromJson(JsonObjectConst o) {
  if (o["tz"].is<const char*>())          tz = o["tz"].as<String>();
  if (o["tzPosix"].is<const char*>())     tzPosix = o["tzPosix"].as<String>();
  if (o["nightEnabled"].is<bool>())       nightEnabled = o["nightEnabled"];
  if (o["nightStart"].is<const char*>())  nightStartMin = hhmmToMin(o["nightStart"], nightStartMin);
  if (o["nightEnd"].is<const char*>())    nightEndMin   = hhmmToMin(o["nightEnd"], nightEndMin);
  if (o["nightLevel"].is<int>())          nightLevel = constrain((int)o["nightLevel"], 0, 100);
  if (o["face"].is<int>())                face = (uint8_t)constrain((int)o["face"], 0, 4);
  if (o["nightMining"].is<bool>())        nightMining = o["nightMining"];
  if (o["mode12h"].is<bool>())            mode12h = o["mode12h"];
  if (o["showSeconds"].is<bool>())        showSeconds = o["showSeconds"];
  if (o["showDate"].is<bool>())           showDate = o["showDate"];
}

// ===========================================================================
// Radar slice
// ===========================================================================
void RadarSettings::setDefaults() {
  lat = DEFAULT_RADAR_LAT;
  lon = DEFAULT_RADAR_LON;
  source = DEFAULT_RADAR_SRC;
  webhookUrl = "";
  rangeKm = DEFAULT_RADAR_RANGE_KM;
  pollSec = DEFAULT_RADAR_POLL_SEC;
  unitsMi = false;
  showLabels = true;
  showVectors = true;
  showRimDots = true;
  uiScale = 1;            // medium
  minAltFt = 0;           // show all
  airportCount = 0;
  for (uint8_t i = 0; i < MAX_AIRPORTS; i++) {
    airports[i].icao[0] = 0;
    airports[i].lat = airports[i].lon = 0;
  }
}

void RadarSettings::toJson(JsonObject o) const {
  o["lat"]         = lat;
  o["lon"]         = lon;
  o["source"]      = (source == RADAR_SRC_WEBHOOK) ? "webhook" : "direct";
  o["webhookUrl"]  = webhookUrl;
  o["rangeKm"]     = rangeKm;
  o["pollSec"]     = pollSec;
  o["unitsMi"]     = unitsMi;
  o["showLabels"]  = showLabels;
  o["showVectors"] = showVectors;
  o["showRimDots"] = showRimDots;
  o["uiScale"]     = uiScale;
  o["minAltFt"]    = minAltFt;

  JsonArray arr = o["airports"].to<JsonArray>();
  for (uint8_t i = 0; i < airportCount; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["icao"] = airports[i].icao;
    e["lat"]  = airports[i].lat;
    e["lon"]  = airports[i].lon;
  }
}

void RadarSettings::fromJson(JsonObjectConst o) {
  if (o["lat"].is<float>() || o["lat"].is<int>()) lat = o["lat"].as<float>();
  if (o["lon"].is<float>() || o["lon"].is<int>()) lon = o["lon"].as<float>();
  if (o["source"].is<const char*>()) {
    String src = o["source"].as<String>();
    source = src.equalsIgnoreCase("webhook") ? RADAR_SRC_WEBHOOK : RADAR_SRC_DIRECT;
  }
  if (o["webhookUrl"].is<const char*>()) webhookUrl = o["webhookUrl"].as<String>();
  if (o["rangeKm"].is<int>())    rangeKm = constrain((int)o["rangeKm"], 1, 500);
  if (o["pollSec"].is<int>())    pollSec = max(3, (int)o["pollSec"]);
  if (o["unitsMi"].is<bool>())   unitsMi = o["unitsMi"];
  if (o["showLabels"].is<bool>())  showLabels = o["showLabels"];
  if (o["showVectors"].is<bool>()) showVectors = o["showVectors"];
  if (o["showRimDots"].is<bool>()) showRimDots = o["showRimDots"];
  if (o["uiScale"].is<int>())      uiScale = constrain((int)o["uiScale"], 0, 2);
  if (o["minAltFt"].is<int>())     minAltFt = constrain((int)o["minAltFt"], 0, 60000);

  if (o["airports"].is<JsonArrayConst>()) {
    JsonArrayConst arr = o["airports"].as<JsonArrayConst>();
    airportCount = 0;
    for (JsonObjectConst e : arr) {
      if (airportCount >= MAX_AIRPORTS) break;
      const char* ic = e["icao"] | "";
      if (!ic[0]) continue;                  // skip blank rows
      Airport& dst = airports[airportCount];
      strlcpy(dst.icao, ic, MAX_ICAO_LEN);
      dst.lat = e["lat"].as<float>();
      dst.lon = e["lon"].as<float>();
      airportCount++;
    }
  }
}

// ===========================================================================
// Miner slice
// ===========================================================================
void MinerSettings::setDefaults() {
  enabled    = true;
  poolHost   = DEFAULT_POOL_HOST;
  poolPort   = DEFAULT_POOL_PORT;
  btcAddress = "";
  workerName = "";
  engine     = DEFAULT_MINER_ENGINE;
}

void MinerSettings::toJson(JsonObject o) const {
  o["enabled"]    = enabled;
  o["poolHost"]   = poolHost;
  o["poolPort"]   = poolPort;
  o["btcAddress"] = btcAddress;
  o["workerName"] = workerName;
  o["engine"]     = (engine == MINER_ENGINE_HYBRID) ? "hybrid" : "sw";
}

void MinerSettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>()) enabled = o["enabled"];
  if (o["poolHost"].is<const char*>()) {
    poolHost = o["poolHost"].as<String>();
    poolHost.trim();
    if (!poolHost.length()) poolHost = DEFAULT_POOL_HOST;   // blank = back to default
  }
  if (o["poolPort"].is<int>()) poolPort = constrain((int)o["poolPort"], 1, 65535);
  if (o["btcAddress"].is<const char*>()) {
    btcAddress = o["btcAddress"].as<String>();
    btcAddress.trim();
  }
  if (o["workerName"].is<const char*>()) {
    workerName = o["workerName"].as<String>();
    workerName.trim();
  }
  if (o["engine"].is<const char*>())
    engine = o["engine"].as<String>().equalsIgnoreCase("hybrid")
               ? MINER_ENGINE_HYBRID : MINER_ENGINE_SW;
}

// ===========================================================================
// Spotify slice
// ===========================================================================
void SpotifySettings::setDefaults() {
  enabled      = false;         // needs linking before it can do anything
  clientId     = "";
  clientSecret = "";
  refreshToken = "";
  pollSec      = DEFAULT_SPOTIFY_POLL_SEC;
  autoShow     = true;
}

void SpotifySettings::toJson(JsonObject o, bool includeSecrets) const {
  o["enabled"]  = enabled;
  o["clientId"] = clientId;
  o["pollSec"]  = pollSec;
  o["autoShow"] = autoShow;
  // Over the web API the page only learns whether these are set, never their
  // values — same as the WiFi passwords. They must still reach config.json,
  // though, or linking Spotify would not survive a power cycle.
  o["secretSet"] = clientSecret.length() > 0;
  o["tokenSet"]  = refreshToken.length() > 0;
  if (includeSecrets) {
    o["clientSecret"] = clientSecret;
    o["refreshToken"] = refreshToken;
  }
}

void SpotifySettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>())            enabled = o["enabled"];
  if (o["autoShow"].is<bool>())           autoShow = o["autoShow"];
  if (o["clientId"].is<const char*>())    { clientId = o["clientId"].as<String>(); clientId.trim(); }
  if (o["pollSec"].is<int>())             pollSec = constrain((int)o["pollSec"], 5, 300);
  // Blank means "keep what is stored", so saving the page does not wipe them.
  if (o["clientSecret"].is<const char*>()) {
    String v = o["clientSecret"].as<String>(); v.trim();
    if (v.length()) clientSecret = v;
  }
  if (o["refreshToken"].is<const char*>()) {
    String v = o["refreshToken"].as<String>(); v.trim();
    if (v.length()) refreshToken = v;
  }
}


// ===========================================================================
// Calendar slice
// ===========================================================================
static const char* calProvToStr(uint8_t v) {
  return v == CAL_MICROSOFT ? "microsoft" : v == CAL_ICS ? "ics" : "google";
}
static uint8_t calProvFromStr(const String& s) {
  if (s.equalsIgnoreCase("microsoft") || s.equalsIgnoreCase("outlook")) return CAL_MICROSOFT;
  if (s.equalsIgnoreCase("ics")) return CAL_ICS;
  return CAL_GOOGLE;
}

void WeatherSettings::setDefaults() {
  lat = 0.0f;
  lon = 0.0f;
  unitsF = true;
  pollSec = 900;          // Open-Meteo updates its models ~hourly; 15 min is plenty
  stormAlert = true;
  rainRadar = true;
}

void WeatherSettings::toJson(JsonObject o) const {
  o["lat"]     = lat;
  o["lon"]     = lon;
  o["unitsF"]  = unitsF;
  o["pollSec"] = pollSec;
  o["stormAlert"] = stormAlert;
  o["rainRadar"] = rainRadar;
}

void WeatherSettings::fromJson(JsonObjectConst o) {
  if (o["lat"].is<float>() || o["lat"].is<int>()) lat = o["lat"].as<float>();
  if (o["lon"].is<float>() || o["lon"].is<int>()) lon = o["lon"].as<float>();
  if (o["unitsF"].is<bool>()) unitsF = o["unitsF"];
  if (o["pollSec"].is<int>()) pollSec = (uint16_t)constrain((int)o["pollSec"], 300, 43200);
  if (o["stormAlert"].is<bool>()) stormAlert = o["stormAlert"];
  if (o["rainRadar"].is<bool>()) rainRadar = o["rainRadar"];
}

void CalendarSettings::setDefaults() {
  enabled      = false;        // needs linking before it can do anything
  provider     = CAL_GOOGLE;
  clientId     = "";
  clientSecret = "";
  refreshToken = "";
  icsUrl       = "";
  pollSec      = DEFAULT_CALENDAR_POLL_SEC;
  remindMin    = 5;        // a banner five minutes out; 0 turns it off
}

void CalendarSettings::toJson(JsonObject o, bool includeSecrets) const {
  o["enabled"]  = enabled;
  o["provider"] = calProvToStr(provider);
  o["clientId"] = clientId;
  o["pollSec"]  = pollSec;
  o["remindMin"] = remindMin;
  // The web API learns whether these are set, never their values — the same
  // rule as the WiFi passwords and the Spotify secrets. The ICS "secret
  // address" is exactly that — it grants read access to whoever holds it.
  o["secretSet"] = clientSecret.length() > 0;
  o["tokenSet"]  = refreshToken.length() > 0;
  o["icsSet"]    = icsUrl.length() > 0;
  if (includeSecrets) {
    o["clientSecret"] = clientSecret;
    o["refreshToken"] = refreshToken;
    o["icsUrl"]       = icsUrl;
  }
}

void CalendarSettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>())         enabled = o["enabled"];
  if (o["provider"].is<const char*>()) provider = calProvFromStr(o["provider"].as<String>());
  if (o["clientId"].is<const char*>()) { clientId = o["clientId"].as<String>(); clientId.trim(); }
  if (o["pollSec"].is<int>())          pollSec = constrain((int)o["pollSec"], 60, 3600);
  if (o["remindMin"].is<int>())        remindMin = (uint8_t)constrain((int)o["remindMin"], 0, 60);
  // Blank means "keep what is stored", so saving the page does not wipe them.
  if (o["clientSecret"].is<const char*>()) {
    String v = o["clientSecret"].as<String>(); v.trim();
    if (v.length()) clientSecret = v;
  }
  if (o["refreshToken"].is<const char*>()) {
    String v = o["refreshToken"].as<String>(); v.trim();
    if (v.length()) refreshToken = v;
  }  if (o["icsUrl"].is<const char*>()) {
    String v = o["icsUrl"].as<String>(); v.trim();
    // Both providers offer the link as webcal://, which is HTTPS wearing a
    // protocol-handler costume. Accept it as pasted.
    if (v.startsWith("webcal://")) v = "https://" + v.substring(9);
    if (v.length()) icsUrl = v;
  }
}

// ===========================================================================
// Ambient slice
// ===========================================================================
// One row per pattern: the bit, and the key it is saved under. Adding a pattern
// means adding a row here and a tick in the web UI — the same shape as the
// carousel table above, for the same reason.
static const struct { uint8_t pat; const char* key; } kAmbientPatterns[] = {
    {AMB_PAT_LIFE,   "life"},
    {AMB_PAT_PLASMA, "plasma"},
    {AMB_PAT_STARS,  "stars"},
    {AMB_PAT_RAIN,   "rain"},
    {AMB_PAT_SPARKS, "fireworks"},
};
static_assert(sizeof(kAmbientPatterns) / sizeof(kAmbientPatterns[0]) == AMB_PATTERNS,
              "every ambient pattern needs a row here or it can never be turned on");

void AmbientSettings::setDefaults() {
  dwellSec    = DEFAULT_AMBIENT_DWELL_SEC;
  patternSec  = DEFAULT_AMBIENT_PATTERN_SEC;
  shuffle     = true;
  patternMask = AMB_PATTERN_ALL;      // everything on, as before this was settable
}

void AmbientSettings::toJson(JsonObject o) const {
  o["dwellSec"]   = dwellSec;
  o["patternSec"] = patternSec;
  o["shuffle"]    = shuffle;
  JsonObject pats = o["patterns"].to<JsonObject>();
  for (const auto& r : kAmbientPatterns) pats[r.key] = patternOn(r.pat);
}

void AmbientSettings::fromJson(JsonObjectConst o) {
  if (o["dwellSec"].is<int>())   dwellSec = constrain((int)o["dwellSec"], 10, 3600);
  if (o["patternSec"].is<int>()) patternSec = (uint16_t)constrain((int)o["patternSec"], 0, 3600);
  if (o["shuffle"].is<bool>())   shuffle = o["shuffle"];

  if (o["patterns"].is<JsonObjectConst>()) {
    JsonObjectConst pats = o["patterns"].as<JsonObjectConst>();
    uint8_t m = patternMask;
    for (const auto& r : kAmbientPatterns) {
      if (pats[r.key].is<bool>()) {
        if (pats[r.key].as<bool>()) m |= (uint8_t)(1u << r.pat);
        else                       m &= (uint8_t)~(1u << r.pat);
      }
    }
    // Every pattern off would leave a black screen with nothing to explain it,
    // and "I want no ambient" is already sayable by unticking it in the
    // carousel. Refuse the empty set rather than honour it.
    patternMask = m ? m : AMB_PATTERN_ALL;
  }
}

// ===========================================================================
// Work mode slice
// ===========================================================================
void WorkSettings::setDefaults() {
  enabled      = false;
  noMining     = true;    // the sub-switches describe what "work mode" means,
  hideExplicit = true;    // and both are on by default once it is enabled
  blocklist    = "";
}

void WorkSettings::toJson(JsonObject o) const {
  o["enabled"]      = enabled;
  o["noMining"]     = noMining;
  o["hideExplicit"] = hideExplicit;
  o["blocklist"]    = blocklist;
}

void WorkSettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>())      enabled = o["enabled"];
  if (o["noMining"].is<bool>())     noMining = o["noMining"];
  if (o["hideExplicit"].is<bool>()) hideExplicit = o["hideExplicit"];
  if (o["blocklist"].is<const char*>()) blocklist = o["blocklist"].as<String>();
}

// ===========================================================================
// Captive portal slice
// ===========================================================================
void CaptiveSettings::setDefaults() {
  autoAccept = true;
  probeUrl   = "";
  formUrl    = "";
  formBody   = "";
}

void CaptiveSettings::toJson(JsonObject o) const {
  o["autoAccept"] = autoAccept;
  o["probeUrl"]   = probeUrl;
  o["formUrl"]    = formUrl;
  o["formBody"]   = formBody;
}

void CaptiveSettings::fromJson(JsonObjectConst o) {
  if (o["autoAccept"].is<bool>())      autoAccept = o["autoAccept"];
  if (o["probeUrl"].is<const char*>()) { probeUrl = o["probeUrl"].as<String>(); probeUrl.trim(); }
  if (o["formUrl"].is<const char*>())  { formUrl = o["formUrl"].as<String>(); formUrl.trim(); }
  if (o["formBody"].is<const char*>()) { formBody = o["formBody"].as<String>(); formBody.trim(); }
}

// ===========================================================================
// Touch slice
// ===========================================================================
void TouchSettings::setDefaults() {
  enabled   = true;
  gpio      = DEFAULT_TOUCH_GPIO;
  threshold = DEFAULT_TOUCH_THRESHOLD;
}

void TouchSettings::toJson(JsonObject o) const {
  o["enabled"]   = enabled;
  o["gpio"]      = gpio;
  o["threshold"] = threshold;
}

void TouchSettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>())  enabled = o["enabled"];
  if (o["gpio"].is<int>())      gpio = (int8_t)constrain((int)o["gpio"], -1, 39);
  if (o["threshold"].is<int>()) threshold = (uint8_t)constrain((int)o["threshold"], 3, 200);
}

// ===========================================================================
// Top-level settings
// ===========================================================================

// Every mode the carousel can rotate through: the MODE_* constant, the
// config.json key, and whether it is ticked out of the box. Adding a mode used
// to mean editing the struct, the defaults, both JSON directions and the
// dispatcher in main.cpp — five places, and missing any one of them left a
// checkbox that renders, reads back its saved value, and does nothing. The
// miner tick shipped that way, then ambient and flappy after it. One row now.
static const struct {
  uint8_t     mode;
  const char* key;
  bool        defOn;
} kCarousel[] = {
    {MODE_STOCKS,  "carouselTicker",  true},
    {MODE_USAGE,   "carouselUsage",   true},
    {MODE_RADAR,   "carouselRadar",   true},
    {MODE_MINER,   "carouselMiner",   true},
    {MODE_CLOCK,   "carouselClock",   true},
    {MODE_SPOTIFY, "carouselSpotify", true},
    {MODE_AMBIENT, "carouselAmbient", false},  // decoration, not information
    {MODE_BLACKJACK, "carouselBlackjack", false},  // a game should never just appear
    {MODE_CALENDAR,  "carouselCalendar",  false},  // blank until linked; tick it after setup
    {MODE_WEATHER,   "carouselWeather",   false},  // prompts for a location until one is set
};
static_assert(MODE_WEATHER < 16, "carouselMask is 16 bits wide");

void Settings::setDefaults() {
  wifiCount = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETS; i++) {
    wifi[i].ssid = "";
    wifi[i].pass = "";
  }
  apSsid  = DEFAULT_AP_SSID;
  apPass  = DEFAULT_AP_PASS;
  // Unique per device so several SmallTVs on one network don't collide on
  // mDNS out of the box. A hostname saved in config.json overrides this.
  hostname = String(DEFAULT_HOSTNAME) + "-" + String(platformChipId() & 0xFFFF, HEX);
  dnsOverride = "";

  mode = DEFAULT_MODE;
  carouselSec = DEFAULT_CAROUSEL_SEC;
  carouselMask = 0;
  for (const auto& e : kCarousel)
    if (e.defOn) carouselMask |= (uint16_t)(1u << e.mode);
  httpTimeout = DEFAULT_HTTP_TIMEOUT;

  brightness = DEFAULT_BRIGHTNESS;
  autoBrightness = false;
  backlightInverted = TFT_BL_DEFAULT_INVERTED;
  rotation = 0;
  numFont       = NUM_FONT_PIXEL;   // nothing changes until someone opts in

  ticker.setDefaults();
  usage.setDefaults();
  radar.setDefaults();
  miner.setDefaults();
  spotify.setDefaults();
  calendar.setDefaults();
  weather.setDefaults();
  ambient.setDefaults();
  work.setDefaults();
  captive.setDefaults();
  touch.setDefaults();
  clock.setDefaults();
}

// ---------------------------------------------------------------------------
bool settingsBegin() {
  if (LittleFS.begin()) return true;
  // First boot on a fresh chip: format then mount.
  if (LittleFS.format() && LittleFS.begin()) return true;
  return false;
}

bool loadSettings(Settings& s) {
  s.setDefaults();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  settingsApplyJson(s, doc.as<JsonObjectConst>());
  return true;
}

bool saveSettings(const Settings& s) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  settingsToJson(s, root, /*includeSecrets=*/true);

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

void factoryReset(Settings& s) {
  LittleFS.remove(CONFIG_PATH);
  s.setDefaults();
}

// ---------------------------------------------------------------------------
// Mode <-> token, one table serving both directions. This used to be a pair of
// ternary ladders, one in each function, and adding a mode meant remembering
// both: miss the read side and the web UI offers a mode the device silently
// refuses, falling back to the default with no error anywhere. Ambient and
// flappy shipped exactly that way. One line per mode now, in one place.
static const struct { uint8_t mode; const char* tok; } kModeTokens[] = {
    {MODE_STOCKS,   "stocks"},
    {MODE_USAGE,    "usage"},
    {MODE_RADAR,    "radar"},
    {MODE_CAROUSEL, "carousel"},
    {MODE_MINER,    "miner"},
    {MODE_CLOCK,    "clock"},
    {MODE_SPOTIFY,  "spotify"},
    {MODE_AMBIENT,  "ambient"},
    {MODE_BLACKJACK, "blackjack"},
    {MODE_CALENDAR,  "calendar"},
    {MODE_WEATHER,   "weather"},
};

static const char* modeToken(uint8_t m) {
  for (const auto& e : kModeTokens)
    if (e.mode == m) return e.tok;
  return "stocks";
}

static uint8_t modeFromToken(const String& t) {
  for (const auto& e : kModeTokens)
    if (t.equalsIgnoreCase(e.tok)) return e.mode;
  return MODE_STOCKS;
}

void settingsToJson(const Settings& s, JsonObject root, bool includeSecrets) {
  root["hostname"]   = s.hostname;
  root["dns"]        = s.dnsOverride;

  // WiFi networks. Passwords only reach the config file, never the web API.
  JsonArray wf = root["wifi"].to<JsonArray>();
  for (uint8_t i = 0; i < s.wifiCount; i++) {
    JsonObject e = wf.add<JsonObject>();
    e["ssid"]    = s.wifi[i].ssid;
    e["passSet"] = s.wifi[i].pass.length() > 0;
    if (includeSecrets) e["pass"] = s.wifi[i].pass;
  }
  // Legacy mirror of the primary network, kept for one release so a firmware
  // downgrade still finds its WiFi in config.json.
  root["staSsid"]    = s.wifiCount ? s.wifi[0].ssid : "";
  root["staPassSet"] = s.wifiCount && s.wifi[0].pass.length() > 0;
  root["apSsid"]     = s.apSsid;
  root["apPassSet"]  = s.apPass.length() > 0;
  if (includeSecrets) {
    root["staPass"]  = s.wifiCount ? s.wifi[0].pass : "";
    root["apPass"]   = s.apPass;
  }

  // Mode + shared HTTP/display
  root["mode"]              = modeToken(s.mode);
  root["carouselSec"]       = s.carouselSec;
  for (const auto& e : kCarousel) root[e.key] = s.carouselHas(e.mode);
  root["httpTimeout"]       = s.httpTimeout;
  root["brightness"]        = s.brightness;
  root["autoBrightness"]    = s.autoBrightness;
  root["backlightInverted"] = s.backlightInverted;
  root["rotation"]          = s.rotation;
  root["numFont"]           = s.numFont;

  // Feature slices
  s.ticker.toJson(root["ticker"].to<JsonObject>());
  s.usage.toJson(root["usage"].to<JsonObject>());
  s.radar.toJson(root["radar"].to<JsonObject>());
  s.miner.toJson(root["miner"].to<JsonObject>());
  s.spotify.toJson(root["spotify"].to<JsonObject>(), includeSecrets);
  s.calendar.toJson(root["calendar"].to<JsonObject>(), includeSecrets);
  s.weather.toJson(root["weather"].to<JsonObject>());
  s.ambient.toJson(root["ambient"].to<JsonObject>());
  s.work.toJson(root["work"].to<JsonObject>());
  s.captive.toJson(root["captive"].to<JsonObject>());
  s.touch.toJson(root["touch"].to<JsonObject>());
  s.clock.toJson(root["clock"].to<JsonObject>());
}

// Apply only the keys that are present (partial update friendly). Accepts both
// the nested layout and the legacy flat layout (feature keys at the top level).
void settingsApplyJson(Settings& s, JsonObjectConst root) {
  if (root["hostname"].is<const char*>()) s.hostname = root["hostname"].as<String>();
  if (root["dns"].is<const char*>())      s.dnsOverride = root["dns"].as<String>();

  if (root["wifi"].is<JsonArrayConst>()) {
    // The list is authoritative when present (order = try priority, missing
    // row = deletion). A blank password keeps the stored one, matched by SSID
    // so rows survive reordering.
    WifiCred old[MAX_WIFI_NETS];
    uint8_t oldCount = s.wifiCount;
    for (uint8_t i = 0; i < oldCount; i++) old[i] = s.wifi[i];

    s.wifiCount = 0;
    for (JsonObjectConst e : root["wifi"].as<JsonArrayConst>()) {
      if (s.wifiCount >= MAX_WIFI_NETS) break;
      const char* ssid = e["ssid"] | "";
      if (!ssid[0]) continue;                // skip blank rows
      WifiCred& dst = s.wifi[s.wifiCount];
      dst.ssid = ssid;
      const char* pass = e["pass"] | "";
      dst.pass = pass;
      if (!pass[0])
        for (uint8_t i = 0; i < oldCount; i++)
          if (old[i].ssid == dst.ssid) { dst.pass = old[i].pass; break; }
      s.wifiCount++;
    }
  } else if (root["staSsid"].is<const char*>()) {
    // Legacy single-network layout (pre-2.4 config.json or an old cached web
    // page): it becomes/updates the primary network, extras stay untouched.
    String ssid = root["staSsid"].as<String>();
    if (ssid.length()) {
      s.wifi[0].ssid = ssid;
      if (root["staPass"].is<const char*>()) {
        String p = root["staPass"].as<String>();
        if (p.length() > 0) s.wifi[0].pass = p;   // blank = keep
      }
      if (s.wifiCount < 1) s.wifiCount = 1;
    }
  }
  if (root["apSsid"].is<const char*>()) s.apSsid = root["apSsid"].as<String>();
  // AP password: apply as-is when present (empty allowed => open AP).
  if (root["apPass"].is<const char*>()) s.apPass = root["apPass"].as<String>();

  if (root["mode"].is<const char*>()) {
    s.mode = modeFromToken(root["mode"].as<String>());
  }
  if (root["carouselSec"].is<int>())      s.carouselSec = constrain((int)root["carouselSec"], 5, 3600);
  for (const auto& e : kCarousel) {
    if (!root[e.key].is<bool>()) continue;          // absent = leave as-is
    const uint16_t bit = (uint16_t)(1u << e.mode);
    if (root[e.key].as<bool>()) s.carouselMask |= bit;
    else                        s.carouselMask &= (uint16_t)~bit;
  }

  if (root["httpTimeout"].is<int>())        s.httpTimeout = constrain((int)root["httpTimeout"], 1000, 20000);
  if (root["brightness"].is<int>())         s.brightness = constrain((int)root["brightness"], 0, 100);
  if (root["autoBrightness"].is<bool>())    s.autoBrightness = root["autoBrightness"];
  if (root["backlightInverted"].is<bool>()) s.backlightInverted = root["backlightInverted"];
  if (root["rotation"].is<int>())           s.rotation = (uint8_t)(((int)root["rotation"]) & 3);
  if (root["numFont"].is<int>())            s.numFont = (uint8_t)constrain((int)root["numFont"], 0, 1);

  // Feature slices: prefer the nested object; fall back to the top level so a
  // legacy flat config.json (or a legacy POST) still applies. The old shared
  // "pollSec" thus seeds both ticker and usage cadence on first upgrade.
  JsonObjectConst t = root["ticker"].is<JsonObjectConst>() ? root["ticker"].as<JsonObjectConst>() : root;
  s.ticker.fromJson(t);
  JsonObjectConst u = root["usage"].is<JsonObjectConst>() ? root["usage"].as<JsonObjectConst>() : root;
  s.usage.fromJson(u);
  // Radar/miner have no legacy flat layout; only apply when nested.
  if (root["radar"].is<JsonObjectConst>()) s.radar.fromJson(root["radar"].as<JsonObjectConst>());
  if (root["miner"].is<JsonObjectConst>()) s.miner.fromJson(root["miner"].as<JsonObjectConst>());
  if (root["spotify"].is<JsonObjectConst>()) s.spotify.fromJson(root["spotify"].as<JsonObjectConst>());
  if (root["calendar"].is<JsonObjectConst>()) s.calendar.fromJson(root["calendar"].as<JsonObjectConst>());
  if (root["weather"].is<JsonObjectConst>())  s.weather.fromJson(root["weather"].as<JsonObjectConst>());
  if (root["ambient"].is<JsonObjectConst>()) s.ambient.fromJson(root["ambient"].as<JsonObjectConst>());
  if (root["work"].is<JsonObjectConst>()) s.work.fromJson(root["work"].as<JsonObjectConst>());
  if (root["captive"].is<JsonObjectConst>()) s.captive.fromJson(root["captive"].as<JsonObjectConst>());
  if (root["touch"].is<JsonObjectConst>()) s.touch.fromJson(root["touch"].as<JsonObjectConst>());
  if (root["clock"].is<JsonObjectConst>()) s.clock.fromJson(root["clock"].as<JsonObjectConst>());
}
