// BlackjackMode.h — the game the lid pad can actually carry.
//
// It replaced a Flappy Bird, which was the wrong game for this hardware: the pad
// is debounced and polled from the main loop alongside the network and the
// panel, so a tap lands tens of milliseconds after your finger does. Any amount
// of that is fatal to a game scored on reaction time, and no amount of it
// matters to one played in turns.
//
// So nothing here is timed. The pad has two gestures — tap and long-press — and
// the mode interface spends long-press on "leave", which leaves one input for
// the game itself. One input is enough for a menu: tap moves the highlight,
// long-press takes the highlighted action, and Quit is always one of the
// choices, so the way out survives. Every decision can be made as slowly as you
// like, and a late tap costs nothing.
//
// Rules are single deck, dealer stands on all 17s, no betting — a record of
// hands won, lost and pushed rather than chips. The rules themselves are in
// Blackjack.h so they can be checked without a screen.
#pragma once
#include "config.h"
#if WITH_GAME

#include "Mode.h"
#include "Blackjack.h"

class BlackjackMode : public DisplayMode {
 public:
  const char* id() const override { return "blackjack"; }
  uint8_t     modeConst() const override { return MODE_BLACKJACK; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;
  void onContextAction(Settings& s) override;   // long-press: take the highlighted action

  bool wantsTap() const override { return true; }
  void onTap(Settings& s) override;             // tap: move the highlight
  bool holdsScreen() const override;

 private:
  // PLAYER and DONE are the two that take input. DEALER is the dealer drawing
  // its cards one at a time — cosmetic, and short, but it is the difference
  // between seeing why you lost and being told you did.
  enum State : uint8_t { ST_PLAYER, ST_DEALER, ST_DONE };

  enum Action : uint8_t { ACT_HIT, ACT_STAND, ACT_DEAL, ACT_QUIT };

  uint8_t menuCount() const;
  Action  menuAction(uint8_t i) const;
  void    doAction(Action a);

  void deal();
  void stand();
  void finish();
  void render(const Settings& s);
  void drawCard(int x, int y, uint8_t card, bool faceDown) const;
  void drawHand(int y, const BjHand& h, bool hideHole) const;

  void recordLoad();
  void recordSave() const;

  State    state_ = ST_DONE;
  bool     dirty_ = true;
  bool     dealt_ = false;         // a hand has been played since entering
  uint8_t  sel_ = 0;               // highlighted menu row
  uint32_t lastInput_ = 0;
  uint32_t dealerAt_ = 0;          // when the dealer may take its next card

  BjShoe    shoe_ = {};
  BjHand    player_ = {};
  BjHand    dealer_ = {};
  BjOutcome outcome_ = BJ_PLAYING;

  uint16_t won_ = 0, lost_ = 0, pushed_ = 0;
  uint32_t rng_ = 0x9E3779B9u;
};

extern BlackjackMode g_blackjackMode;

#endif  // WITH_GAME
