// CaptiveForm.h — the pure half of captive-portal handling: given a portal's
// HTML, work out the request a browser would send when you press Connect.
//
// Separated from Captive.cpp so it can be compiled and exercised on a host with
// no radio (tools/captive_selftest). Everything here is string in, string out,
// with no I/O and no globals — the parts that are easy to get quietly wrong are
// the parts that are testable.
#pragma once
#include <Arduino.h>

// One parsed form, ready to submit.
struct CaptiveForm {
  String url;      // absolute, resolved against the page it came from
  String body;     // url-encoded fields
  bool   post;     // false = append body as a query string
  bool   ok;
  const char* problem;   // why not, when !ok
};

// Value of `name=` in a tag, quoted or bare. Returns empty when absent.
inline String captiveAttr(const String& tag, const char* name) {
  // Match on a preceding space so `action=` does not also match `data-action=`
  // and `value=` does not match `data-value=`.
  const String needle = String(" ") + name + "=";
  String hay = tag;
  hay.replace('\n', ' ');
  hay.replace('\t', ' ');
  hay.replace('\r', ' ');
  int i = hay.indexOf(needle);
  if (i < 0) return String();
  i += needle.length();
  if (i >= (int)hay.length()) return String();
  const char q = hay[i];
  if (q == '"' || q == '\'') {
    const int end = hay.indexOf(q, i + 1);
    return end < 0 ? String() : hay.substring(i + 1, end);
  }
  int end = i;
  while (end < (int)hay.length() && hay[end] != ' ' && hay[end] != '>') end++;
  return hay.substring(i, end);
}

inline String captiveUrlEncode(const String& v) {
  String out;
  out.reserve(v.length() + 8);
  for (int i = 0; i < (int)v.length(); i++) {
    const char c = v[i];
    const bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      out += b;
    }
  }
  return out;
}

// Resolve a form action against the page it came from: absolute, protocol
// relative, root relative, or a sibling path.
inline String captiveResolveUrl(const String& base, const String& action) {
  if (!action.length()) return base;
  if (action.startsWith("http://") || action.startsWith("https://")) return action;

  const int schemeEnd = base.indexOf("://");
  if (schemeEnd < 0) return action;
  if (action.startsWith("//")) return base.substring(0, schemeEnd + 1) + action;

  int hostEnd = base.indexOf('/', schemeEnd + 3);
  if (hostEnd < 0) hostEnd = base.length();
  const String origin = base.substring(0, hostEnd);
  if (action.startsWith("/")) return origin + action;

  // Sibling: everything up to the last slash of the path, or the origin when
  // the path has no slash of its own.
  const int lastSlash = base.lastIndexOf('/');
  if (lastSlash < hostEnd) return origin + "/" + action;
  return base.substring(0, lastSlash + 1) + action;
}

// Find the first form in `page` and build what pressing its button would send.
inline CaptiveForm captiveParseForm(const String& page, const String& pageUrl,
                                    int maxFields) {
  CaptiveForm f;
  f.post = false;
  f.ok = false;
  f.problem = "";

  const int fStart = page.indexOf("<form");
  if (fStart < 0) { f.problem = "portal page has no <form>"; return f; }
  const int fTagEnd = page.indexOf('>', fStart);
  if (fTagEnd < 0) { f.problem = "portal <form> tag is truncated"; return f; }
  const String formTag = page.substring(fStart, fTagEnd);

  int fEnd = page.indexOf("</form", fTagEnd);
  if (fEnd < 0) fEnd = page.length();   // truncated page: take what we have

  String method = captiveAttr(formTag, "method");
  method.toLowerCase();
  f.post = (method == "post");
  f.url = captiveResolveUrl(pageUrl, captiveAttr(formTag, "action"));

  // Hidden fields are the ones that matter: they carry the session id and, on
  // most portals, the client's own identity. That is why the device submitting
  // the page *it* was served authorises the device — replaying a capture taken
  // on a laptop would just re-authorise the laptop.
  int pos = fTagEnd;
  int fields = 0;
  while (pos < fEnd && fields < maxFields) {
    const int i = page.indexOf("<input", pos);
    if (i < 0 || i >= fEnd) break;
    int e = page.indexOf('>', i);
    if (e < 0) break;
    const String tag = page.substring(i, e);
    pos = e + 1;

    const String name = captiveAttr(tag, "name");
    if (!name.length()) continue;
    String type = captiveAttr(tag, "type");
    type.toLowerCase();
    if (type == "button" || type == "reset" || type == "file" || type == "image")
      continue;

    String value = captiveAttr(tag, "value");
    // A browser omits an unticked box, but a portal's box is the "I agree" —
    // the entire point of the exercise — so it always goes.
    if ((type == "checkbox" || type == "radio") && !value.length()) value = "on";

    if (f.body.length()) f.body += '&';
    f.body += captiveUrlEncode(name);
    f.body += '=';
    f.body += captiveUrlEncode(value);
    fields++;
  }

  if (!f.url.length()) { f.problem = "form has no usable action"; return f; }
  f.ok = true;
  return f;
}
