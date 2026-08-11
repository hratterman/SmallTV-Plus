#include "StockClient.h"
#include "NetFetch.h"

// Lets the existing Stream-based parsers read a response that arrived as text
// over the tether, without giving any of them a second implementation.
namespace {
class StringStream : public Stream {
 public:
  explicit StringStream(const String& s) : s_(s) {}
  int available() override { return (int)(s_.length() - pos_); }
  int read() override { return pos_ < s_.length() ? (uint8_t)s_[pos_++] : -1; }
  int peek() override { return pos_ < s_.length() ? (uint8_t)s_[pos_] : -1; }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }
 private:
  const String& s_;
  unsigned pos_ = 0;
};
}  // namespace

// Why the ticker can be blank, in the device's own words. Only set on the
// tether path so far, where the reason is a fixed property of the source rather
// than a transient network problem.
static char s_note[64] = "";
void stocksSetNote(const char* n) {
  const char* v = n ? n : "";
  if (!strcmp(s_note, v)) return;          // once per change, not once per poll
  strlcpy(s_note, v, sizeof(s_note));
  if (s_note[0]) Serial.printf("[ticker] %s\n", s_note);
}
const char* stocksNote() { return s_note; }

#include "Platform.h"
#include <ArduinoJson.h>
#include <math.h>

static StockData g_stocks[MAX_SYMBOLS];
static uint8_t   g_count = 0;

static bool     g_refreshing = false;
static uint8_t  g_fetchIdx = 0;
static uint32_t g_nextPollMs = 0;
static uint8_t  g_retryBurst = 0;   // consecutive fast retries after a failed cycle

// ---------------------------------------------------------------------------
void stocksInit(const Settings& s) {
  g_count = s.ticker.symbolCount;
  for (uint8_t i = 0; i < g_count; i++) {
    g_stocks[i].clear();
    strlcpy(g_stocks[i].symbol, s.ticker.symbols[i].symbol, MAX_SYMBOL_LEN);
    g_stocks[i].source = s.ticker.symbols[i].source;
    g_stocks[i].qty  = s.ticker.symbols[i].qty;
    g_stocks[i].cost = s.ticker.symbols[i].cost;
    g_stocks[i].userNamed = (s.ticker.symbols[i].name[0] != 0);
    strlcpy(g_stocks[i].name,
            g_stocks[i].userNamed ? s.ticker.symbols[i].name : s.ticker.symbols[i].symbol,
            MAX_NAME_LEN);
  }
  g_refreshing = false;
  g_nextPollMs = millis();
}

void stocksForceRefresh() {
  g_nextPollMs = millis();
  g_refreshing = false;
}

uint8_t          stocksCount()        { return g_count; }
const StockData& stockAt(uint8_t i)   { return g_stocks[i]; }

bool stockDisplayChange(const StockData& d, const TickerSettings& t,
                        float& chg, float& pct, bool* onRange) {
  if (t.changeOnRange && d.sparkCount >= 1 && d.spark[0] > 0 && d.price > 0) {
    chg = d.price - d.spark[0];
    pct = chg / d.spark[0] * 100.0f;
    if (onRange) *onRange = true;
    return true;
  }
  if (onRange) *onRange = false;
  chg = d.change;
  pct = d.changePct;
  return d.hasChange;
}

// ---- URL helpers ----------------------------------------------------------
static String urlEncode(const char* src) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (const char* p = src; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

static String buildWebhookUrl(const Settings& s, const char* symbol) {
  String url = s.ticker.webhookUrl;
  char sep = (url.indexOf('?') >= 0) ? '&' : '?';
  url += sep;
  url += "symbol=" + urlEncode(symbol);
  url += "&range=" + urlEncode(s.ticker.range.c_str());
  url += "&points=" + String(s.ticker.points);
  return url;
}

// Map the chart timeframe to a sensible Yahoo candle interval (mirrors the
// reference n8n workflow so the sparkline has a useful number of points).
static const char* yahooInterval(const String& r) {
  if (r == "1d")  return "5m";
  if (r == "5d")  return "30m";
  if (r == "1mo" || r == "3mo" || r == "6mo" || r == "ytd") return "1d";
  if (r == "1y"  || r == "2y")  return "1wk";
  if (r == "5y"  || r == "10y" || r == "max") return "1mo";
  return "1d";
}

static String buildYahooUrl(const Settings& s, const char* host, const char* symbol) {
  String range = s.ticker.range;
  range.toLowerCase();
  if (range.length() == 0) range = "1d";
  String url = F("https://");
  url += host;
  url += F(YAHOO_CHART_PATH);
  url += urlEncode(symbol);          // e.g. AAPL, NESN.SW, BTC-USD, EURUSD=X
  url += F("?range=");
  url += range;
  url += F("&interval=");
  url += yahooInterval(range);
  return url;
}

// Short, display-safe currency prefix. The built-in bitmap font has no glyphs
// for symbols like €, so non-USD currencies are shown as their ISO code.
//
// EVERY source must run its currency through here rather than storing the raw
// code. Not doing so is what put "USD724.48" on the screen: the stockanalysis
// parser knew its listings were all US and wrote "USD" straight into the field,
// so the one place that turns a code into a symbol never saw it.
static void currencyPrefix(const char* code, char* out, size_t n) {
  if (!code || !code[0]) { out[0] = 0; return; }
  if (!strcmp(code, "USD")) { strlcpy(out, "$", n); return; }
  snprintf(out, n, "%s ", code);     // "CHF 79.73", "EUR 1.08", ...
}

// ---- URL builders: cash.ch --------------------------------------------------
// cash.ch answers hand-written GraphQL sent as a plain GET ?query=... — no
// auth, no cookies, no required headers (see config.h). The symbol is the
// cash.ch listing key (valor-marketId-currencyId).

// Daily closes to request per chart timeframe. The server trims to the newest
// N (max=N) inside a fixed catch-all from/to window, so the device needs no
// clock; asking for more than cash.ch stores (~6 months) just returns it all.
static uint16_t cashRangeDays(const String& r) {
  if (r == "1d")  return 2;          // >=2 points so a line can be drawn
  if (r == "5d")  return 5;
  if (r == "1mo") return 22;
  if (r == "3mo") return 65;
  if (r == "6mo" || r == "ytd") return 130;
  if (r == "1y")  return 260;
  return 400;                        // 2y/5y/max: everything available
}

static String buildCashUrl(const String& query) {
  String url = F("https://" CASH_GQL_HOST CASH_GQL_PATH "?query=");
  url += urlEncode(query.c_str());
  return url;
}

static String buildCashQuoteUrl(const char* symbol) {
  String q = F("query{quoteList(listingKeys:\"");
  q += symbol;
  q += F("\"){quoteList{edges{node{...on Instrument{"
         "lval iNetVperprV perfPercentage mCur mShortName}}}}}}");
  return buildCashUrl(q);
}

static String buildCashChartUrl(const Settings& s, const char* symbol) {
  String q = F("query{integration{solid{chart(listingKey:\"");
  q += symbol;
  q += F("\" frequency:\"1d\" from:\"2020-01-01\" to:\"2100-01-01\" max:\"");
  q += String(cashRangeDays(s.ticker.range));
  q += F("\"){timeserie{prices{close}}}}}}");
  return buildCashUrl(q);
}

// ---- URL builder: GitHub static quotes ------------------------------------
// The per-symbol file published by the quotes workflow. Same JSON as a webhook.
static String buildSaQuoteUrl(const char* symbol) {
  String url = F(SA_QUOTE_URL);
  url += urlEncode(symbol);
  return url;
}

static String buildSaHistUrl(const char* symbol) {
  String url = F(SA_HIST_URL);
  url += urlEncode(symbol);
  url += F(SA_HIST_TAIL);
  return url;
}

static String buildGithubUrl(const char* symbol) {
  String url = F(GH_QUOTES_BASE);
  url += urlEncode(symbol);
  url += F(".json");
  return url;
}

// ---- parse: custom webhook contract ---------------------------------------
static bool parseWebhook(const Settings& s, StockData& d, Stream& stream) {
  // Filter so unexpected/large fields don't blow up the heap.
  JsonDocument filter;
  filter["symbol"] = true;
  filter["name"] = true;
  filter["price"] = true;
  filter["currency"] = true;
  filter["change"] = true;
  filter["changePct"] = true;
  filter["range"] = true;
  filter["ok"] = true;
  filter["spark"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  if (doc["ok"].is<bool>() && doc["ok"].as<bool>() == false) return false;
  if (!doc["price"].is<float>() && !doc["price"].is<int>()) return false;

  float price = doc["price"].as<float>();
  if (isnan(price)) return false;
  d.price = price;

  if (!d.userNamed && doc["name"].is<const char*>())
    strlcpy(d.name, doc["name"].as<const char*>(), MAX_NAME_LEN);
  currencyPrefix(doc["currency"] | "", d.currency, sizeof(d.currency));

  const char* rng = doc["range"] | s.ticker.range.c_str();
  strlcpy(d.rangeLabel, rng, sizeof(d.rangeLabel));

  bool hasChg = doc["change"].is<float>() || doc["change"].is<int>();
  bool hasPct = doc["changePct"].is<float>() || doc["changePct"].is<int>();
  d.hasChange = hasChg || hasPct;
  if (hasChg) d.change = doc["change"].as<float>();
  if (hasPct) d.changePct = doc["changePct"].as<float>();
  if (d.hasChange) {
    if (!hasPct && hasChg) {               // derive % from absolute change
      float prev = price - d.change;
      d.changePct = (prev != 0) ? (d.change / prev * 100.0f) : 0;
    } else if (hasPct && !hasChg) {        // derive absolute change from %
      d.change = price * d.changePct / (100.0f + d.changePct);
    }
  }

  d.sparkCount = 0;
  if (doc["spark"].is<JsonArrayConst>()) {
    for (JsonVariantConst v : doc["spark"].as<JsonArrayConst>()) {
      if (!v.is<float>() && !v.is<int>()) continue;   // skip nulls, like the other parsers
      if (d.sparkCount >= MAX_SPARK_POINTS) break;
      d.spark[d.sparkCount++] = v.as<float>();
    }
  }

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// ---- parse: Yahoo Finance chart payload -----------------------------------
static bool parseYahoo(const Settings& s, StockData& d, Stream& stream) {
  // Keep only the handful of fields we need; the full payload is large and the
  // `meta` object alone has many nested members we don't care about.
  JsonDocument filter;
  JsonObject fmeta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  fmeta["regularMarketPrice"] = true;
  fmeta["chartPreviousClose"] = true;
  fmeta["previousClose"]      = true;
  fmeta["currency"]           = true;
  fmeta["shortName"]          = true;
  fmeta["longName"]           = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst res  = doc["chart"]["result"][0];
  JsonObjectConst meta = res["meta"];
  if (meta.isNull()) return false;                 // bad symbol => result null
  if (!meta["regularMarketPrice"].is<float>() &&
      !meta["regularMarketPrice"].is<int>()) return false;

  float price = meta["regularMarketPrice"].as<float>();
  if (isnan(price)) return false;
  d.price = price;

  currencyPrefix(meta["currency"] | "", d.currency, sizeof(d.currency));

  if (!d.userNamed) {
    const char* nm = meta["shortName"] | (meta["longName"] | (const char*)d.symbol);
    if (nm && nm[0]) strlcpy(d.name, nm, MAX_NAME_LEN);
  }

  // Change vs the previous close.
  float prev = NAN;
  if (meta["chartPreviousClose"].is<float>() || meta["chartPreviousClose"].is<int>())
    prev = meta["chartPreviousClose"].as<float>();
  else if (meta["previousClose"].is<float>() || meta["previousClose"].is<int>())
    prev = meta["previousClose"].as<float>();

  if (!isnan(prev) && prev != 0) {
    d.change = price - prev;
    d.changePct = d.change / prev * 100.0f;
    d.hasChange = true;
  } else {
    d.hasChange = false;
  }

  String rl = s.ticker.range;
  rl.toUpperCase();
  strlcpy(d.rangeLabel, rl.c_str(), sizeof(d.rangeLabel));

  // Sparkline from indicators.quote[0].close (oldest -> newest; may hold nulls).
  // Downsample on the fly to at most `points` evenly-spaced samples.
  d.sparkCount = 0;
  JsonArrayConst closes = res["indicators"]["quote"][0]["close"];
  uint16_t want = s.ticker.points;
  if (want > MAX_SPARK_POINTS) want = MAX_SPARK_POINTS;
  if (!closes.isNull() && want >= 2) {
    uint16_t valid = 0;
    for (JsonVariantConst v : closes) if (!v.isNull()) valid++;
    if (valid >= 2) {
      if (valid <= want) {
        for (JsonVariantConst v : closes) {
          if (v.isNull()) continue;
          if (d.sparkCount >= MAX_SPARK_POINTS) break;
          d.spark[d.sparkCount++] = v.as<float>();
        }
      } else {
        uint16_t i = 0, k = 0;
        float last = 0;
        for (JsonVariantConst v : closes) {
          if (v.isNull()) continue;
          last = v.as<float>();
          // k-th output maps to valid-index round(k*(valid-1)/(want-1)).
          uint16_t target =
              (uint16_t)(((uint32_t)k * (valid - 1) + (want - 1) / 2) / (want - 1));
          if (i == target && k < want) { d.spark[d.sparkCount++] = last; k++; }
          i++;
        }
        if (d.sparkCount > 0) d.spark[d.sparkCount - 1] = last;  // pin newest
      }
    }
  }

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// ---- parse: cash.ch quote ---------------------------------------------------
// {"data":{"quoteList":{"quoteList":{"edges":[{"node":{"lval":"1086.51",
//  "iNetVperprV":"7.36","perfPercentage":"0.68","mCur":"USD",...}}]}}}}
// Numeric fields arrive as JSON *strings*, so read them as text and atof().
static bool parseCashQuote(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  JsonObject node =
      filter["data"]["quoteList"]["quoteList"]["edges"][0]["node"].to<JsonObject>();
  node["lval"]           = true;   // last value (price)
  node["iNetVperprV"]    = true;   // absolute day change
  node["perfPercentage"] = true;   // day change in %
  node["mCur"]           = true;
  node["mShortName"]     = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst n = doc["data"]["quoteList"]["quoteList"]["edges"][0]["node"];
  const char* lval = n["lval"] | "";       // empty => unknown key / no fix yet
  if (!lval[0]) return false;
  float price = atof(lval);
  if (isnan(price) || price <= 0) return false;
  d.price = price;

  currencyPrefix(n["mCur"] | "", d.currency, sizeof(d.currency));

  if (!d.userNamed) {
    const char* nm = n["mShortName"] | (const char*)d.symbol;
    if (nm && nm[0]) strlcpy(d.name, nm, MAX_NAME_LEN);
  }

  const char* chg = n["iNetVperprV"]    | "";
  const char* pct = n["perfPercentage"] | "";
  d.hasChange = chg[0] || pct[0];
  if (d.hasChange) {
    d.change    = atof(chg);
    d.changePct = pct[0] ? atof(pct) : 0;
    if (!pct[0]) {                          // derive % from the absolute change
      float prev = price - d.change;
      d.changePct = (prev != 0) ? (d.change / prev * 100.0f) : 0;
    }
  }

  String rl = s.ticker.range;
  rl.toUpperCase();
  strlcpy(d.rangeLabel, rl.c_str(), sizeof(d.rangeLabel));

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// ---- parse: cash.ch daily-close series --------------------------------------
// {"data":{"integration":{"solid":{"chart":{"timeserie":{"prices":
//  [{"close":998.45},...]}}}}}} — closes are real JSON numbers here (unlike
// the quote), oldest -> newest. Downsampling mirrors the Yahoo parser.
// {"status":200,"data":{"p":710.71,"c":4.31,"cp":0.61,"cl":706.4, ...}}
//   p  last price      c  change in currency
//   cp change percent  cl previous close
static bool parseSaQuote(const Settings& s, StockData& d, Stream& src) {
  JsonDocument filter;
  JsonObject fd = filter["data"].to<JsonObject>();
  fd["p"] = true; fd["c"] = true; fd["cp"] = true; fd["cl"] = true;
  filter["status"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, src, DeserializationOption::Filter(filter))) return false;
  if ((doc["status"] | 0) != 200) return false;          // 404 for a bad symbol

  JsonObjectConst q = doc["data"];
  if (q.isNull() || !(q["p"].is<float>() || q["p"].is<int>())) return false;
  const float price = q["p"].as<float>();
  if (isnan(price) || price <= 0) return false;
  d.price = price;

  // US listings only, which is the whole of what this source covers.
  currencyPrefix("USD", d.currency, sizeof(d.currency));

  if (q["c"].is<float>() || q["c"].is<int>())   d.change    = q["c"].as<float>();
  if (q["cp"].is<float>() || q["cp"].is<int>()) d.changePct = q["cp"].as<float>();
  // Prefer deriving the pair from the previous close when it is offered: it
  // keeps change and percent consistent with each other and with the price.
  if (q["cl"].is<float>() || q["cl"].is<int>()) {
    const float prev = q["cl"].as<float>();
    if (prev > 0) { d.change = price - prev; d.changePct = d.change / prev * 100.0f; }
  }
  d.hasChange = true;
  // The span the chart covers is decided by the history parse below, not here.
  // Clear it so a label left over from a different source cannot outlive it.
  d.rangeLabel[0] = 0;

  // Without these the symbol never leaves "loading...", however well the parse
  // went. Every other quote parser ends exactly here; this one did not.
  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// {"status":200,"data":[{"t":"2026-08-07","c":710.71}, ...]} newest first, so
// the sparkline is filled backwards.
static bool parseSaHist(const Settings& s, StockData& d, Stream& src) {
  JsonDocument filter;
  filter["data"][0]["c"] = true;
  filter["status"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, src, DeserializationOption::Filter(filter))) return false;
  if ((doc["status"] | 0) != 200) return false;

  JsonArrayConst bars = doc["data"];
  if (bars.isNull()) return false;

  uint16_t want = s.ticker.points;
  if (want > MAX_SPARK_POINTS) want = MAX_SPARK_POINTS;
  if (want < 2) return true;                    // chart not wanted; quote stands

  // Newest first: take the most recent `want` and write them in reverse, so the
  // sparkline reads left-to-right in time like every other source's.
  float tmp[MAX_SPARK_POINTS];
  uint16_t n = 0;
  for (JsonObjectConst b : bars) {
    if (n >= want) break;
    if (!b["c"].is<float>() && !b["c"].is<int>()) continue;
    tmp[n++] = b["c"].as<float>();
  }
  if (n < 2) return false;                      // keep whatever was there

  d.sparkCount = 0;
  for (uint16_t i = 0; i < n; i++) d.spark[d.sparkCount++] = tmp[n - 1 - i];

  // This source ignores the range setting, so the only honest label is one
  // derived from the bars that actually got drawn: ~21 trading days to the
  // month, rounded to nearest. Hardcoding it would be the same bug this
  // codebase keeps producing — one fact written in two places, one of which
  // stops being true the moment `points` changes.
  const unsigned days   = d.sparkCount;
  const unsigned months = (days + 10) / 21;
  if (months) snprintf(d.rangeLabel, sizeof(d.rangeLabel), "%uM", months);
  else        snprintf(d.rangeLabel, sizeof(d.rangeLabel), "%uD", days);
  return true;
}

static bool parseCashChart(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  filter["data"]["integration"]["solid"]["chart"]["timeserie"]["prices"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonArrayConst prices =
      doc["data"]["integration"]["solid"]["chart"]["timeserie"]["prices"];
  if (prices.isNull()) return false;         // unknown key / empty series

  uint16_t want = s.ticker.points;
  if (want > MAX_SPARK_POINTS) want = MAX_SPARK_POINTS;
  if (want < 2) return true;                 // chart not wanted; keep quote data

  uint16_t valid = 0;
  for (JsonObjectConst p : prices)
    if (p["close"].is<float>() || p["close"].is<int>()) valid++;
  if (valid < 2) return false;               // keep the previous sparkline

  d.sparkCount = 0;
  if (valid <= want) {
    for (JsonObjectConst p : prices) {
      if (!p["close"].is<float>() && !p["close"].is<int>()) continue;
      if (d.sparkCount >= MAX_SPARK_POINTS) break;
      d.spark[d.sparkCount++] = p["close"].as<float>();
    }
  } else {
    uint16_t i = 0, k = 0;
    float last = 0;
    for (JsonObjectConst p : prices) {
      if (!p["close"].is<float>() && !p["close"].is<int>()) continue;
      last = p["close"].as<float>();
      // k-th output maps to valid-index round(k*(valid-1)/(want-1)).
      uint16_t target =
          (uint16_t)(((uint32_t)k * (valid - 1) + (want - 1) / 2) / (want - 1));
      if (i == target && k < want) { d.spark[d.sparkCount++] = last; k++; }
      i++;
    }
    if (d.sparkCount > 0) d.spark[d.sparkCount - 1] = last;  // pin newest
  }
  return true;
}

// ---- one HTTP(S) GET + parse ----------------------------------------------
enum ParseKind : uint8_t { PARSE_WEBHOOK, PARSE_YAHOO, PARSE_CASH_QUOTE, PARSE_CASH_CHART,
                          PARSE_GITHUB, PARSE_SA_QUOTE, PARSE_SA_HIST };

// cash.ch is the only host we let negotiate ECDHE, and only the first handshake
// pays for it: this session resumes the rest for ~23 h.
static TlsSession g_cashSession;

// The one place that maps a kind to a parser. It is deliberately not inlined:
// the tether path and the WiFi path both dispatch, and at -Os the compiler
// happily pastes all six parsers into each of them — about 3 KB of flash to say
// the same thing twice. Both callers hand it a Stream, so there is nothing to
// specialise on either.
static __attribute__((noinline)) bool parseInto(ParseKind kind, const Settings& s, StockData& d, Stream& src) {
  switch (kind) {
    case PARSE_YAHOO:      return parseYahoo(s, d, src);
    case PARSE_SA_QUOTE:   return parseSaQuote(s, d, src);
    case PARSE_SA_HIST:    return parseSaHist(s, d, src);
    case PARSE_CASH_QUOTE: return parseCashQuote(s, d, src);
    case PARSE_CASH_CHART: return parseCashChart(s, d, src);
    default:               return parseWebhook(s, d, src);
  }
}

static const char* kindTag(ParseKind k) {
  switch (k) {
    case PARSE_YAHOO:                          return "yahoo";
    case PARSE_SA_QUOTE: case PARSE_SA_HIST:   return "stockanalysis";
    case PARSE_CASH_QUOTE: case PARSE_CASH_CHART: return "cash.ch";
    case PARSE_GITHUB:                         return "github";
    default:                                   return "webhook";
  }
}

static bool fetchUrl(const Settings& s, const String& url, ParseKind kind, StockData& d) {
  bool https = url.startsWith("https://");
  bool cash = (kind == PARSE_CASH_QUOTE || kind == PARSE_CASH_CHART);

  std::unique_ptr<NetClient> client;
  if (https) {
    // Per-source TLS shaping (ESP8266; the ESP32 ignores the hints):
    //  - cash.ch: must do ECDHE. Keep it cheap — 512 B buffers (it honors MFLN)
    //    and session resumption, and require a large CONTIGUOUS free block up
    //    front so a fragmented heap skips the fetch instead of crashing inside
    //    the handshake. The full cipher list (default) is left on for it.
    //  - Yahoo / GitHub / webhook: forced to the cheap static-RSA suites, so
    //    those handshakes stay as light as the old BASIC build.
    if (cash) {
      if (platformMaxFreeBlock() < 16000) {   // largest contiguous block, not total
        stocksSetNote("low heap: fetch skipped");
        return false;
      }
      client.reset(platformMakeSecureClient(512, &g_cashSession, 512, /*cheapCiphers=*/false));
    } else {
      // raw.githubusercontent.com sends a ~4 KB cert record and won't negotiate
      // MFLN, so it needs a bigger receive buffer than Yahoo's small records.
      uint16_t rx = (kind == PARSE_GITHUB) ? GH_QUOTES_RXBUF : 2048;
      if (ESP.getFreeHeap() < (uint32_t)rx + 12000) {
        stocksSetNote("low heap: fetch skipped");
        return false;
      }
      client.reset(platformMakeSecureClient(rx, nullptr, 512, /*cheapCiphers=*/true));
    }
  } else {
    client.reset(new WiFiClient());
  }

  // Tethered: the request goes down the cable and comes back as text, which the
  // same parsers read through a Stream wrapper. The WiFi path below is left
  // exactly as it was — its per-source TLS buffer sizes and cipher choices are
  // tuned for the ESP8266's heap, and a generic client would undo that work.
#if WITH_TETHER
  if (netFetchTethered()) {
    // Yahoo sends no access-control-allow-origin, on any response, so a page
    // may not read it however willing — measured, not assumed. Asking anyway
    // produced six doomed requests per poll cycle and a wall of failures in
    // the tether log. Fail here instead and say what to change.
    if (kind == PARSE_YAHOO) {
      // stepSymbol substitutes stockanalysis.com before it gets here, so
      // reaching this means something asked for Yahoo directly.
      stocksSetNote("Yahoo can't be read by a browser");
      return false;
    }
    const char* hdrs =
        (kind == PARSE_YAHOO)  ? "Accept: application/json\nUser-Agent: " YAHOO_USER_AGENT :
        (kind == PARSE_GITHUB) ? "Accept: application/json\nUser-Agent: " FW_NAME :
                                 "Accept: application/json";
    String raw;
    const NetFetchResult r = netFetchToString(url.c_str(), false, hdrs, nullptr, 0,
                                              raw, 12288, s.httpTimeout);
    if (!r.ok) {
      // Yahoo does not send CORS headers, so a browser cannot fetch it however
      // willing it is. Say that rather than leaving a bare failure, because the
      // remedy is a setting the user can change.
      if (kind == PARSE_YAHOO)
        stocksSetNote("Yahoo can't be fetched over the tether - use the GitHub source");
      else
        stocksSetNote(r.error);
      return false;
    }
    StringStream ss(raw);
    return parseInto(kind, s, d, ss);
  }
#endif

  {
    // Same pre-resolve NetFetch does: a filtering resolver's block answer must
    // not masquerade as "connection refused" in the note.
    char derr[64];
    if (!netDnsPrecheck(url.c_str(), derr, sizeof(derr))) {
      stocksSetNote(derr);
      return false;
    }
  }

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  // HTTP/1.0 so the server can't reply with chunked framing: the parsers read
  // the raw stream via getStream(), which neither core de-chunks. Yahoo chunks
  // its HTTP/1.1 responses, which broke Yahoo tickers on the ESP32 targets.
  http.useHTTP10(true);
  if (!http.begin(*client, url)) return false;
  http.addHeader("Accept", "application/json");
  if (kind == PARSE_YAHOO) {
    http.setUserAgent(F(YAHOO_USER_AGENT));   // empty UA => HTTP 429 from Yahoo
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  } else if (kind == PARSE_GITHUB) {
    http.setUserAgent(F(FW_NAME));            // GitHub rejects an empty UA
  } else if (kind != PARSE_WEBHOOK) {
    http.setUserAgent(F(CASH_USER_AGENT));    // cash.ch requires none; sent to be identifiable
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char note[72];
    if (code < 0)
      snprintf(note, sizeof(note), "%s: %s", kindTag(kind),
               HTTPClient::errorToString(code).c_str());
    else
      snprintf(note, sizeof(note), "%s: HTTP %d", kindTag(kind), code);
    stocksSetNote(note);
    http.end();
    return false;
  }

  // PARSE_GITHUB falls to parseWebhook inside parseInto: same JSON shape.
  const bool ok = parseInto(kind, s, d, http.getStream());
  http.end();
  if (!ok) {
    char note[72];
    snprintf(note, sizeof(note), "%s: unexpected reply", kindTag(kind));
    stocksSetNote(note);
  }
  return ok;
}

// ---- fetch one symbol, one network request per call -----------------------
// A symbol may need several requests (Yahoo mirror retry; cash.ch quote, its
// retry, then the chart). Each is a separate step so only ONE TLS handshake
// runs per service() call: a full-BearSSL ECDHE handshake takes ~1 s on the
// ESP8266, and chaining two or three in one loop() iteration starved the web
// server and tripped the watchdog. g_fetchPhase tracks the step for g_fetchIdx.
static uint8_t g_fetchPhase = 0;

// Yahoo sometimes refuses connections from a whole network: a phone hotspot
// puts thousands of users behind one carrier-NAT address, and Yahoo turns the
// shared address away outright ("connection refused" while every other host
// answers). After two full round-trip failures in a row the client latches
// the same stockanalysis.com substitution the tether path makes, and retries
// Yahoo when the latch expires. The saved configuration is untouched.
static uint8_t  s_yahooFails = 0;
static uint32_t s_yahooAvoidUntil = 0;
static const uint32_t YAHOO_AVOID_MS = 10UL * 60UL * 1000UL;

// Returns true when this symbol is finished (caller advances to the next).
static bool stepSymbol(const Settings& s, StockData& d) {
  uint8_t source = d.source;

#if WITH_TETHER
  // Plug and play rather than a setting to remember: while tethered, requests
  // go out through a browser tab, and Yahoo sends no access-control-allow-origin
  // on any response — so it is not merely slow there, it is unreachable. Swap
  // to the one source that answers a browser, with the same symbol, and put it
  // back the moment the cable is gone. The saved configuration is untouched.
  if (source == SRC_YAHOO && netFetchTethered()) {
    source = SRC_SA;
    stocksSetNote("tethered: Yahoo blocks browsers, using stockanalysis.com");
  }
#endif
  if (source == SRC_YAHOO && s_yahooAvoidUntil &&
      (int32_t)(millis() - s_yahooAvoidUntil) < 0) {
    source = SRC_SA;
    stocksSetNote("yahoo unreachable - using stockanalysis.com");
  }

  if (source == SRC_SA) {
    if (g_fetchPhase == 0) {            // quote: price and the day's change
      if (!fetchUrl(s, buildSaQuoteUrl(d.symbol), PARSE_SA_QUOTE, d)) { d.error = true; return true; }
      g_fetchPhase = 1;
      return false;
    }
    // Sparkline. Non-fatal, exactly as the cash.ch chart is: a stale chart
    // beside a fresh price beats no price at all.
    if ((s.ticker.showChart || (s.ticker.changeOnRange && s.ticker.showChange)) &&
        s.ticker.points >= 2) {
      // Still non-fatal — a fresh price beside a stale chart beats neither —
      // but no longer silent: a sparkline that never appears otherwise gives
      // nothing to debug with.
      if (!fetchUrl(s, buildSaHistUrl(d.symbol), PARSE_SA_HIST, d) && !d.sparkCount)
        stocksSetNote("chart: history fetch failed (see tether log)");
    }
    return true;
  }

  if (source == SRC_YAHOO) {
    if (g_fetchPhase == 0) {
      if (fetchUrl(s, buildYahooUrl(s, YAHOO_CHART_HOST1, d.symbol), PARSE_YAHOO, d)) {
        s_yahooFails = 0;
        return true;
      }
      g_fetchPhase = 1;                 // transient drop: retry the mirror next tick
      return false;
    }
    if (!fetchUrl(s, buildYahooUrl(s, YAHOO_CHART_HOST2, d.symbol), PARSE_YAHOO, d)) {
      d.error = true;
      if (++s_yahooFails >= 2) {        // both mirrors, twice: not transient
        s_yahooAvoidUntil = millis() + YAHOO_AVOID_MS;
        if (!s_yahooAvoidUntil) s_yahooAvoidUntil = 1;   // 0 means "no latch"
        s_yahooFails = 0;
      }
    } else {
      s_yahooFails = 0;
    }
    return true;
  }

  if (source == SRC_CASH) {
    if (g_fetchPhase == 0) {            // quote: price + day change (~200 B)
      if (fetchUrl(s, buildCashQuoteUrl(d.symbol), PARSE_CASH_QUOTE, d)) { g_fetchPhase = 2; return false; }
      g_fetchPhase = 1;                 // retry the quote next tick
      return false;
    }
    if (g_fetchPhase == 1) {
      if (!fetchUrl(s, buildCashQuoteUrl(d.symbol), PARSE_CASH_QUOTE, d)) { d.error = true; return true; }
      g_fetchPhase = 2;
      return false;
    }
    // phase 2: sparkline series; its failure is non-fatal (stale chart beats
    // none). Also fetched with the chart hidden when the range-based change
    // needs the series' first point — but not when nothing on the device
    // would show it (cash TLS requests are expensive on the ESP8266).
    if ((s.ticker.showChart || (s.ticker.changeOnRange && s.ticker.showChange)) &&
        s.ticker.points >= 2)
      fetchUrl(s, buildCashChartUrl(s, d.symbol), PARSE_CASH_CHART, d);
    return true;
  }

  if (source == SRC_GHUB) {             // static per-symbol JSON from the repo
    if (!fetchUrl(s, buildGithubUrl(d.symbol), PARSE_GITHUB, d)) d.error = true;
    return true;
  }

  if (s.ticker.webhookUrl.length() < 8) { d.error = true; return true; }
  if (!fetchUrl(s, buildWebhookUrl(s, d.symbol), PARSE_WEBHOOK, d)) d.error = true;
  return true;
}

// ---------------------------------------------------------------------------
void stocksService(const Settings& s) {
  if (g_count == 0) return;

  if (!g_refreshing) {
    if ((int32_t)(millis() - g_nextPollMs) >= 0) {
      g_refreshing = true;
      g_fetchIdx = 0;
      g_fetchPhase = 0;
      stocksSetNote("");   // this cycle's failures write it fresh
    } else {
      return;
    }
  }

  // One network request per call so net/web/display keep getting serviced
  // between the (slow, on the ESP8266) TLS handshakes.
  if (g_fetchIdx < g_count) {
    StockData& d = g_stocks[g_fetchIdx];
    if (stepSymbol(s, d)) {          // symbol finished -> move to the next
      g_fetchIdx++;
      g_fetchPhase = 0;
    }
  }

  if (g_fetchIdx >= g_count) {
    g_refreshing = false;
    // If any symbol failed this cycle (typically a cash fetch skipped because
    // the heap was momentarily too fragmented for TLS), retry soon instead of
    // waiting the full poll interval — the heap usually recovers within
    // seconds. Cap the burst so a genuinely bad symbol doesn't hammer forever.
    bool anyErr = false;
    for (uint8_t i = 0; i < g_count; i++) if (g_stocks[i].error) { anyErr = true; break; }
    uint32_t period = (uint32_t)s.ticker.pollSec * 1000UL;
    if (anyErr && g_retryBurst < TICKER_RETRY_MAX) {
      g_retryBurst++;
      uint32_t fast = (uint32_t)TICKER_RETRY_SEC * 1000UL;
      if (fast < period) period = fast;
    } else {
      g_retryBurst = 0;   // all good, or burst spent -> back to normal cadence
    }
    g_nextPollMs = millis() + period;
  }
}
