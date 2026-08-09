// WorkMask.h — replacing words you would rather not have on a desk at work.
//
// Pure text in, text out, so tools/workmask_selftest can check it on a host.
// The interesting cases are all about *not* matching: a blocklist that fires on
// substrings turns "Scunthorpe" and "assassin" into censored gibberish, which
// is both wrong and conspicuous — precisely what the feature exists to avoid.
// Matching is therefore on whole words only.
#pragma once
#include <stdint.h>
#include <string.h>

// Is `c` part of a word? Anything else is a boundary.
static inline bool workIsWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '\'';
}

static inline char workLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive whole-word compare of `len` bytes.
static inline bool workWordEq(const char* a, const char* b, int len) {
  for (int i = 0; i < len; i++)
    if (workLower(a[i]) != workLower(b[i])) return false;
  return true;
}

// Overwrite every whole-word occurrence of any entry in `blocklist` with '*'.
// The blocklist is free text: entries separated by commas, spaces or newlines,
// so it can be typed into a web form without ceremony. Edits `text` in place
// and never changes its length, which keeps every layout calculation valid.
static inline void workMaskWords(char* text, const char* blocklist) {
  if (!text || !blocklist || !blocklist[0]) return;

  for (const char* e = blocklist; *e;) {
    while (*e == ',' || *e == ' ' || *e == '\n' || *e == '\r' || *e == '\t') e++;
    if (!*e) break;
    const char* wordStart = e;
    while (*e && *e != ',' && *e != ' ' && *e != '\n' && *e != '\r' && *e != '\t') e++;
    const int wordLen = (int)(e - wordStart);
    if (wordLen <= 0) continue;

    const int textLen = (int)strlen(text);
    for (int i = 0; i + wordLen <= textLen; i++) {
      // Whole word only: a boundary either side, or the end of the string.
      if (i > 0 && workIsWordChar(text[i - 1])) continue;
      if (i + wordLen < textLen && workIsWordChar(text[i + wordLen])) continue;
      if (!workWordEq(text + i, wordStart, wordLen)) continue;
      for (int k = 0; k < wordLen; k++) text[i + k] = '*';
      i += wordLen - 1;
    }
  }
}
