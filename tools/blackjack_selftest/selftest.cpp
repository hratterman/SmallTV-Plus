// Host-side checks for src/features/game/Blackjack.h.
//
// Card-game rules are the kind of thing that looks obviously right and is
// quietly wrong for one hand in fifty — a soft seventeen the dealer hits when
// it should stand, a three-card 21 paid as a blackjack, an ace demoted once
// when it needed demoting twice. None of that shows up as a crash; it shows up
// as a game that feels slightly unfair and nobody can say why.
//
// The last section is not a rules check at all. BjHand is a fixed array, and
// "how many cards can a hand hold" is obviously eleven right up until it is
// twelve — so a full playout is run over a large number of shuffles and the
// hand sizes are watched.
#include <cstdio>
#include <cstring>

#include "../../src/features/game/Blackjack.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

// A card by rank and suit, for readable test hands.
static uint8_t C(uint8_t rank, uint8_t suit = 0) { return (uint8_t)(suit * 13 + rank); }
static const uint8_t ACE = 0, TWO = 1, FIVE = 4, SIX = 5, SEVEN = 6, NINE = 8,
                     TEN = 9, JACK = 10, QUEEN = 11, KING = 12;

static BjHand hand(const uint8_t* cards, uint8_t n) {
  BjHand h;
  bjHandClear(h);
  for (uint8_t i = 0; i < n; i++) bjHandPush(h, cards[i]);
  return h;
}

static uint32_t s_rng = 1;
static uint32_t rnd(void*) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}

int main() {
  printf("--- card values ----------------------------------------------\n");
  ck(bjCardValue(C(ACE)) == 11, "an ace starts at eleven");
  ck(bjCardValue(C(TWO)) == 2, "a two is two");
  ck(bjCardValue(C(NINE)) == 9, "a nine is nine");
  ck(bjCardValue(C(TEN)) == 10 && bjCardValue(C(JACK)) == 10 &&
     bjCardValue(C(QUEEN)) == 10 && bjCardValue(C(KING)) == 10,
     "ten and every court card are ten");
  {
    bool anyBad = false;
    for (uint8_t c = 0; c < BJ_DECK; c++)
      if (bjCardValue(c) < 2 || bjCardValue(c) > 11) anyBad = true;
    ck(!anyBad, "every card in the deck values between 2 and 11");
  }
  {
    // Hearts and diamonds red, spades and clubs black — the renderer picks the
    // pip colour straight off this.
    ck(!bjRed(C(ACE, 0)) && bjRed(C(ACE, 1)) && bjRed(C(ACE, 2)) && !bjRed(C(ACE, 3)),
       "suits 1 and 2 are the red ones");
  }

  printf("\n--- hand totals and aces -------------------------------------\n");
  {
    const uint8_t c[] = {C(TEN), C(SEVEN)};
    ck(bjValue(hand(c, 2)) == 17, "ten and seven is seventeen");
  }
  {
    const uint8_t c[] = {C(ACE), C(SIX)};
    bool soft = false;
    ck(bjValue(hand(c, 2), &soft) == 17 && soft, "ace-six is a soft seventeen");
  }
  {
    const uint8_t c[] = {C(ACE), C(ACE)};
    ck(bjValue(hand(c, 2)) == 12, "two aces is twelve, not twenty-two");
  }
  {
    const uint8_t c[] = {C(ACE), C(ACE), C(ACE)};
    ck(bjValue(hand(c, 3)) == 13, "three aces is thirteen");
  }
  {
    const uint8_t c[] = {C(ACE), C(ACE), C(ACE), C(ACE)};
    ck(bjValue(hand(c, 4)) == 14, "four aces is fourteen");
  }
  {
    // The demotion that needs to happen after the hand is already complete.
    const uint8_t c[] = {C(ACE), C(NINE), C(FIVE)};
    bool soft = true;
    ck(bjValue(hand(c, 3), &soft) == 15 && !soft,
       "ace-nine-five is a hard fifteen, not a bust");
  }
  {
    const uint8_t c[] = {C(ACE), C(ACE), C(NINE)};
    ck(bjValue(hand(c, 3)) == 21, "ace-ace-nine is twenty-one");
  }
  {
    const uint8_t c[] = {C(KING), C(QUEEN), C(TWO)};
    ck(bjBust(hand(c, 3)), "king-queen-two busts");
  }
  {
    const uint8_t c[] = {C(KING), C(QUEEN)};
    ck(!bjBust(hand(c, 2)) && bjValue(hand(c, 2)) == 20, "king-queen is twenty");
  }

  printf("\n--- what counts as a blackjack -------------------------------\n");
  {
    const uint8_t c[] = {C(ACE), C(KING)};
    ck(bjBlackjack(hand(c, 2)), "ace and a king on the deal is a blackjack");
  }
  {
    const uint8_t c[] = {C(SEVEN), C(SEVEN), C(SEVEN)};
    const BjHand h = hand(c, 3);
    ck(bjValue(h) == 21 && !bjBlackjack(h), "three sevens is 21 but not a blackjack");
  }
  {
    const uint8_t c[] = {C(ACE), C(FIVE), C(FIVE)};
    ck(!bjBlackjack(hand(c, 3)), "21 reached on the third card is not a blackjack");
  }

  printf("\n--- when the dealer stops ------------------------------------\n");
  {
    const uint8_t c[] = {C(TEN), C(SIX)};
    ck(bjDealerHits(hand(c, 2)), "dealer hits a hard sixteen");
  }
  {
    const uint8_t c[] = {C(TEN), C(SEVEN)};
    ck(!bjDealerHits(hand(c, 2)), "dealer stands on a hard seventeen");
  }
  {
    // The house rule this game picked, stated once in bjDealerHits and checked
    // here so a change to it cannot pass unnoticed.
    const uint8_t c[] = {C(ACE), C(SIX)};
    ck(!bjDealerHits(hand(c, 2)), "dealer stands on a soft seventeen too");
  }
  {
    const uint8_t c[] = {C(ACE), C(FIVE)};
    ck(bjDealerHits(hand(c, 2)), "dealer hits a soft sixteen");
  }

  printf("\n--- settling -------------------------------------------------\n");
  {
    const uint8_t p[] = {C(KING), C(QUEEN), C(TWO)};      // 22
    const uint8_t d[] = {C(KING), C(QUEEN), C(TWO)};      // also 22
    ck(bjSettle(hand(p, 3), hand(d, 3)) == BJ_PLAYER_BUST,
       "busting loses even when the dealer would bust too");
  }
  {
    const uint8_t p[] = {C(TEN), C(NINE)};                // 19
    const uint8_t d[] = {C(KING), C(QUEEN), C(TWO)};      // 22
    ck(bjSettle(hand(p, 2), hand(d, 3)) == BJ_DEALER_BUST, "dealer busts, player wins");
  }
  {
    const uint8_t p[] = {C(ACE), C(KING)};
    const uint8_t d[] = {C(TEN), C(NINE)};
    ck(bjSettle(hand(p, 2), hand(d, 2)) == BJ_PLAYER_BLACKJACK, "blackjack beats nineteen");
  }
  {
    const uint8_t p[] = {C(ACE), C(KING)};
    const uint8_t d[] = {C(ACE), C(QUEEN)};
    ck(bjSettle(hand(p, 2), hand(d, 2)) == BJ_PUSH, "two blackjacks push");
  }
  {
    // The distinction three-card 21 exists for.
    const uint8_t p[] = {C(SEVEN), C(SEVEN), C(SEVEN)};   // 21, not a blackjack
    const uint8_t d[] = {C(ACE), C(KING)};                // blackjack
    ck(bjSettle(hand(p, 3), hand(d, 2)) == BJ_DEALER_WIN,
       "a dealer blackjack beats a three-card 21");
  }
  {
    const uint8_t p[] = {C(TEN), C(NINE)};
    const uint8_t d[] = {C(TEN), C(NINE)};
    ck(bjSettle(hand(p, 2), hand(d, 2)) == BJ_PUSH, "equal totals push");
  }
  {
    const uint8_t p[] = {C(TEN), C(SEVEN)};
    const uint8_t d[] = {C(TEN), C(NINE)};
    ck(bjSettle(hand(p, 2), hand(d, 2)) == BJ_DEALER_WIN, "lower total loses");
  }
  {
    // Every outcome must be scored as exactly one of won/lost/pushed, or the
    // record on screen stops adding up to the hands played.
    const BjOutcome all[] = {BJ_PLAYER_BUST, BJ_DEALER_BUST, BJ_PLAYER_WIN,
                             BJ_DEALER_WIN, BJ_PUSH, BJ_PLAYER_BLACKJACK};
    bool ok = true;
    for (BjOutcome o : all)
      if (bjPlayerWon(o) && bjPlayerLost(o)) ok = false;
    ck(ok, "no outcome counts as both a win and a loss");
    ck(!bjPlayerWon(BJ_PUSH) && !bjPlayerLost(BJ_PUSH), "a push is neither");
  }

  printf("\n--- the shoe -------------------------------------------------\n");
  {
    BjShoe s;
    bool everyDeckComplete = true;
    for (int trial = 0; trial < 200; trial++) {
      bjShuffle(s, rnd, nullptr);
      int seen[BJ_DECK] = {0};
      for (int i = 0; i < BJ_DECK; i++) seen[s.card[i]]++;
      for (int i = 0; i < BJ_DECK; i++) if (seen[i] != 1) everyDeckComplete = false;
    }
    ck(everyDeckComplete, "a shuffle is always a permutation of all 52 cards");
  }
  {
    BjShoe s;
    bjShuffle(s, rnd, nullptr);
    ck(bjRemaining(s) == BJ_DECK, "a fresh shoe has all 52 left");
    for (int i = 0; i < 10; i++) bjDraw(s, rnd, nullptr);
    ck(bjRemaining(s) == BJ_DECK - 10, "and counts down as cards come out");
  }
  {
    // Drawing past the end must reshuffle rather than read off the array.
    BjShoe s;
    bjShuffle(s, rnd, nullptr);
    bool inRange = true;
    for (int i = 0; i < 500; i++) {
      const uint8_t c = bjDraw(s, rnd, nullptr);
      if (c >= BJ_DECK) inRange = false;
    }
    ck(inRange, "drawing past the end reshuffles instead of running off it");
  }

  printf("\n--- a hand never outgrows its array --------------------------\n");
  {
    // Play the game the way BlackjackMode does — the player hits while under
    // 21, the dealer follows its rule — over many shuffles, and watch the
    // sizes. This is what BJ_MAX_CARDS has to be big enough for.
    int worstPlayer = 0, worstDealer = 0, hands = 0, overflow = 0;
    BjShoe s;
    s_rng = 0xC0FFEE;
    bjShuffle(s, rnd, nullptr);
    for (int i = 0; i < 200000; i++) {
      if (bjRemaining(s) < BJ_RESHUFFLE) bjShuffle(s, rnd, nullptr);
      BjHand p, d;
      bjHandClear(p);
      bjHandClear(d);
      if (!bjHandPush(p, bjDraw(s, rnd, nullptr))) overflow++;
      if (!bjHandPush(d, bjDraw(s, rnd, nullptr))) overflow++;
      if (!bjHandPush(p, bjDraw(s, rnd, nullptr))) overflow++;
      if (!bjHandPush(d, bjDraw(s, rnd, nullptr))) overflow++;

      // The most cards a hand can ever take: hit everything below 21.
      while (bjValue(p) < 21 && p.n < BJ_MAX_CARDS)
        if (!bjHandPush(p, bjDraw(s, rnd, nullptr))) { overflow++; break; }
      while (bjDealerHits(d) && d.n < BJ_MAX_CARDS)
        if (!bjHandPush(d, bjDraw(s, rnd, nullptr))) { overflow++; break; }

      if (p.n > worstPlayer) worstPlayer = p.n;
      if (d.n > worstDealer) worstDealer = d.n;
      (void)bjSettle(p, d);
      hands++;
    }
    printf("        %d hands, largest player hand %d, dealer %d, ceiling %d\n",
           hands, worstPlayer, worstDealer, BJ_MAX_CARDS);
    ck(overflow == 0, "no hand ever tried to hold more cards than the array takes");
    ck(worstPlayer <= BJ_MAX_CARDS && worstDealer <= BJ_MAX_CARDS,
       "and every hand stayed inside the ceiling");
  }
  {
    // Pushing onto a full hand must refuse rather than write past the end.
    BjHand h;
    bjHandClear(h);
    for (int i = 0; i < BJ_MAX_CARDS; i++) bjHandPush(h, C(ACE));
    ck(h.n == BJ_MAX_CARDS, "a hand fills to exactly the ceiling");
    ck(!bjHandPush(h, C(TWO)), "and refuses the card after that");
    ck(h.n == BJ_MAX_CARDS, "without growing");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
