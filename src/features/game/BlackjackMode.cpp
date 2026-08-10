#include "config.h"
#if WITH_GAME

#include "BlackjackMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include <LittleFS.h>

// main.cpp: step to the next mode. The game holds the tap, so it has to be able
// to ask for the way out on the player's behalf.
extern void appNextMode();

BlackjackMode g_blackjackMode;

#define C_FELT   0x0300   // deep table green
#define C_CARD   0xFFFF
#define C_BACK   0x18B6   // card back, slate
#define C_EDGE   0x8410
#define C_PIP_R  0xE000
#define C_PIP_B  0x0000
#define C_LABEL  0x7BEF

#define REC_PATH "/blackjack.dat"
#define REC_MAGIC 0xB1u

// Card metrics. Six cards fit without overlapping; beyond that they shingle,
// which is what a real hand does anyway.
#define CARD_W    32
#define CARD_H    46
#define CARD_GAP  4
#define HAND_X    8
#define HAND_MAX  (TFT_WIDTH - 2 * HAND_X)

// The record line sits at y=10, not 4: the housing overhangs the panel's top
// few rows on these units, and 4 put the tops of the digits under the bezel.
// Every mode that learned this lesson starts at y >= 8.
#define REC_Y     10
#define DEALER_Y  44
#define PLAYER_Y  104
#define RESULT_Y  158
#define MENU_Y    178
#define MENU_H    18

#define DEALER_STEP_MS 450   // pace of the dealer's draw; nothing waits on input

static inline uint32_t xr(uint32_t* s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  return *s = v;
}

// bjShuffle/bjDraw take their randomness as a callback so the rules stay
// testable; this is the one that feeds them on the device.
static uint32_t bjRandCb(void* ctx) { return xr((uint32_t*)ctx); }

// ---- the record ------------------------------------------------------------
// Three counters of its own rather than settings fields: they change every hand,
// and rewriting config.json to record a card game would be a strange thing for a
// card game to do. The magic byte means a truncated or foreign file reads as a
// fresh record instead of a wild one.
void BlackjackMode::recordLoad() {
  won_ = lost_ = pushed_ = 0;
  File f = LittleFS.open(REC_PATH, "r");
  if (!f) return;
  uint8_t  magic = 0;
  uint16_t v[3] = {0, 0, 0};
  const bool ok = f.read(&magic, 1) == 1 &&
                  f.read((uint8_t*)v, sizeof(v)) == (int)sizeof(v) &&
                  magic == REC_MAGIC;
  f.close();
  if (ok) { won_ = v[0]; lost_ = v[1]; pushed_ = v[2]; }
}

void BlackjackMode::recordSave() const {
  File f = LittleFS.open(REC_PATH, "w");
  if (!f) return;
  const uint8_t  magic = REC_MAGIC;
  const uint16_t v[3] = {won_, lost_, pushed_};
  f.write(&magic, 1);
  f.write((const uint8_t*)v, sizeof(v));
  f.close();
}

// ---------------------------------------------------------------------------
void BlackjackMode::begin(const Settings& s) {
  recordLoad();
  rng_ = ((uint32_t)millis() * 2654435761u) | 1u;
  bjShuffle(shoe_, bjRandCb, &rng_);
  state_ = ST_DONE;
  outcome_ = BJ_PLAYING;
  dealt_ = false;
  sel_ = 0;
  bjHandClear(player_);
  bjHandClear(dealer_);
  invalidate(s);
}

void BlackjackMode::invalidate(const Settings& s) { (void)s; dirty_ = true; }
void BlackjackMode::wake(const Settings& s)       { (void)s; dirty_ = true; }

// Hold the screen while a hand is live, and for a while after the last input so
// the carousel does not rotate away between hands. Left alone it lets go.
bool BlackjackMode::holdsScreen() const {
  if (state_ != ST_DONE) return true;
  // lastInput_ is 0 until the pad is actually touched. Without that test the
  // zero reads as "a moment ago" for the first 30 s of every boot, and the
  // carousel sticks on a game nobody is playing.
  return lastInput_ && (millis() - lastInput_) < 30000UL;
}

// ---- the menu --------------------------------------------------------------
uint8_t BlackjackMode::menuCount() const { return state_ == ST_PLAYER ? 3 : 2; }

BlackjackMode::Action BlackjackMode::menuAction(uint8_t i) const {
  if (state_ == ST_PLAYER) {
    if (i == 0) return ACT_HIT;
    if (i == 1) return ACT_STAND;
    return ACT_QUIT;
  }
  return i == 0 ? ACT_DEAL : ACT_QUIT;
}

void BlackjackMode::onTap(Settings& s) {
  (void)s;
  lastInput_ = millis();
  if (state_ == ST_DEALER) return;              // nothing to choose mid-draw
  sel_ = (uint8_t)((sel_ + 1) % menuCount());
  dirty_ = true;
}

void BlackjackMode::onContextAction(Settings& s) {
  (void)s;
  lastInput_ = millis();
  if (state_ == ST_DEALER) return;
  doAction(menuAction(sel_));
}

void BlackjackMode::doAction(Action a) {
  switch (a) {
    case ACT_QUIT:
      appNextMode();
      return;
    case ACT_DEAL:
      deal();
      return;
    case ACT_STAND:
      stand();
      return;
    case ACT_HIT:
      if (!bjHandPush(player_, bjDraw(shoe_, bjRandCb, &rng_))) { stand(); return; }
      // A hand at the ceiling cannot be hit again, and 21 has nothing to gain
      // by it — either way the turn is over rather than offering a dead button.
      if (bjBust(player_))            { finish(); return; }
      if (bjValue(player_) == 21 ||
          player_.n >= BJ_MAX_CARDS)  { stand(); return; }
      dirty_ = true;
      return;
  }
}

void BlackjackMode::deal() {
  if (bjRemaining(shoe_) < BJ_RESHUFFLE) bjShuffle(shoe_, bjRandCb, &rng_);
  bjHandClear(player_);
  bjHandClear(dealer_);
  bjHandPush(player_, bjDraw(shoe_, bjRandCb, &rng_));
  bjHandPush(dealer_, bjDraw(shoe_, bjRandCb, &rng_));
  bjHandPush(player_, bjDraw(shoe_, bjRandCb, &rng_));
  bjHandPush(dealer_, bjDraw(shoe_, bjRandCb, &rng_));

  dealt_ = true;
  outcome_ = BJ_PLAYING;
  sel_ = 0;
  dirty_ = true;

  // A blackjack either way settles immediately; there is no decision to offer.
  if (bjBlackjack(player_) || bjBlackjack(dealer_)) { finish(); return; }
  state_ = ST_PLAYER;
}

void BlackjackMode::stand() {
  state_ = ST_DEALER;
  dealerAt_ = millis() + DEALER_STEP_MS;   // reveal the hole card first
  dirty_ = true;
}

void BlackjackMode::finish() {
  outcome_ = bjSettle(player_, dealer_);
  if      (bjPlayerWon(outcome_))  won_++;
  else if (bjPlayerLost(outcome_)) lost_++;
  else                             pushed_++;
  recordSave();
  state_ = ST_DONE;
  sel_ = 0;                                 // DEAL, the one you almost always want
  dirty_ = true;
}

// ---- drawing ---------------------------------------------------------------
void BlackjackMode::drawCard(int x, int y, uint8_t card, bool faceDown) const {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  gfx->fillRoundRect(x, y, CARD_W, CARD_H, 4, faceDown ? C_BACK : C_CARD);
  gfx->drawRoundRect(x, y, CARD_W, CARD_H, 4, C_EDGE);
  if (faceDown) {
    for (int i = 6; i < CARD_H - 6; i += 5)
      gfx->drawFastHLine(x + 5, y + i, CARD_W - 10, C_EDGE);
    return;
  }

  static const char* kRank[13] = {"A", "2", "3", "4", "5", "6", "7",
                                  "8", "9", "10", "J", "Q", "K"};
  const uint16_t col = bjRed(card) ? C_PIP_R : C_PIP_B;
  const char* r = kRank[bjRank(card)];

  gfx->setTextSize(2);
  gfx->setTextColor(col);
  gfx->setCursor(x + 4, y + 6);
  gfx->print(r);

  // A suit glyph in the corner. The panel font has no card suits, so these are
  // drawn: a filled diamond, and a blob-and-stem for the rest. At this size the
  // colour carries most of the meaning and the shape only has to differ.
  const int sx = x + CARD_W - 11, sy = y + CARD_H - 14;
  switch (bjSuit(card)) {
    case 0:  // spades
      gfx->fillTriangle(sx + 4, sy, sx, sy + 6, sx + 8, sy + 6, col);
      gfx->fillRect(sx + 3, sy + 6, 3, 4, col);
      break;
    case 1:  // hearts
      gfx->fillCircle(sx + 2, sy + 3, 2, col);
      gfx->fillCircle(sx + 6, sy + 3, 2, col);
      gfx->fillTriangle(sx, sy + 4, sx + 8, sy + 4, sx + 4, sy + 10, col);
      break;
    case 2:  // diamonds
      gfx->fillTriangle(sx + 4, sy, sx, sy + 5, sx + 8, sy + 5, col);
      gfx->fillTriangle(sx + 4, sy + 10, sx, sy + 5, sx + 8, sy + 5, col);
      break;
    default:  // clubs
      gfx->fillCircle(sx + 4, sy + 2, 2, col);
      gfx->fillCircle(sx + 1, sy + 6, 2, col);
      gfx->fillCircle(sx + 7, sy + 6, 2, col);
      gfx->fillRect(sx + 3, sy + 6, 3, 4, col);
      break;
  }
}

void BlackjackMode::drawHand(int y, const BjHand& h, bool hideHole) const {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillRect(0, y, TFT_WIDTH, CARD_H, C_FELT);
  if (h.n == 0) return;

  // Shingle once the hand outgrows the row rather than running off the edge.
  int step = CARD_W + CARD_GAP;
  if (h.n > 1) {
    const int fit = (HAND_MAX - CARD_W) / (h.n - 1);
    if (fit < step) step = fit < 8 ? 8 : fit;
  }
  for (uint8_t i = 0; i < h.n; i++)
    drawCard(HAND_X + i * step, y, h.card[i], hideHole && i == 1);
}

void BlackjackMode::render(const Settings& s) {
  (void)s;
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  gfx->fillScreen(C_FELT);
  gfx->setTextSize(1);

  char line[40];
  snprintf(line, sizeof(line), "W %u   L %u   P %u",
           (unsigned)won_, (unsigned)lost_, (unsigned)pushed_);
  gfxDrawCentered(line, REC_Y, 1, C_LABEL);

  const bool hideHole = (state_ == ST_PLAYER);

  // Label each hand with its total, except the dealer's while the hole card is
  // down — showing it then would give away the card that is face down.
  if (hideHole) snprintf(line, sizeof(line), "DEALER");
  else if (dealer_.n)
    snprintf(line, sizeof(line), "DEALER  %u%s", (unsigned)bjValue(dealer_),
             bjBust(dealer_) ? "  BUST" : "");
  else snprintf(line, sizeof(line), "DEALER");
  gfx->setTextColor(C_LABEL);
  gfx->setCursor(HAND_X, DEALER_Y - 12);
  gfx->print(line);
  drawHand(DEALER_Y, dealer_, hideHole);

  if (player_.n) {
    bool soft = false;
    const uint8_t v = bjValue(player_, &soft);
    snprintf(line, sizeof(line), "YOU  %s%u%s", soft ? "soft " : "", (unsigned)v,
             bjBust(player_) ? "  BUST" : "");
  } else {
    snprintf(line, sizeof(line), "YOU");
  }
  gfx->setTextColor(C_LABEL);
  gfx->setCursor(HAND_X, PLAYER_Y - 12);
  gfx->print(line);
  drawHand(PLAYER_Y, player_, false);

  if (state_ == ST_DONE && dealt_) {
    const uint16_t c = bjPlayerWon(outcome_)  ? C_GREEN
                       : bjPlayerLost(outcome_) ? C_RED
                                                : C_YELLOW;
    gfxDrawCentered(bjOutcomeText(outcome_), RESULT_Y, 2, c);
  } else if (state_ == ST_DONE) {
    gfxDrawCentered("BLACKJACK", RESULT_Y, 2, C_WHITE);
  }

  // The menu. The highlighted row is the one a long-press takes, so it has to
  // read as selected at a glance and from an angle: filled bar, not an outline.
  if (state_ != ST_DEALER) {
    static const char* kLabel[4] = {"HIT", "STAND", "DEAL", "QUIT"};
    const uint8_t n = menuCount();
    const int w = TFT_WIDTH / n;
    for (uint8_t i = 0; i < n; i++) {
      const bool on = (i == sel_);
      const int x = i * w;
      gfx->fillRect(x + 2, MENU_Y, w - 4, MENU_H, on ? C_WHITE : C_FELT);
      gfx->drawRect(x + 2, MENU_Y, w - 4, MENU_H, on ? C_WHITE : C_EDGE);
      const char* t = kLabel[menuAction(i)];
      const int tw = gfxTextW(t, 1);
      gfx->setTextSize(1);
      gfx->setTextColor(on ? C_BLACK : C_LABEL);
      gfx->setCursor(x + (w - tw) / 2, MENU_Y + 6);
      gfx->print(t);
    }
    gfxDrawCentered("tap to move  .  hold to pick", MENU_Y + MENU_H + 8, 1, C_LABEL);
  } else {
    gfxDrawCentered("dealer draws", MENU_Y + 6, 1, C_LABEL);
  }
}

void BlackjackMode::service(const Settings& s) {
  if (state_ == ST_DEALER && (int32_t)(millis() - dealerAt_) >= 0) {
    if (bjDealerHits(dealer_) && dealer_.n < BJ_MAX_CARDS) {
      bjHandPush(dealer_, bjDraw(shoe_, bjRandCb, &rng_));
      dealerAt_ = millis() + DEALER_STEP_MS;
      dirty_ = true;
    } else {
      finish();
    }
  }

  if (!dirty_) return;
  dirty_ = false;
  render(s);
}

#endif  // WITH_GAME
