// StoryMode.h — the cube writes a story, a word at a time, on the glass.
//
// A 264k-parameter TinyStories transformer running off the filesystem. It is
// not a chatbot and cannot be made into one: at this size the model has a
// 512-word vocabulary, no world knowledge, and one trick — simple children's
// stories. That is the point of it rather than a limitation to work around.
//
// One token is generated per loop pass instead of a story per call. A token
// takes a few hundred milliseconds, nearly all of it reading weights, and
// blocking the loop for the whole story would stall the web server and freeze
// the screen mid-sentence. Yielding between tokens is also what makes it
// watchable, which is most of why this exists.
#pragma once
#include "config.h"
#if WITH_STORY

#include "Mode.h"
#include "TinyLlm.h"

class StoryMode : public DisplayMode {
 public:
  const char* id() const override { return "story"; }
  uint8_t     modeConst() const override { return MODE_STORY; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;
  void onContextAction(Settings& s) override;   // long-press: write a new one

  // True while a story is being generated, so the carousel can hold still
  // rather than sliding away mid-sentence.
  bool busy() const { return state_ == GENERATING; }

  // For /api/status.
  static bool  modelPresent();
  static size_t modelSize();
  int   contextTokens() const { return llm_.seqLimit(); }
  float tokensPerSec() const { return tps_; }

 private:
  enum State : uint8_t { IDLE, GENERATING, DONE, FAILED };

  void startStory(const Settings& s);
  void stopStory();
  void drawChrome(const Settings& s);
  void drawPiece(const char* text);
  void drawFooter();

  TinyLlm  llm_;
  State    state_ = IDLE;
  bool     needFull_ = true;
  bool     autoStarted_ = false;   // this visit already kicked one off

  int      token_ = 0;
  int      pos_ = 0;
  uint32_t rng_ = 1;
  uint32_t startedMs_ = 0;
  float    tps_ = 0;
  char     err_[48] = {0};

  // Where the next piece lands. Text is appended, never repainted, so the
  // screen never flashes and a long story costs nothing extra to draw.
  int16_t  cx_ = 0, cy_ = 0;
  bool     full_ = false;          // ran out of screen; keep generating quietly
  char     footer_[32] = {0};
};

extern StoryMode g_storyMode;

#endif  // WITH_STORY
