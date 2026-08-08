#include "config.h"
#if WITH_STORY

#include "StoryMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Settings.h"
#include <LittleFS.h>

#define C_DIM     0xB574   // secondary text, as the other modes use
#define C_PANEL   0x18E3
#define C_ACCENT  0x7D3F   // soft violet: this screen is the odd one out

StoryMode g_storyMode;

#define STORY_PATH "/story.tll"

// Screen layout: 240x240, the built-in 6x8 font at size 1. Small, but the
// model's whole context is about 340 characters and this holds twice that,
// so a story never scrolls off.
#define TXT_L     8
#define TXT_R     232
#define TXT_TOP   40
#define TXT_BOT   218
#define CHAR_W    6
#define LINE_H    10

// ---------------------------------------------------------------------------
// model file
// ---------------------------------------------------------------------------
static File s_file;

// TinyLlm asks for bytes; this is where they come from on the device. Reads
// are sequential within a tensor, which is why the export is ordered the way
// the forward pass consumes it.
static bool fileRead(void* /*ctx*/, uint32_t off, void* dst, uint32_t len) {
  if (!s_file) return false;
  if (!s_file.seek(off)) return false;
  return s_file.read((uint8_t*)dst, len) == (int)len;
}

bool StoryMode::modelPresent() { return LittleFS.exists(STORY_PATH); }

size_t StoryMode::modelSize() {
  File f = LittleFS.open(STORY_PATH, "r");
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

// ---------------------------------------------------------------------------
void StoryMode::begin(const Settings& s) {
  needFull_ = true;
  state_ = IDLE;
}

void StoryMode::invalidate(const Settings& s) {
  stopStory();
  needFull_ = true;
  state_ = IDLE;
  autoStarted_ = false;
}

void StoryMode::wake(const Settings& s) {
  needFull_ = true;
  autoStarted_ = false;    // arriving on this screen earns a fresh story
}

void StoryMode::onContextAction(Settings& s) {
  stopStory();
  needFull_ = true;
  autoStarted_ = false;
}

// ---------------------------------------------------------------------------
void StoryMode::stopStory() {
  if (llm_.ready()) llm_.end();     // ~100 KB back to the heap for everyone else
  if (s_file) s_file.close();
  if (state_ == GENERATING) state_ = DONE;
}

void StoryMode::startStory(const Settings& s) {
  stopStory();
  err_[0] = 0;
  full_ = false;
  tps_ = 0;

  if (!modelPresent()) {
    strlcpy(err_, "no model uploaded", sizeof(err_));
    state_ = FAILED;
    return;
  }
  s_file = LittleFS.open(STORY_PATH, "r");
  if (!s_file) {
    strlcpy(err_, "cannot open the model", sizeof(err_));
    state_ = FAILED;
    return;
  }
  // The engine is allocated per story and released at the end. Holding its
  // working set permanently would take ~100 KB out of the heap that the ticker
  // and Spotify need for TLS handshakes, and a story is a brief event.
  if (!llm_.begin(fileRead, nullptr, s.story.maxTokens)) {
    s_file.close();
    strlcpy(err_, "not enough free memory", sizeof(err_));
    state_ = FAILED;
    return;
  }

  // A prompt gives the model somewhere to start; with none it opens on BOS and
  // finds its own beginning, which is usually "Once upon a time" anyway.
  token_ = TinyLlm::kBos;
  pos_ = 0;
  rng_ = (uint32_t)millis() * 2654435761u + 1u;
  startedMs_ = millis();
  state_ = GENERATING;
  drawChrome(s);

  if (s.story.prompt.length()) {
    int ids[32];
    String p = s.story.prompt;
    if (!p.startsWith(" ")) p = " " + p;
    const int n = llm_.encode(p.c_str(), ids, 32);
    for (int i = 0; i < n && pos_ < llm_.seqLimit() - 1; i++) {
      llm_.step(token_, pos_, 0.0f, 1.0f, &rng_);   // prime the KV cache
      token_ = ids[i];
      pos_++;
      drawPiece(llm_.piece(token_));
    }
  }
}

// ---------------------------------------------------------------------------
void StoryMode::drawChrome(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(C_ACCENT);
  gfx->setCursor(10, 10);
  gfx->print("STORY");
  gfx->drawFastHLine(TXT_L, TXT_TOP - 10, TXT_R - TXT_L, C_PANEL);
  cx_ = TXT_L;
  cy_ = TXT_TOP;
  full_ = false;
  footer_[0] = 0;
}

// Appends one piece at the cursor, wrapping on word boundaries. Nothing is ever
// repainted, so there is no flicker and the cost per token does not grow with
// the length of the story.
void StoryMode::drawPiece(const char* text) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx || !text || !*text || full_) return;

  gfx->setTextSize(1);
  gfx->setTextColor(C_WHITE);

  // Split on spaces so a word never breaks across a line.
  const char* p = text;
  while (*p && !full_) {
    // One word, plus any leading space.
    char word[32];
    int n = 0;
    if (*p == ' ' && n < (int)sizeof(word) - 1) word[n++] = *p++;
    while (*p && *p != ' ' && *p != '\n' && n < (int)sizeof(word) - 1) word[n++] = *p++;
    word[n] = 0;
    if (*p == '\n') { p++; cx_ = TXT_L; cy_ += LINE_H; }
    if (!n) continue;

    const int w = n * CHAR_W;
    if (cx_ + w > TXT_R) {
      cx_ = TXT_L;
      cy_ += LINE_H;
      // Drop the leading space at the start of a line.
      if (word[0] == ' ') { memmove(word, word + 1, n); n--; }
    }
    if (cy_ + 8 > TXT_BOT) { full_ = true; break; }
    gfx->setCursor(cx_, cy_);
    gfx->print(word);
    cx_ += (int)strlen(word) * CHAR_W;
  }
}

void StoryMode::drawFooter() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  char buf[32];
  if (state_ == GENERATING)
    snprintf(buf, sizeof(buf), "writing  %d/%d  %.1f tok/s", pos_, llm_.seqLimit(), tps_);
  else if (state_ == FAILED)
    snprintf(buf, sizeof(buf), "%s", err_);
  else
    snprintf(buf, sizeof(buf), "hold the pad for another");
  if (!strcmp(buf, footer_)) return;
  strlcpy(footer_, buf, sizeof(footer_));
  gfx->fillRect(0, 226, TFT_WIDTH, 10, C_BLACK);
  gfxDrawCentered(buf, 226, 1, state_ == FAILED ? C_RED : C_DIM);
}

// ---------------------------------------------------------------------------
void StoryMode::service(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (needFull_) {
    needFull_ = false;
    if (state_ != GENERATING) {
      drawChrome(s);
      if (state_ == FAILED || !modelPresent()) {
        gfxDrawCentered("no model uploaded", 104, 1, C_WHITE);
        gfxDrawCentered("Story tab in the web UI", 122, 1, C_DIM);
        state_ = FAILED;
        strlcpy(err_, "no model uploaded", sizeof(err_));
        drawFooter();
        return;
      }
    }
  }

  // Arriving on this screen starts one, which is the whole interaction: tap
  // round to Story and it begins writing.
  if (state_ != GENERATING && !autoStarted_ && modelPresent()) {
    autoStarted_ = true;
    startStory(s);
    drawFooter();
    return;
  }

  if (state_ != GENERATING) { drawFooter(); return; }

  // Exactly one token per pass. The loop gets the core back in between, which
  // is what keeps the web UI answering and the text appearing as it is written.
  const int next = llm_.step(token_, pos_, s.story.temperature, s.story.topP, &rng_);
  pos_++;

  const uint32_t el = millis() - startedMs_;
  if (el) tps_ = (float)pos_ * 1000.0f / (float)el;

  if (next == TinyLlm::kEos || next == TinyLlm::kBos || pos_ >= llm_.seqLimit()) {
    stopStory();
    state_ = DONE;
    drawFooter();
    return;
  }
  token_ = next;
  drawPiece(llm_.piece(token_));
  drawFooter();
}

#endif  // WITH_STORY
