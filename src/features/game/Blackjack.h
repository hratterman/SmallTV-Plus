// Blackjack.h — the rules, with no screen and no Arduino anywhere in them.
//
// Split from the mode so the parts that can be wrong quietly are checkable on a
// host: ace demotion, when the dealer stops, who actually won, and the size of
// a hand. That last one is not a rules question but a buffer one — the hand is
// a fixed array, and "how many cards can a hand hold" is the sort of thing that
// is obviously eleven right up until it is twelve.
//
// The shuffle takes its randomness as a callback rather than calling anything
// itself, so a test can pin the deck and get the same hand every run.
#pragma once
#include <stdint.h>

#define BJ_DECK      52
// A hand cannot exceed this. The lowest-scoring 21 possible is four aces (4),
// four twos (8) and three threes (9) — eleven cards. A twelfth card of any
// value busts it, and a bust hand is never hit again.
#define BJ_MAX_CARDS 12
// Reshuffle with this many left rather than dealing across the shuffle, so a
// hand is always dealt from one deck.
#define BJ_RESHUFFLE 15

// A card is 0..51. Rank 0 is an ace, 1..8 are 2..9, 9 is a ten, 10..12 are the
// court cards. Suits are ordered spades, hearts, diamonds, clubs.
static inline uint8_t bjRank(uint8_t c) { return (uint8_t)(c % 13); }
static inline uint8_t bjSuit(uint8_t c) { return (uint8_t)(c / 13); }
static inline bool    bjRed(uint8_t c)  { const uint8_t s = bjSuit(c); return s == 1 || s == 2; }

// Aces count 11 here; bjValue demotes them as needed.
static inline uint8_t bjCardValue(uint8_t c) {
  const uint8_t r = bjRank(c);
  if (r == 0) return 11;
  return r >= 9 ? (uint8_t)10 : (uint8_t)(r + 1);
}

struct BjHand {
  uint8_t card[BJ_MAX_CARDS];
  uint8_t n;
};

static inline void bjHandClear(BjHand& h) { h.n = 0; }

static inline bool bjHandPush(BjHand& h, uint8_t c) {
  if (h.n >= BJ_MAX_CARDS) return false;      // caller is over the ceiling
  h.card[h.n++] = c;
  return true;
}

// The best total that is not a bust. `soft` reports whether an ace is still
// being counted as eleven, which is what "soft 17" means.
static inline uint8_t bjValue(const BjHand& h, bool* soft = nullptr) {
  uint16_t total = 0;
  uint8_t  aces = 0;
  for (uint8_t i = 0; i < h.n; i++) {
    total += bjCardValue(h.card[i]);
    if (bjRank(h.card[i]) == 0) aces++;
  }
  while (total > 21 && aces) { total -= 10; aces--; }
  if (soft) *soft = (aces > 0);
  return (uint8_t)total;
}

static inline bool bjBust(const BjHand& h) { return bjValue(h) > 21; }

// Twenty-one on the first two cards. Three cards making 21 is not this, and the
// difference decides a hand where both sides have 21.
static inline bool bjBlackjack(const BjHand& h) { return h.n == 2 && bjValue(h) == 21; }

// ---- the shoe --------------------------------------------------------------
typedef uint32_t (*BjRand)(void* ctx);

struct BjShoe {
  uint8_t card[BJ_DECK];
  uint8_t next;
};

static inline void bjShuffle(BjShoe& s, BjRand rnd, void* ctx) {
  for (uint8_t i = 0; i < BJ_DECK; i++) s.card[i] = i;
  // Fisher-Yates downward: every ordering is equally likely, which the naive
  // "swap each card with any other" version is not.
  for (uint8_t i = BJ_DECK - 1; i > 0; i--) {
    const uint8_t j = (uint8_t)(rnd(ctx) % (uint32_t)(i + 1));
    const uint8_t t = s.card[i];
    s.card[i] = s.card[j];
    s.card[j] = t;
  }
  s.next = 0;
}

static inline uint8_t bjRemaining(const BjShoe& s) { return (uint8_t)(BJ_DECK - s.next); }

static inline uint8_t bjDraw(BjShoe& s, BjRand rnd, void* ctx) {
  if (s.next >= BJ_DECK) bjShuffle(s, rnd, ctx);   // never read off the end
  return s.card[s.next++];
}

// ---- outcomes --------------------------------------------------------------
enum BjOutcome : uint8_t {
  BJ_PLAYING = 0,
  BJ_PLAYER_BUST,
  BJ_DEALER_BUST,
  BJ_PLAYER_WIN,
  BJ_DEALER_WIN,
  BJ_PUSH,
  BJ_PLAYER_BLACKJACK,
};

// Stands on all seventeens, soft ones included. Written once, here, so the
// screen and the play loop cannot come to different conclusions about it.
static inline bool bjDealerHits(const BjHand& h) { return bjValue(h) < 17; }

static inline BjOutcome bjSettle(const BjHand& p, const BjHand& d) {
  if (bjBust(p)) return BJ_PLAYER_BUST;          // busting loses even if the dealer would too
  const bool pbj = bjBlackjack(p), dbj = bjBlackjack(d);
  if (pbj && dbj) return BJ_PUSH;
  if (pbj)        return BJ_PLAYER_BLACKJACK;
  if (dbj)        return BJ_DEALER_WIN;
  if (bjBust(d))  return BJ_DEALER_BUST;
  const uint8_t pv = bjValue(p), dv = bjValue(d);
  if (pv > dv) return BJ_PLAYER_WIN;
  if (pv < dv) return BJ_DEALER_WIN;
  return BJ_PUSH;
}

static inline bool bjPlayerWon(BjOutcome o) {
  return o == BJ_PLAYER_WIN || o == BJ_DEALER_BUST || o == BJ_PLAYER_BLACKJACK;
}
static inline bool bjPlayerLost(BjOutcome o) {
  return o == BJ_PLAYER_BUST || o == BJ_DEALER_WIN;
}

static inline const char* bjOutcomeText(BjOutcome o) {
  switch (o) {
    case BJ_PLAYER_BLACKJACK: return "BLACKJACK!";
    case BJ_PLAYER_WIN:       return "YOU WIN";
    case BJ_DEALER_BUST:      return "DEALER BUST";
    case BJ_PLAYER_BUST:      return "BUST";
    case BJ_DEALER_WIN:       return "DEALER WINS";
    case BJ_PUSH:             return "PUSH";
    default:                  return "";
  }
}
