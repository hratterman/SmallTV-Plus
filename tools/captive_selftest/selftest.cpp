// Host-side checks for src/CaptiveForm.h — the portal HTML parser.
//
// The firmware half of this (sockets, HTTP, retries) cannot run here, but the
// half that quietly does the wrong thing can: attribute extraction, URL
// resolution, and which inputs get carried across. Two animations shipped
// broken this session for want of exactly this, so the parser gets a harness.
//
// Build + run: tools/captive_selftest/run.sh
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>
#include <cstdlib>

// --- the smallest Arduino String that CaptiveForm.h actually uses ----------
class String {
 public:
  String() {}
  String(const char* s) : v(s ? s : "") {}
  String(const std::string& s) : v(s) {}
  int length() const { return (int)v.size(); }
  char operator[](int i) const { return v[i]; }
  void reserve(int n) { v.reserve(n); }
  int indexOf(const char* n, int from = 0) const {
    auto p = v.find(n, from < 0 ? 0 : from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const String& n, int from = 0) const { return indexOf(n.v.c_str(), from); }
  int indexOf(char c, int from = 0) const {
    auto p = v.find(c, from < 0 ? 0 : from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int lastIndexOf(char c) const {
    auto p = v.rfind(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  String substring(int a, int b = -1) const {
    if (a < 0) a = 0;
    if (a > (int)v.size()) return String();
    if (b < 0 || b > (int)v.size()) b = (int)v.size();
    if (b < a) return String();
    return String(v.substr(a, b - a));
  }
  bool startsWith(const char* p) const { return v.rfind(p, 0) == 0; }
  void toLowerCase() { for (auto& c : v) c = (char)tolower((unsigned char)c); }
  void replace(char a, char b) { for (auto& c : v) if (c == a) c = b; }
  String& operator+=(const String& o) { v += o.v; return *this; }
  String& operator+=(const char* o) { v += o; return *this; }
  String& operator+=(char c) { v += c; return *this; }
  bool operator==(const char* o) const { return v == o; }
  const char* c_str() const { return v.c_str(); }
  std::string v;
};
static inline String operator+(const String& a, const String& b) { return String(a.v + b.v); }
static inline String operator+(const String& a, const char* b) { return String(a.v + b); }
static inline String operator+(const char* a, const String& b) { return String(a + b.v); }

#include "../../src/CaptiveForm.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}
static void ckEq(const String& got, const char* want, const char* what) {
  const bool good = got.v == want;
  printf("  %-5s %s\n", good ? "ok" : "FAIL", what);
  if (!good) {
    printf("        got  '%s'\n        want '%s'\n", got.c_str(), want);
    failures++;
  }
}

int main() {
  printf("--- attributes ---------------------------------------------\n");
  ckEq(captiveAttr("<form action=\"/login\" method=\"POST\">", "action"), "/login",
       "double-quoted attribute");
  ckEq(captiveAttr("<form action='/a b' method=post>", "action"), "/a b",
       "single-quoted attribute with a space");
  ckEq(captiveAttr("<form action=/go method=post>", "action"), "/go",
       "unquoted attribute");
  ckEq(captiveAttr("<input data-action=\"x\" name=\"u\">", "action"), "",
       "data-action does not match action");
  ckEq(captiveAttr("<input\n  name=\"u\"\n  value=\"v\">", "value"), "v",
       "attribute split across lines");
  ckEq(captiveAttr("<form method=post>", "action"), "", "absent attribute is empty");

  printf("\n--- url resolution -----------------------------------------\n");
  const String base = "http://portal.hotel.net/portal/index.html";
  ckEq(captiveResolveUrl(base, "https://x.example/go"), "https://x.example/go", "absolute");
  ckEq(captiveResolveUrl(base, "//x.example/go"), "http://x.example/go", "protocol relative");
  ckEq(captiveResolveUrl(base, "/accept"), "http://portal.hotel.net/accept", "root relative");
  ckEq(captiveResolveUrl(base, "accept.cgi"), "http://portal.hotel.net/portal/accept.cgi",
       "sibling path");
  ckEq(captiveResolveUrl("http://1.2.3.4", "/accept"), "http://1.2.3.4/accept",
       "origin with no path");
  ckEq(captiveResolveUrl("http://1.2.3.4", "accept"), "http://1.2.3.4/accept",
       "bare host, relative action");
  ckEq(captiveResolveUrl(base, ""), base.c_str(), "empty action posts back to the page");

  printf("\n--- a typical 'tick the box' portal ------------------------\n");
  {
    const String page =
        "<html><head><title>WiFi</title></head><body>"
        "<h1>Welcome</h1>"
        "<form method=\"POST\" action=\"/cgi-bin/accept\">"
        "<input type=\"hidden\" name=\"sessid\" value=\"a1b2c3\">"
        "<input type=\"hidden\" name=\"clientmac\" value=\"AA:BB:CC:DD:EE:FF\">"
        "<input type=\"checkbox\" name=\"agree\">"
        "<input type=\"submit\" name=\"go\" value=\"Connect\">"
        "</form></body></html>";
    CaptiveForm f = captiveParseForm(page, "http://portal.hotel.net/welcome", 24);
    ck(f.ok, "form found");
    ck(f.post, "method POST honoured");
    ckEq(f.url, "http://portal.hotel.net/cgi-bin/accept", "action resolved");
    ckEq(f.body,
         "sessid=a1b2c3&clientmac=AA%3ABB%3ACC%3ADD%3AEE%3AFF&agree=on&go=Connect",
         "hidden fields kept, box ticked, submit value carried, MAC encoded");
  }

  printf("\n--- awkward but real shapes --------------------------------\n");
  {
    // GET form, unquoted attrs, an input with no name, and a file input.
    const String page =
        "<form action=grant method=get>"
        "<input type=hidden name=tok value=zz9>"
        "<input type=text placeholder=\"unused\">"
        "<input type=file name=nope>"
        "<input type=submit value=Go>"
        "</form>";
    CaptiveForm f = captiveParseForm(page, "http://1.1.1.1/p/", 24);
    ck(f.ok && !f.post, "GET form");
    ckEq(f.url, "http://1.1.1.1/p/grant", "relative action off a directory");
    // The submit here has no name, so a browser would not send it either; the
    // file input is dropped by type. Only the hidden token survives.
    ckEq(f.body, "tok=zz9", "nameless input skipped, file input dropped");
  }
  {
    // Page truncated mid-form by the read cap: still usable.
    const String page =
        "<form method=post action=\"/ok\">"
        "<input type=hidden name=a value=1>"
        "<input type=hidden name=b value=2";
    CaptiveForm f = captiveParseForm(page, "http://h/", 24);
    ck(f.ok, "truncated page still yields a form");
    ckEq(f.body, "a=1", "only the complete inputs are used");
  }
  {
    const String page = "<html><body>Redirecting via JavaScript...</body></html>";
    CaptiveForm f = captiveParseForm(page, "http://h/", 24);
    ck(!f.ok, "a page with no form is rejected");
    ck(strstr(f.problem, "no <form>") != nullptr, "and says so");
  }
  {
    // Field cap respected — a portal with a huge form must not overrun.
    String page = "<form method=post action=/x>";
    for (int i = 0; i < 40; i++) {
      char b[64];
      snprintf(b, sizeof(b), "<input type=hidden name=f%d value=%d>", i, i);
      page += b;
    }
    page += "</form>";
    CaptiveForm f = captiveParseForm(page, "http://h/", 5);
    int n = 1;
    for (int i = 0; i < f.body.length(); i++) if (f.body[i] == '&') n++;
    ck(f.ok && n == 5, "field cap honoured");
  }

  printf("\n--- encoding ------------------------------------------------\n");
  ckEq(captiveUrlEncode("a b&c=d"), "a+b%26c%3Dd", "space, ampersand, equals");
  ckEq(captiveUrlEncode("Zz09-_.~"), "Zz09-_.~", "unreserved characters pass through");

  printf("\n-------------------------------------------------------------\n");
  if (failures) {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}
