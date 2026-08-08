#include "TinyLlm.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
bool TinyLlm::begin(TinyLlmRead rd, void* ctx, int maxSeq) {
  end();
  rd_ = rd;
  ctx_ = ctx;

  uint8_t hdr[48];
  if (!rd_(ctx_, 0, hdr, sizeof(hdr))) return false;
  if (memcmp(hdr, "TLLM", 4) != 0 || rd32(hdr + 4) != 1) return false;

  cfg_.dim     = (int32_t)rd32(hdr + 8);
  cfg_.hidden  = (int32_t)rd32(hdr + 12);
  cfg_.layers  = (int32_t)rd32(hdr + 16);
  cfg_.heads   = (int32_t)rd32(hdr + 20);
  cfg_.kvHeads = (int32_t)rd32(hdr + 24);
  cfg_.vocab   = (int32_t)rd32(hdr + 28);
  cfg_.seq     = (int32_t)rd32(hdr + 32);
  const uint32_t tokOff = rd32(hdr + 36);
  const uint32_t tokLen = rd32(hdr + 40);
  wOff_ = rd32(hdr + 44);

  if (cfg_.dim <= 0 || cfg_.heads <= 0 || cfg_.kvHeads <= 0 || cfg_.vocab <= 0 ||
      cfg_.layers <= 0 || cfg_.hidden <= 0 || cfg_.dim % cfg_.heads != 0 ||
      cfg_.heads % cfg_.kvHeads != 0)
    return false;
  headSize_ = cfg_.dim / cfg_.heads;
  kvDim_    = cfg_.kvHeads * headSize_;

  if (!loadTokenizer(tokOff, tokLen)) { end(); return false; }

  const int maxRows = cfg_.vocab > cfg_.hidden ? cfg_.vocab : cfg_.hidden;
  const int maxCols = cfg_.hidden > cfg_.dim ? cfg_.hidden : cfg_.dim;
  scale_ = (float*)malloc(sizeof(float) * maxRows);
  qbufRows_ = 4096 / maxCols;
  if (qbufRows_ < 1) qbufRows_ = 1;
  qbuf_ = (int8_t*)malloc((size_t)qbufRows_ * maxCols);

  x_      = (float*)malloc(sizeof(float) * cfg_.dim);
  xb_     = (float*)malloc(sizeof(float) * cfg_.dim);
  xb2_    = (float*)malloc(sizeof(float) * cfg_.dim);
  hb_     = (float*)malloc(sizeof(float) * cfg_.hidden);
  hb2_    = (float*)malloc(sizeof(float) * cfg_.hidden);
  q_      = (float*)malloc(sizeof(float) * cfg_.dim);
  logits_ = (float*)malloc(sizeof(float) * cfg_.vocab);
  if (!scale_ || !qbuf_ || !x_ || !xb_ || !xb2_ || !hb_ || !hb2_ || !q_ || !logits_) {
    end();
    return false;
  }

  static const int kChoices[] = TINYLLM_SEQ_CHOICES;
  const size_t per = (size_t)cfg_.layers * kvDim_ * sizeof(float);
  for (size_t i = 0; i < sizeof(kChoices) / sizeof(kChoices[0]); i++) {
    const int s = kChoices[i];
    if (maxSeq > 0 && s > maxSeq) continue;
    if (s > cfg_.seq) continue;
    kcache_ = (float*)malloc(per * s);
    vcache_ = (float*)malloc(per * s);
    att_    = (float*)malloc(sizeof(float) * s);
    if (kcache_ && vcache_ && att_) { seqLimit_ = s; break; }
    free(kcache_); free(vcache_); free(att_);
    kcache_ = vcache_ = att_ = nullptr;
  }
  if (!seqLimit_) { end(); return false; }

  ready_ = true;
  return true;
}

void TinyLlm::end() {
  free(tokBlob_);  tokBlob_ = nullptr;
  free(tokAt_);    tokAt_ = nullptr;
  free(tokLen_);   tokLen_ = nullptr;
  free(tokScore_); tokScore_ = nullptr;
  free(scale_);    scale_ = nullptr;
  free(qbuf_);     qbuf_ = nullptr;
  free(x_);   x_ = nullptr;
  free(xb_);  xb_ = nullptr;
  free(xb2_); xb2_ = nullptr;
  free(hb_);  hb_ = nullptr;
  free(hb2_); hb2_ = nullptr;
  free(q_);   q_ = nullptr;
  free(att_); att_ = nullptr;
  free(logits_); logits_ = nullptr;
  free(kcache_); kcache_ = nullptr;
  free(vcache_); vcache_ = nullptr;
  ready_ = false;
  seqLimit_ = 0;
}

// ---------------------------------------------------------------------------
// tokenizer
// ---------------------------------------------------------------------------
bool TinyLlm::loadTokenizer(uint32_t off, uint32_t len) {
  uint8_t* raw = (uint8_t*)malloc(len);
  if (!raw) return false;
  if (!rd_(ctx_, off, raw, len)) { free(raw); return false; }

  const int n = cfg_.vocab;
  tokBlob_  = (char*)malloc(len + n);        // room for the NUL after each piece
  tokAt_    = (uint16_t*)malloc(sizeof(uint16_t) * n);
  tokLen_   = (uint8_t*)malloc(n);
  tokScore_ = (float*)malloc(sizeof(float) * n);
  if (!tokBlob_ || !tokAt_ || !tokLen_ || !tokScore_) { free(raw); return false; }

  uint32_t p = 0, w = 0;
  for (int i = 0; i < n; i++) {
    if (p + 5 > len) { free(raw); return false; }
    memcpy(&tokScore_[i], raw + p, 4); p += 4;
    uint8_t l = raw[p++];
    if (p + l > len) { free(raw); return false; }

    const uint8_t* src = raw + p;
    tokAt_[i] = (uint16_t)w;
    // SentencePiece writes a leading space as U+2581 (E2 96 81). Fold it back
    // to a real space here so that every consumer — rendering, prompt
    // encoding, width measurement — sees ordinary text and none of them has to
    // know the convention.
    if (l >= 3 && src[0] == 0xE2 && src[1] == 0x96 && src[2] == 0x81) {
      tokBlob_[w++] = ' ';
      memcpy(tokBlob_ + w, src + 3, l - 3);
      w += l - 3;
      tokLen_[i] = (uint8_t)(l - 2);
    } else {
      memcpy(tokBlob_ + w, src, l);
      w += l;
      tokLen_[i] = l;
    }
    tokBlob_[w++] = 0;
    p += l;
  }
  free(raw);
  return true;
}

const char* TinyLlm::piece(int token) const {
  if (!tokBlob_ || token < 0 || token >= cfg_.vocab) return "";
  return tokBlob_ + tokAt_[token];
}

// Byte-pair encoding, as llama2.c does it: start from the longest single
// pieces, then keep merging the best-scoring adjacent pair until none merges.
// Prompts are a few words, so the repeated linear scan is not worth avoiding.
int TinyLlm::encode(const char* text, int* out, int maxOut) const {
  if (!text || !tokBlob_ || maxOut <= 0) return 0;

  int n = 0;
  for (const char* p = text; *p && n < maxOut; ) {
    // Longest piece matching here, so multi-byte characters stay whole.
    int best = -1, bestLen = 0;
    for (int i = 0; i < cfg_.vocab; i++) {
      const int l = tokLen_[i];
      if (l == 0 || l <= bestLen) continue;
      if (strncmp(p, tokBlob_ + tokAt_[i], l) == 0) { best = i; bestLen = l; }
    }
    if (best < 0) { p++; continue; }          // nothing represents this byte
    out[n++] = best;
    p += bestLen;
  }

  for (;;) {
    int bestAt = -1, bestId = -1;
    float bestScore = -1e30f;
    for (int i = 0; i + 1 < n; i++) {
      const char* a = tokBlob_ + tokAt_[out[i]];
      const char* b = tokBlob_ + tokAt_[out[i + 1]];
      const int la = tokLen_[out[i]], lb = tokLen_[out[i + 1]];
      for (int c = 0; c < cfg_.vocab; c++) {
        if (tokLen_[c] != la + lb) continue;
        const char* m = tokBlob_ + tokAt_[c];
        if (strncmp(m, a, la) || strncmp(m + la, b, lb)) continue;
        if (tokScore_[c] > bestScore) { bestScore = tokScore_[c]; bestAt = i; bestId = c; }
        break;
      }
    }
    if (bestAt < 0) break;
    out[bestAt] = bestId;
    for (int i = bestAt + 1; i + 1 < n; i++) out[i] = out[i + 1];
    n--;
  }
  return n;
}

// ---------------------------------------------------------------------------
// forward pass
// ---------------------------------------------------------------------------
static void rmsnorm(float* o, const float* x, const float* w, int n) {
  float ss = 0;
  for (int i = 0; i < n; i++) ss += x[i] * x[i];
  ss = 1.0f / sqrtf(ss / n + 1e-5f);
  for (int i = 0; i < n; i++) o[i] = x[i] * ss * w[i];
}

static void softmax(float* v, int n) {
  float m = v[0];
  for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
  float sum = 0;
  for (int i = 0; i < n; i++) { v[i] = expf(v[i] - m); sum += v[i]; }
  for (int i = 0; i < n; i++) v[i] /= sum;
}

void TinyLlm::matmul(float* out, const float* x, uint32_t off, int rows, int cols) {
  if (!rd_(ctx_, off, scale_, sizeof(float) * rows)) return;
  uint32_t p = off + sizeof(float) * rows;
  for (int r0 = 0; r0 < rows; r0 += qbufRows_) {
    int nr = rows - r0;
    if (nr > qbufRows_) nr = qbufRows_;
    if (!rd_(ctx_, p, qbuf_, (uint32_t)nr * cols)) return;
    p += (uint32_t)nr * cols;
    for (int r = 0; r < nr; r++) {
      const int8_t* w = qbuf_ + (size_t)r * cols;
      float acc = 0;
      for (int c = 0; c < cols; c++) acc += (float)w[c] * x[c];
      out[r0 + r] = acc * scale_[r0 + r];
    }
  }
}

void TinyLlm::forward(int token, int pos) {
  const int dim = cfg_.dim, hidden = cfg_.hidden;
  const int kvMul = cfg_.heads / cfg_.kvHeads;

  // Embedding: one row of the table the classifier shares.
  {
    float s;
    if (!rd_(ctx_, wOff_ + (uint32_t)token * sizeof(float), &s, sizeof(float))) return;
    const uint32_t base = wOff_ + (uint32_t)cfg_.vocab * sizeof(float);
    if (!rd_(ctx_, base + (uint32_t)token * dim, qbuf_, (uint32_t)dim)) return;
    for (int i = 0; i < dim; i++) x_[i] = (float)qbuf_[i] * s;
  }

  const uint32_t vecsz = (uint32_t)dim * sizeof(float);
  const uint32_t qsz   = vecsz + (uint32_t)dim * dim;
  const uint32_t kvsz  = (uint32_t)kvDim_ * sizeof(float) + (uint32_t)kvDim_ * dim;
  const uint32_t w13sz = (uint32_t)hidden * sizeof(float) + (uint32_t)hidden * dim;
  const uint32_t w2sz  = vecsz + (uint32_t)dim * hidden;

  uint32_t p = wOff_ + (uint32_t)cfg_.vocab * (sizeof(float) + dim);

  for (int l = 0; l < cfg_.layers; l++) {
    float* krow = kcache_ + ((size_t)l * seqLimit_ + pos) * kvDim_;
    float* vrow = vcache_ + ((size_t)l * seqLimit_ + pos) * kvDim_;

    if (!rd_(ctx_, p, xb2_, vecsz)) return;                 // rms_att
    p += vecsz;
    rmsnorm(xb_, x_, xb2_, dim);

    matmul(q_,   xb_, p, dim,    dim); p += qsz;            // wq
    matmul(krow, xb_, p, kvDim_, dim); p += kvsz;           // wk
    matmul(vrow, xb_, p, kvDim_, dim); p += kvsz;           // wv

    for (int i = 0; i < dim; i += 2) {                      // RoPE
      const int hd = i % headSize_;
      const float freq = 1.0f / powf(10000.0f, (float)hd / (float)headSize_);
      const float v = pos * freq, fcr = cosf(v), fci = sinf(v);
      const float q0 = q_[i], q1 = q_[i + 1];
      q_[i]     = q0 * fcr - q1 * fci;
      q_[i + 1] = q0 * fci + q1 * fcr;
      if (i < kvDim_) {
        const float k0 = krow[i], k1 = krow[i + 1];
        krow[i]     = k0 * fcr - k1 * fci;
        krow[i + 1] = k0 * fci + k1 * fcr;
      }
    }

    for (int h = 0; h < cfg_.heads; h++) {
      const int kvh = h / kvMul;
      const float* qh = q_ + h * headSize_;
      for (int t = 0; t <= pos; t++) {
        const float* k = kcache_ + ((size_t)l * seqLimit_ + t) * kvDim_ + kvh * headSize_;
        float s = 0;
        for (int i = 0; i < headSize_; i++) s += qh[i] * k[i];
        att_[t] = s / sqrtf((float)headSize_);
      }
      softmax(att_, pos + 1);
      float* dst = xb_ + h * headSize_;
      for (int i = 0; i < headSize_; i++) dst[i] = 0;
      for (int t = 0; t <= pos; t++) {
        const float* v = vcache_ + ((size_t)l * seqLimit_ + t) * kvDim_ + kvh * headSize_;
        const float a = att_[t];
        for (int i = 0; i < headSize_; i++) dst[i] += a * v[i];
      }
    }

    matmul(xb2_, xb_, p, dim, dim); p += qsz;               // wo
    for (int i = 0; i < dim; i++) x_[i] += xb2_[i];

    if (!rd_(ctx_, p, xb2_, vecsz)) return;                 // rms_ffn
    p += vecsz;
    rmsnorm(xb_, x_, xb2_, dim);

    // SwiGLU. The export stores w1, w3, w2 in that order precisely so this
    // reads straight through without a seek.
    matmul(hb_,  xb_, p, hidden, dim); p += w13sz;          // w1
    matmul(hb2_, xb_, p, hidden, dim); p += w13sz;          // w3
    for (int i = 0; i < hidden; i++) {
      const float v = hb_[i];
      hb_[i] = v * (1.0f / (1.0f + expf(-v))) * hb2_[i];
    }
    matmul(xb2_, hb_, p, dim, hidden); p += w2sz;           // w2
    for (int i = 0; i < dim; i++) x_[i] += xb2_[i];
  }

  if (!rd_(ctx_, p, xb_, vecsz)) return;                    // rms_final
  rmsnorm(xb2_, x_, xb_, dim);
  matmul(logits_, xb2_, wOff_, cfg_.vocab, dim);            // classifier = embedding
}

// ---------------------------------------------------------------------------
// sampling
// ---------------------------------------------------------------------------
static float rnd01(uint32_t* s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  *s = v;
  return (float)(v >> 8) / 16777216.0f;
}

int TinyLlm::sample(float temp, float topp, uint32_t* rng) {
  const int n = cfg_.vocab;
  if (temp <= 0.0f) {
    int best = 0;
    for (int i = 1; i < n; i++) if (logits_[i] > logits_[best]) best = i;
    return best;
  }
  for (int i = 0; i < n; i++) logits_[i] /= temp;
  softmax(logits_, n);

  // Top-p by repeated selection rather than sorting: the set that reaches p is
  // a couple of dozen tokens, so taking maxima costs less than ordering 512.
  const int kMax = 48;
  int   idx[kMax];
  float prob[kMax];
  float cum = 0;
  int   m = 0;
  while (m < kMax) {
    int best = -1;
    for (int i = 0; i < n; i++)
      if (logits_[i] >= 0 && (best < 0 || logits_[i] > logits_[best])) best = i;
    if (best < 0) break;
    idx[m] = best;
    prob[m] = logits_[best];
    cum += prob[m];
    m++;
    logits_[best] = -1.0f;                    // taken
    if (cum >= topp) break;
  }
  if (m == 0) return kEos;

  const float r = rnd01(rng) * cum;
  float acc = 0;
  for (int i = 0; i < m; i++) {
    acc += prob[i];
    if (r <= acc) return idx[i];
  }
  return idx[m - 1];
}

int TinyLlm::step(int token, int pos, float temp, float topp, uint32_t* rng) {
  if (!ready_ || pos < 0 || pos >= seqLimit_) return kEos;
  forward(token, pos);
  return sample(temp, topp, rng);
}
