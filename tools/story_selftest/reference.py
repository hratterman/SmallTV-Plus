#!/usr/bin/env python3
# An independent reader for the .tll format, used only by the self-test.
#
# It shares no code with src/features/story/TinyLlm.cpp: same file, two
# implementations, and the test requires them to agree token for token. A wrong
# tensor offset in the C++ would otherwise still emit fluent-looking text, which
# is exactly the kind of bug that survives eyeballing.
#
#     reference.py <model.tll> [tokens]   -> greedy decode on stdout
import struct, sys, math

f = open(sys.argv[1], 'rb').read()
magic, ver, dim, hidden, layers, heads, kv_heads, vocab, seq = struct.unpack_from('<4sI7i', f, 0)
tokOff, tokLen, wOff = struct.unpack_from('<III', f, 36)
assert magic == b'TLLM' and ver == 1
head_size = dim // heads
kv_dim = kv_heads * head_size

# tokenizer
vocab_str, scores = [], []
p = tokOff
for _ in range(vocab):
    sc, ln = struct.unpack_from('<fB', f, p); p += 5
    vocab_str.append(f[p:p+ln]); scores.append(sc); p += ln
assert p == tokOff + tokLen, (p, tokOff + tokLen)

cur = wOff
def qtensor(rows, cols):
    global cur
    s = struct.unpack_from('<%df' % rows, f, cur); cur += rows*4
    q = f[cur:cur+rows*cols]; cur += rows*cols
    return q, s
def rtensor(n):
    global cur
    v = struct.unpack_from('<%df' % n, f, cur); cur += n*4
    return v

emb = qtensor(vocab, dim)
L = []
for _ in range(layers):
    e = {}
    e['rms_att'] = rtensor(dim)
    e['wq'] = qtensor(dim, dim)
    e['wk'] = qtensor(kv_dim, dim)
    e['wv'] = qtensor(kv_dim, dim)
    e['wo'] = qtensor(dim, dim)
    e['rms_ffn'] = rtensor(dim)
    e['w1'] = qtensor(hidden, dim)
    e['w3'] = qtensor(hidden, dim)
    e['w2'] = qtensor(dim, hidden)
    L.append(e)
rms_final = rtensor(dim)
assert cur == len(f), (cur, len(f))
print('# layout consistent: consumed exactly %d bytes' % cur, file=sys.stderr)

def s8(b): return b - 256 if b > 127 else b

def matmul(out_n, w, x, in_n):
    q, sc = w
    o = [0.0]*out_n
    for r in range(out_n):
        base = r*in_n; acc = 0
        for c in range(in_n):
            acc += s8(q[base+c]) * x[c]
        o[r] = acc * sc[r]
    return o

def matmul_q(out_n, w, xq, in_n):
    # x pre-quantised is not used here; reference keeps x in float for clarity
    return matmul(out_n, w, xq, in_n)

def rmsnorm(x, wgt, n):
    ss = sum(v*v for v in x)/n + 1e-5
    r = 1.0/math.sqrt(ss)
    return [x[i]*r*wgt[i] for i in range(n)]

kcache = [[0.0]*(seq*kv_dim) for _ in range(layers)]
vcache = [[0.0]*(seq*kv_dim) for _ in range(layers)]

def forward(token, pos):
    q8, qs = emb
    x = [s8(q8[token*dim+i])*qs[token] for i in range(dim)]
    for li, e in enumerate(L):
        xb = rmsnorm(x, e['rms_att'], dim)
        q = matmul(dim, e['wq'], xb, dim)
        k = matmul(kv_dim, e['wk'], xb, dim)
        v = matmul(kv_dim, e['wv'], xb, dim)
        # RoPE
        for i in range(0, dim, 2):
            hd = i % head_size
            freq = 1.0 / (10000.0 ** (hd/head_size))
            val = pos*freq; fcr, fci = math.cos(val), math.sin(val)
            q[i], q[i+1] = q[i]*fcr - q[i+1]*fci, q[i]*fci + q[i+1]*fcr
            if i < kv_dim:
                k[i], k[i+1] = k[i]*fcr - k[i+1]*fci, k[i]*fci + k[i+1]*fcr
        kcache[li][pos*kv_dim:(pos+1)*kv_dim] = k
        vcache[li][pos*kv_dim:(pos+1)*kv_dim] = v
        xb = [0.0]*dim
        for h in range(heads):
            kvh = h // (heads // kv_heads)
            att = []
            for t in range(pos+1):
                off = t*kv_dim + kvh*head_size
                sc = sum(q[h*head_size+i]*kcache[li][off+i] for i in range(head_size))
                att.append(sc/math.sqrt(head_size))
            m = max(att); ex = [math.exp(a-m) for a in att]; ssum = sum(ex)
            att = [a/ssum for a in ex]
            for i in range(head_size):
                xb[h*head_size+i] = sum(att[t]*vcache[li][t*kv_dim+kvh*head_size+i]
                                        for t in range(pos+1))
        xb2 = matmul(dim, e['wo'], xb, dim)
        x = [x[i]+xb2[i] for i in range(dim)]
        xb = rmsnorm(x, e['rms_ffn'], dim)
        h1 = matmul(hidden, e['w1'], xb, dim)
        h3 = matmul(hidden, e['w3'], xb, dim)
        hb = [h1[i]*(1.0/(1.0+math.exp(-h1[i])))*h3[i] for i in range(hidden)]
        xb2 = matmul(dim, e['w2'], hb, hidden)
        x = [x[i]+xb2[i] for i in range(dim)]
    x = rmsnorm(x, rms_final, dim)
    return matmul(vocab, emb, x, dim)

# Greedy decode from BOS for exactly `tokens` steps, so the C++ can be asked
# for the same count and the two strings compared whole.
tokens = int(sys.argv[2]) if len(sys.argv) > 2 else 40
tok, out = 1, []
for pos in range(min(tokens, seq)):
    logits = forward(tok, pos)
    nxt = max(range(vocab), key=lambda i: logits[i])
    if nxt in (1, 2):
        break
    out.append(vocab_str[nxt].decode('utf-8', 'replace'))
    tok = nxt
sys.stdout.write(''.join(out).replace('\u2581', ' '))
