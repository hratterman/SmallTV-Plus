// Host-side check for TinyLlm: runs the real engine over a real .tll with the
// model in memory instead of on a filesystem, and greedy-decodes. The expected
// text is supplied by run.sh, which derives it from an independent Python
// implementation of the same file format.
#include "../../src/features/story/TinyLlm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* g_blob = nullptr;
static uint32_t g_len = 0;

static bool memRead(void* /*ctx*/, uint32_t off, void* dst, uint32_t len) {
  if ((uint64_t)off + len > g_len) return false;
  memcpy(dst, g_blob + off, len);
  return true;
}

static int g_fail = 0;
static void check(bool ok, const char* what) {
  printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) g_fail++;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: selftest <model.tll> <tokens> <expected>\n"); return 2; }
  const int steps = atoi(argv[2]);

  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  g_len = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  g_blob = (uint8_t*)malloc(g_len);
  if (fread(g_blob, 1, g_len, f) != g_len) { fprintf(stderr, "short read\n"); return 2; }
  fclose(f);

  printf("--- model ---------------------------------------------------\n");
  TinyLlm llm;
  check(llm.begin(memRead, nullptr, 0), "header parses and buffers allocate");
  if (!llm.ready()) return 1;

  const TinyLlmConfig& c = llm.cfg();
  printf("       dim %d hidden %d layers %d heads %d kv %d vocab %d ctx %d\n",
         c.dim, c.hidden, c.layers, c.heads, c.kvHeads, c.vocab, llm.seqLimit());
  check(c.dim == 64 && c.layers == 5 && c.vocab == 512, "config matches stories260K");

  printf("--- tokenizer -----------------------------------------------\n");
  // Round-tripping a phrase the model was trained on exercises the merge pass
  // and the leading-space folding at once.
  const char* phrase = " Once upon a time";
  int ids[64];
  const int n = llm.encode(phrase, ids, 64);
  check(n > 0, "prompt encodes to at least one token");
  char back[256] = {0};
  for (int i = 0; i < n; i++) strncat(back, llm.piece(ids[i]), sizeof(back) - strlen(back) - 1);
  check(strcmp(back, phrase) == 0, "encode then decode reproduces the prompt");
  if (strcmp(back, phrase) != 0) printf("       got \"%s\"\n", back);

  printf("--- greedy decode from BOS ----------------------------------\n");
  char out[1024] = {0};
  int tok = TinyLlm::kBos;
  uint32_t rng = 1;
  for (int pos = 0; pos < steps && pos < llm.seqLimit(); pos++) {
    const int nxt = llm.step(tok, pos, 0.0f, 0.9f, &rng);   // temp 0 = greedy
    if (nxt == TinyLlm::kBos || nxt == TinyLlm::kEos) break;
    strncat(out, llm.piece(nxt), sizeof(out) - strlen(out) - 1);
    tok = nxt;
  }
  printf("       \"%s\"\n", out);
  check(strcmp(out, argv[3]) == 0,
        "matches the independent Python decode of the same file");
  if (strcmp(out, argv[3]) != 0) printf("       want \"%s\"\n", argv[3]);

  llm.end();
  printf("-------------------------------------------------------------\n");
  printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
