#include "config.h"
#if WITH_CAPTIVE

#include "Captive.h"
#include "Platform.h"
#include "Net.h"
#include "CaptiveForm.h"
#include <WiFiClient.h>

#if defined(SMALLTV_ESP8266)
#include <ESP8266HTTPClient.h>
#else
#include <HTTPClient.h>
#endif

static CaptiveState s_state = CAP_UNKNOWN;
static char     s_detail[64] = "not probed";
static char     s_portal[160] = "";
static uint8_t  s_attempts = 0;
static uint32_t s_nextProbe = 0;
static bool     s_forced = false;

CaptiveState captiveStateNow() { return s_state; }
const char*  captiveDetail()   { return s_detail; }
const char*  captivePortalUrl(){ return s_portal; }
uint8_t      captiveAttempts() { return s_attempts; }
void         captiveForceTry() { s_forced = true; }

// Stuck means: seen a portal, tried the configured number of times, still not
// out. Net.cpp uses this to raise the setup AP so the device stays reachable.
bool captiveStuck() {
  return s_state == CAP_PORTAL && s_attempts >= CAPTIVE_MAX_ATTEMPTS;
}

void captiveBegin(const Settings& s) {
  s_state = CAP_UNKNOWN;
  s_attempts = 0;
  s_portal[0] = 0;
  strlcpy(s_detail, "not probed", sizeof(s_detail));
  s_nextProbe = millis() + 2000;   // let DHCP settle first
}

// ---------------------------------------------------------------------------
// The probe.
//
// Plain HTTP on purpose. A portal cannot intercept HTTPS without the browser
// screaming, so it lets the connection fail or hang instead — which is
// indistinguishable from a dead network. Over HTTP it has to answer, and its
// answer is the detection.
static CaptiveState probe(const Settings& s, String& portalOut) {
  WiFiClient client;
  HTTPClient http;
  const String url = s.captive.probeUrl.length() ? s.captive.probeUrl
                                                 : String(CAPTIVE_PROBE_URL);
  if (!http.begin(client, url)) {
    strlcpy(s_detail, "probe: could not start", sizeof(s_detail));
    return CAP_NO_NET;
  }
  http.setTimeout(CAPTIVE_TIMEOUT_MS);
  // Do NOT follow the redirect: where it points is the portal's address, and
  // following it throws that away.
  const char* keep[] = {"Location"};
  http.collectHeaders(keep, 1);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  const int code = http.GET();

  if (code == 204) {
    // The expected answer, and it has to be empty as well: some portals return
    // 204 with a body to fool exactly this check.
    const int len = http.getSize();
    http.end();
    if (len <= 0) {
      strlcpy(s_detail, "online", sizeof(s_detail));
      return CAP_ONLINE;
    }
    snprintf(s_detail, sizeof(s_detail), "portal: 204 with %d bytes of body", len);
    return CAP_PORTAL;
  }

  if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
    const String loc = http.header("Location");
    http.end();
    portalOut = loc;
    snprintf(s_detail, sizeof(s_detail), "portal: %d redirect", code);
    return CAP_PORTAL;
  }

  if (code == 200) {
    http.end();
    portalOut = url;              // it answered in place of the real target
    strlcpy(s_detail, "portal: answered the probe itself", sizeof(s_detail));
    return CAP_PORTAL;
  }

  http.end();
  if (code > 0) snprintf(s_detail, sizeof(s_detail), "portal: HTTP %d", code);
  else          snprintf(s_detail, sizeof(s_detail), "no route out (err %d)", code);
  return code > 0 ? CAP_PORTAL : CAP_NO_NET;
}

// ---------------------------------------------------------------------------
// Fetching and submitting. The parsing itself lives in CaptiveForm.h, which is
// pure string handling and is exercised on a host by tools/captive_selftest.

// Fetch the portal page and submit its form.
static bool tryAccept(const Settings& s, const String& portalUrl) {
  String url = portalUrl, body;
  bool post = false;

  if (s.captive.formUrl.length()) {
    // Manual override: exactly what the user captured from their own browser.
    url = s.captive.formUrl;
    body = s.captive.formBody;
    post = body.length() > 0;
  } else {
    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, portalUrl)) {
      strlcpy(s_detail, "portal page: could not start", sizeof(s_detail));
      return false;
    }
    http.setTimeout(CAPTIVE_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    const int code = http.GET();
    if (code != 200) {
      snprintf(s_detail, sizeof(s_detail), "portal page: HTTP %d", code);
      http.end();
      return false;
    }
    // Bounded read. A portal page can be a megabyte of framework, and only the
    // form matters; the heap here is shared with everything else running.
    String page;
    {
      WiFiClient* st = http.getStreamPtr();
      const uint32_t deadline = millis() + CAPTIVE_TIMEOUT_MS;
      page.reserve(CAPTIVE_MAX_PAGE);
      while (page.length() < CAPTIVE_MAX_PAGE &&
             (int32_t)(millis() - deadline) < 0) {
        if (!st->available()) {
          if (!st->connected()) break;
          delay(5);
          continue;
        }
        page += (char)st->read();
      }
    }
    http.end();
    Serial.printf("[captive] portal page %u bytes from %s\n",
                  (unsigned)page.length(), portalUrl.c_str());
    const CaptiveForm f = captiveParseForm(page, portalUrl, CAPTIVE_MAX_FIELDS);
    if (!f.ok) {
      strlcpy(s_detail, f.problem, sizeof(s_detail));
      return false;
    }
    url = f.url;
    body = f.body;
    post = f.post;
  }

  Serial.printf("[captive] submitting %s %s (%u bytes of fields)\n",
                post ? "POST" : "GET", url.c_str(), (unsigned)body.length());

  WiFiClient client2;
  HTTPClient http2;
  String target = url;
  if (!post && body.length()) target += (url.indexOf('?') < 0 ? "?" : "&") + body;
  if (!http2.begin(client2, target)) {
    strlcpy(s_detail, "submit: could not start", sizeof(s_detail));
    return false;
  }
  http2.setTimeout(CAPTIVE_TIMEOUT_MS);
  http2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http2.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = post ? http2.POST(body) : http2.GET();
  http2.end();
  Serial.printf("[captive] submit answered %d\n", code);
  snprintf(s_detail, sizeof(s_detail), "submitted, portal said %d", code);
  return code > 0;
}

// ---------------------------------------------------------------------------
void captiveService(const Settings& s) {
  if (!netConnectedSta()) {          // nothing to probe without an association
    if (s_state != CAP_UNKNOWN) captiveBegin(s);
    return;
  }
  const uint32_t now = millis();
  if (!s_forced && (int32_t)(now - s_nextProbe) < 0) return;
  s_forced = false;

  String portal;
  const CaptiveState st = probe(s, portal);
  s_state = st;
  if (portal.length()) strlcpy(s_portal, portal.c_str(), sizeof(s_portal));

  if (st == CAP_ONLINE) {
    s_attempts = 0;
    s_nextProbe = now + CAPTIVE_OK_RECHECK_MS;
    return;
  }

  Serial.printf("[captive] %s\n", s_detail);

  if (st == CAP_PORTAL && s.captive.autoAccept && s_attempts < CAPTIVE_MAX_ATTEMPTS &&
      (s_portal[0] || s.captive.formUrl.length())) {
    s_attempts++;
    Serial.printf("[captive] accept attempt %u of %u\n",
                  (unsigned)s_attempts, (unsigned)CAPTIVE_MAX_ATTEMPTS);
    if (tryAccept(s, String(s_portal))) {
      // Re-probe straight away rather than claiming success: the portal
      // answering 200 does not mean it let us out.
      String again;
      s_state = probe(s, again);
      if (s_state == CAP_ONLINE) {
        Serial.println("[captive] through — route out confirmed");
        s_attempts = 0;
        s_nextProbe = now + CAPTIVE_OK_RECHECK_MS;
        return;
      }
      Serial.println("[captive] submitted but still no route out");
    }
  }

  s_nextProbe = now + CAPTIVE_RETRY_MS;
}

#endif  // WITH_CAPTIVE
