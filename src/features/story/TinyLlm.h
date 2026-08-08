// TinyLlm.h — a llama2-architecture transformer small enough to run on the cube.
//
// The model is stories260K from karpathy/tinyllamas: 264k parameters, trained
// on TinyStories, quantised to int8 by tools/tinyllm_export.py. It writes
// simple children's stories and can do nothing else — no questions, no
// instructions, no facts. That is the whole capability at this size, and it is
// a hard ceiling: the useful range starts a thousand times further up.
//
// The interesting constraint is not compute, it is memory. 274 KB of weights
// against ~150 KB of free heap means the weights are never resident: every
// matmul streams its rows off the filesystem through a 4 KB window as it
// multiplies. The file is written in forward-pass order so those reads are
// sequential, and reads dominate the token time, not arithmetic.
//
// Like MinerJob this file is deliberately free of Arduino, FreeRTOS and
// LittleFS so it can be compiled and checked on a host machine against a
// reference implementation (tools/story_selftest). Bytes arrive through a
// caller-supplied reader, which is a file on the device and a memory buffer in
// the test.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Reads `len` bytes at `off` from the model blob. Returns false on short read.
typedef bool (*TinyLlmRead)(void* ctx, uint32_t off, void* dst, uint32_t len);

struct TinyLlmConfig {
  int32_t dim, hidden, layers, heads, kvHeads, vocab, seq;
};

// Context lengths worth trying, longest first. The KV cache is the only large
// allocation (1280 bytes per token for this model) and it is what decides how
// long a story can get, so the device takes the longest one that will fit
// rather than failing on a fixed choice.
#define TINYLLM_SEQ_CHOICES { 96, 80, 64, 48, 32 }

class TinyLlm {
 public:
  // Parses the header, loads the tokenizer, and allocates working memory.
  // maxSeq caps the context; the achieved value is seqLimit() afterwards.
  bool begin(TinyLlmRead rd, void* ctx, int maxSeq);
  void end();
  bool ready() const { return ready_; }

  const TinyLlmConfig& cfg() const { return cfg_; }
  int  seqLimit() const { return seqLimit_; }

  // One decode step: runs the forward pass for `token` at `pos` and samples.
  // temp 0 is greedy; topp bounds the sampled set by cumulative probability.
  int step(int token, int pos, float temp, float topp, uint32_t* rng);

  // Text for a token id, or "" if out of range. The leading-space marker the
  // tokenizer uses is already translated.
  const char* piece(int token) const;

  // Encodes `text` into ids (no BOS). Returns how many were written.
  int encode(const char* text, int* out, int maxOut) const;

  static const int kBos = 1;
  static const int kEos = 2;

 private:
  bool  loadTokenizer(uint32_t off, uint32_t len);
  void  forward(int token, int pos);
  // out[rows] = W[rows][cols] . x[cols], with W streamed from `off`.
  void  matmul(float* out, const float* x, uint32_t off, int rows, int cols);
  int   sample(float temp, float topp, uint32_t* rng);

  TinyLlmRead   rd_ = nullptr;
  void*         ctx_ = nullptr;
  TinyLlmConfig cfg_ = {};
  bool          ready_ = false;
  int           seqLimit_ = 0;
  int           headSize_ = 0, kvDim_ = 0;
  uint32_t      wOff_ = 0;         // start of the weight section

  // Tokenizer: one blob of bytes plus an index, so 512 entries cost one
  // allocation instead of 512.
  char*     tokBlob_ = nullptr;
  uint16_t* tokAt_ = nullptr;      // offset of each piece within tokBlob_
  uint8_t*  tokLen_ = nullptr;
  float*    tokScore_ = nullptr;

  // Streaming buffers. scale_ holds one tensor's row scales (2 KB at the
  // largest, the classifier); qbuf_ is the window the int8 rows pass through.
  float*   scale_ = nullptr;
  int8_t*  qbuf_ = nullptr;
  int      qbufRows_ = 0;

  // Activations.
  float *x_ = nullptr, *xb_ = nullptr, *xb2_ = nullptr;
  float *hb_ = nullptr, *hb2_ = nullptr;
  float *q_ = nullptr, *att_ = nullptr, *logits_ = nullptr;
  float *kcache_ = nullptr, *vcache_ = nullptr;
};
