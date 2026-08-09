// Host-side checks for src/WorkMask.h.
//
// A blocklist that over-matches is worse than none: it censors innocent words
// conspicuously, on a screen whose whole purpose here is to not draw attention.
// Most of these checks are about what must NOT be masked.
#include <cstdio>
#include <cstring>
#include "../../src/WorkMask.h"

static int failures = 0;
static void ckMask(const char* in, const char* block, const char* want) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", in);
  workMaskWords(buf, block);
  const bool good = strcmp(buf, want) == 0;
  printf("  %-5s \"%s\"  +[%s]\n", good ? "ok" : "FAIL", in, block);
  if (!good) {
    printf("        got  \"%s\"\n        want \"%s\"\n", buf, want);
    failures++;
  }
}

int main() {
  printf("--- masks what it should ------------------------------------\n");
  ckMask("the damn song", "damn", "the **** song");
  ckMask("Damn Right", "damn", "**** Right");            // case-insensitive
  ckMask("a b c", "b", "a * c");
  ckMask("one two", "one,two", "*** ***");               // several entries
  ckMask("one two", "one two", "*** ***");               // space-separated list
  ckMask("hell", "hell", "****");                        // whole string
  ckMask("(hell)", "hell", "(****)");                    // punctuation boundary

  printf("\n--- and leaves alone what it should -------------------------\n");
  ckMask("Scunthorpe", "cunt", "Scunthorpe");            // the classic
  ckMask("assassin", "ass", "assassin");
  ckMask("classic", "ass", "classic");
  ckMask("shellfish", "hell", "shellfish");
  ckMask("bassline", "ass", "bassline");
  ckMask("hello", "hell", "hello");
  ckMask("anything", "", "anything");                    // empty blocklist
  ckMask("anything", ",,  ,", "anything");               // blocklist of separators

  printf("\n--- repeated and adjacent occurrences -----------------------\n");
  ckMask("damn damn", "damn", "**** ****");
  ckMask("damn, damn!", "damn", "****, ****!");
  ckMask("damndamn", "damn", "damndamn");                // no boundary between

  printf("\n--- length is preserved (layout depends on it) --------------\n");
  {
    char buf[64] = "a damn long title";
    const size_t before = strlen(buf);
    workMaskWords(buf, "damn");
    const bool good = strlen(buf) == before;
    printf("  %-5s masking never changes the string length\n", good ? "ok" : "FAIL");
    if (!good) failures++;
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
