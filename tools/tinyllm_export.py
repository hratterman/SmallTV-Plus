#!/usr/bin/env python3
"""Turn a llama2.c TinyStories checkpoint into the single file the cube loads.

    tools/tinyllm_export.py --out story.tll

Downloads karpathy/tinyllamas stories260K (264k parameters) and its 512-entry
tokenizer, quantises the matrix weights to int8, and writes one .tll containing
both. Upload that file on the Story tab.

Why this shape:

  int8, one scale per row.  The fp32 checkpoint is 1.03 MB and the filesystem
      partition holds 960 KB, so fp32 does not fit at all. Per-row scales (as
      opposed to fixed-size groups) survive w2's 172-wide rows without padding
      and give the device exactly one multiply per output element.

  laid out in forward-pass order.  The device streams weights off the
      filesystem as it computes — there is nowhere near enough RAM to hold them
      — so the file is ordered the way the forward pass reads it: embedding,
      then each layer's tensors in use order, then the final norm. Every read is
      sequential, which is the difference between a story you watch appear and
      one you wait for.

  tokenizer included.  One file to upload rather than two to keep in sync.

The RMSNorm weights stay fp32: all of them together are 2.8 KB, and they are
the weights quantisation hurts most.
"""

import argparse
import struct
import sys
import urllib.request

BASE = "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories260K"
MODEL_URL = BASE + "/stories260K.bin"
TOKEN_URL = BASE + "/tok512.bin"

MAGIC = b"TLLM"
VERSION = 1


def fetch(url):
    sys.stderr.write("fetching %s\n" % url)
    with urllib.request.urlopen(url) as r:
        return r.read()


def read_floats(buf, off, n):
    return list(struct.unpack_from("<%df" % n, buf, off)), off + n * 4


def quantize_rows(vals, rows, cols):
    """int8 with one scale per row. Returns (bytes, scales)."""
    out = bytearray(rows * cols)
    scales = []
    for r in range(rows):
        row = vals[r * cols:(r + 1) * cols]
        m = max(abs(v) for v in row) if row else 0.0
        s = m / 127.0 if m > 0 else 1.0
        scales.append(s)
        base = r * cols
        for c, v in enumerate(row):
            q = int(round(v / s))
            out[base + c] = (max(-127, min(127, q))) & 0xFF
    return bytes(out), scales


class Blob:
    """Accumulates the weight section and remembers where each tensor landed."""

    def __init__(self):
        self.buf = bytearray()

    def quant(self, vals, rows, cols):
        # Scales first, then the int8 block. The device reads the scales once
        # (2 KB at the very largest) and then streams the rows straight through
        # a small buffer; the other order would need a seek per block.
        q, scales = quantize_rows(vals, rows, cols)
        self.buf += struct.pack("<%df" % rows, *scales)
        self.buf += q

    def raw(self, vals):
        self.buf += struct.pack("<%df" % len(vals), *vals)


def build(model, token):
    dim, hidden, layers, heads, kv_heads, vocab, seq = struct.unpack_from("<7i", model, 0)
    shared = vocab > 0
    vocab = abs(vocab)
    head_size = dim // heads
    kv_dim = kv_heads * head_size
    if not shared:
        raise SystemExit("this exporter assumes a shared classifier (positive vocab_size)")

    off = 28
    emb, off = read_floats(model, off, vocab * dim)
    rms_att, off = read_floats(model, off, layers * dim)
    wq, off = read_floats(model, off, layers * dim * dim)
    wk, off = read_floats(model, off, layers * kv_dim * dim)
    wv, off = read_floats(model, off, layers * kv_dim * dim)
    wo, off = read_floats(model, off, layers * dim * dim)
    rms_ffn, off = read_floats(model, off, layers * dim)
    w1, off = read_floats(model, off, layers * hidden * dim)
    w2, off = read_floats(model, off, layers * dim * hidden)
    w3, off = read_floats(model, off, layers * hidden * dim)
    rms_final, off = read_floats(model, off, dim)
    # freq_cis tables follow in the legacy format; the device derives RoPE itself.

    b = Blob()
    b.quant(emb, vocab, dim)                     # embedding, and the classifier
    for l in range(layers):
        b.raw(rms_att[l * dim:(l + 1) * dim])
        b.quant(wq[l * dim * dim:(l + 1) * dim * dim], dim, dim)
        b.quant(wk[l * kv_dim * dim:(l + 1) * kv_dim * dim], kv_dim, dim)
        b.quant(wv[l * kv_dim * dim:(l + 1) * kv_dim * dim], kv_dim, dim)
        b.quant(wo[l * dim * dim:(l + 1) * dim * dim], dim, dim)
        b.raw(rms_ffn[l * dim:(l + 1) * dim])
        # w1, w3, w2 — not the checkpoint's w1, w2, w3. SwiGLU needs w1 and w3
        # before w2 can be applied, so storing w2 last is what keeps the device
        # reading forwards instead of seeking back a tensor every layer.
        b.quant(w1[l * hidden * dim:(l + 1) * hidden * dim], hidden, dim)
        b.quant(w3[l * hidden * dim:(l + 1) * hidden * dim], hidden, dim)
        b.quant(w2[l * dim * hidden:(l + 1) * dim * hidden], dim, hidden)
    b.raw(rms_final)

    # Tokenizer: llama2.c's tok*.bin is max_len, then (score, len, bytes)*vocab.
    # Re-emitted compactly, scores kept because the prompt encoder needs them.
    toks = bytearray()
    p = 4
    for _ in range(vocab):
        score = struct.unpack_from("<f", token, p)[0]; p += 4
        ln = struct.unpack_from("<i", token, p)[0]; p += 4
        piece = token[p:p + ln]; p += ln
        if ln > 255:
            raise SystemExit("token longer than 255 bytes")
        toks += struct.pack("<fB", score, ln) + piece

    header = struct.pack("<4sI7i", MAGIC, VERSION, dim, hidden, layers,
                         heads, kv_heads, vocab, seq)
    header += struct.pack("<III", len(header) + 12, len(toks),
                          len(header) + 12 + len(toks))
    return header + bytes(toks) + bytes(b.buf), (dim, hidden, layers, heads,
                                                 kv_heads, vocab, seq)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="story.tll")
    ap.add_argument("--model", help="local stories260K.bin instead of downloading")
    ap.add_argument("--tokenizer", help="local tok512.bin instead of downloading")
    a = ap.parse_args()

    model = open(a.model, "rb").read() if a.model else fetch(MODEL_URL)
    token = open(a.tokenizer, "rb").read() if a.tokenizer else fetch(TOKEN_URL)

    blob, cfg = build(model, token)
    open(a.out, "wb").write(blob)
    dim, hidden, layers, heads, kv_heads, vocab, seq = cfg
    print("wrote %s" % a.out)
    print("  %d bytes (from %d fp32)" % (len(blob), len(model)))
    print("  dim %d  hidden %d  layers %d  heads %d  kv_heads %d  vocab %d"
          % (dim, hidden, layers, heads, kv_heads, vocab))
    print("  KV cache is %d bytes per token of context" % (2 * layers * (kv_heads * (dim // heads)) * 4))


if __name__ == "__main__":
    main()
