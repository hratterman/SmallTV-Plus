// MinerCore.cpp — stratum connection + job pipeline + hash workers.
//
// Ported from BitMaker-hub/NerdMiner_v2 src/mining.cpp (MIT, (c) 2023 Bitmaker).
// The shape is the same — one task owns the pool socket and prepares work,
// worker tasks grind nonce ranges — but the plumbing here is allocation-free (a
// single current-job slot plus fixed rings instead of std::list/shared_ptr),
// since this firmware shares its heap with the web UI, the display, and three
// other feature modes. The job math itself lives in MinerJob.cpp, which is
// checked against real block data by tools/miner_selftest.
#include "config.h"
#if WITH_MINER

#include "MinerCore.h"
#include "MinerJob.h"
#include "Stratum.h"
#include "MinerShaHw.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

// ---- tuning ---------------------------------------------------------------
#define MINER_WORKERS        2        // one per core
#define MINER_NONCE_CHUNK    4096     // nonces per work grab (~25 ms of grinding)
#define MINER_SUGGEST_DIFF   0.00015  // mining.suggest_difficulty, as NerdMiner
#define MINER_KEEPALIVE_MS   30000UL  // idle chatter so the pool holds the socket
#define MINER_NOJOB_MS       600000UL // no mining.notify this long -> reconnect
#define MINER_PENDING        16       // in-flight share submissions tracked
#define MINER_SOLUTIONS      8        // solved-nonce ring

// The task watchdog watches the core-0 idle task (5 s timeout), so a worker that
// never blocks would panic the chip. One tick (1 ms at CONFIG_FREERTOS_HZ=1000)
// handed back twice a second keeps idle fed for ~0.2% of throughput.
#define MINER_YIELD_EVERY_MS 500UL

// ---------------------------------------------------------------------------
// Shared state. s_lock guards everything below except the volatile counters,
// which have exactly one writer each (aligned 32-bit accesses are atomic).
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = nullptr;

// The one job the workers are grinding. seq is bumped on every mining.notify;
// workers compare it to spot both "new work" and "abandon the current chunk".
static MinerWork      s_work;
static volatile uint32_t s_workSeq;   // 0 = no work
static double         s_poolDiff;
static uint32_t       s_nonceCursor;  // next unhanded nonce range

struct Solution {
  uint32_t seq;
  uint32_t nonce;
  double   diff;
  uint8_t  hash[32];
};
static Solution s_sol[MINER_SOLUTIONS];
static uint8_t  s_solHead, s_solTail;

// Per-worker hash counters: one writer each, summed by the stats tick.
static volatile uint32_t s_hashCount[MINER_WORKERS];

// Config snapshot the stratum task works from; epoch is bumped by applyConfig.
static struct {
  volatile uint32_t epoch;
  bool     configured;
  String   host;
  uint16_t port;
  String   user;          // address, or address.worker
} s_cfg;

// Engine selection, read by worker 0 at each chunk boundary so switching
// engines in the web UI takes effect without a reboot.
static volatile bool s_engineHybrid = false;
static volatile bool s_hwFaulted = false;   // self-check failed -> software only

static MinerStats s_stats;
static bool       s_tasksStarted = false;
static TaskHandle_t s_workerTask[MINER_WORKERS] = {};

static inline void lockTake() { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lockGive() { xSemaphoreGive(s_lock); }

static void clearJob() {
  lockTake();
  s_workSeq = 0;
  s_solHead = s_solTail = 0;
  lockGive();
}

// ---------------------------------------------------------------------------
// Hash worker
// ---------------------------------------------------------------------------
#if MINER_HAS_SHA_HW
// Record a digest the SHA peripheral got wrong. The first one of the run is
// kept in full alongside what software expected, because the difference between
// the two identifies the fault where a count alone cannot.
static void hwBadDigest(uint32_t nonce, const uint8_t* got, const uint8_t* want,
                        const uint8_t* mid) {
  lockTake();
  s_stats.badDigests++;
  if (!s_stats.badSampled) {
    s_stats.badSampled = true;
    s_stats.badNonce = nonce;
    memcpy(s_stats.badGot, got, 32);
    memcpy(s_stats.badWant, want, 32);
    memcpy(s_stats.badMid, mid, 32);
  }
  lockGive();
  Serial.printf("[miner] hw digest mismatch at nonce %08lx\n", (unsigned long)nonce);
}

// The register-level engine path cannot be checked off-device, so prove it
// against the software implementation (which tools/miner_selftest checks against
// a real block) before trusting a single share to it. This catches an engine
// that is wrong every time; the per-candidate recheck in the worker is what
// catches one that is wrong occasionally.
static bool hwSelfCheck(const MinerWork& w) {
  const uint32_t testNonce = 0x12345678;
  uint8_t hdr[128];
  memcpy(hdr, w.header, sizeof(hdr));
  memcpy(hdr + 76, &testNonce, 4);

  uint8_t want[32];
  minerSha256d(hdr, 80, want);

  uint8_t swapped[128], got[32];
  minerHwSwapHeader(hdr, swapped);
  minerHwLock();
  minerHwSha256dRaw(swapped, testNonce, got);
  minerHwUnlock();

  bool ok = memcmp(want, got, 32) == 0;
  Serial.printf("[miner] hardware SHA self-check %s\n", ok ? "passed" : "FAILED");
  return ok;
}
#endif

static void minerWorkerTask(void* arg) {
  const uint32_t idx = (uint32_t)(uintptr_t)arg;

  MinerWork work;
  double    poolDiff = 0;
  uint32_t  localSeq = 0;
  uint8_t   hash[32];
  uint32_t  lastYield = millis();
#if MINER_HAS_SHA_HW
  uint8_t   swapped[128];       // engine-order copy of the current header
  bool      hwChecked = false;
  bool      hadHw = false;      // was the hardware path in use last chunk?
#endif

  esp_task_wdt_add(NULL);
  Serial.printf("[miner] worker %u on core %d\n", (unsigned)idx, xPortGetCoreID());

  for (;;) {
    uint32_t nonceStart = 0;
    bool haveWork = false;

    bool newJob = false;
    lockTake();
    if (s_workSeq) {
      if (localSeq != s_workSeq) {
        work = s_work;              // ~250 B, only on a job change
        localSeq = s_workSeq;
        newJob = true;
      }
      poolDiff = s_poolDiff;
      nonceStart = s_nonceCursor;
      s_nonceCursor += MINER_NONCE_CHUNK;
      haveWork = true;
    }
    lockGive();

    if (!haveWork) {
      esp_task_wdt_reset();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    // Worker 0 may drive the SHA peripheral; worker 1 always stays on software,
    // so hybrid mode benchmarks both engines against each other in one run.
    bool useHw = false;
#if MINER_HAS_SHA_HW
    if (idx == 0 && s_engineHybrid && !s_hwFaulted) {
      if (!hwChecked) {
        hwChecked = true;
        if (!hwSelfCheck(work)) s_hwFaulted = true;
      }
      useHw = !s_hwFaulted;
    }
    // Rebuild the byte-swapped header on a new job *or* the first chunk after
    // the engine is switched on: the web UI can flip to hybrid mid-job, and
    // hashing a stale buffer produces plausible-looking shares whose nonces do
    // not reproduce at the pool — silently rejected, every one.
    if (useHw && (newJob || !hadHw)) minerHwSwapHeader(work.header, swapped);
    hadHw = useHw;
#else
    (void)newJob;
#endif
    s_stats.workerHw[idx & 1] = useHw;

    uint32_t done = MINER_NONCE_CHUNK;
    bool hwHeld = false;
    for (uint32_t i = 0; i < MINER_NONCE_CHUNK; i++) {
      const uint32_t nonce = nonceStart + i;
      bool solved;

      if (useHw) {
#if MINER_HAS_SHA_HW
        // Re-take the engine every 256 nonces (well under a millisecond of
        // holding) so mbedTLS — which needs it for the other modes' HTTPS —
        // never waits on a whole chunk.
        if ((i & 0xFF) == 0) {
          if (hwHeld) minerHwUnlock();
          minerHwLock();
          hwHeld = true;
          minerHwPrime(swapped);   // mbedTLS may have used the engine meanwhile
        }
        solved = minerHwSha256d(swapped, nonce, hash);
        // The engine has been caught returning digests that do not reproduce,
        // so every candidate it raises is rechecked in software before it can
        // become a solution. Candidates arrive ~3x a second and a software
        // double hash is ~60 us, so this costs about 0.02% of the core — and
        // unlike the check at submit time it sees *every* bad digest, not only
        // the ~1-in-2^32 that would also clear pool difficulty.
        if (solved) {
          uint8_t check[32];
          memcpy(work.header + 76, &nonce, 4);
          minerSha256dFromMidstate(work.midstate, work.header + 64, check);
          if (memcmp(check, hash, 32) != 0) {
            // Also keep the intermediate. If the engine's answer turns out to
            // be this rather than the double hash, the read is landing before
            // the second load and TEXT still holds the first hash's digest.
            uint8_t inter[32];
            minerSha256(work.header, 80, inter);
            hwBadDigest(nonce, hash, check, inter);
            solved = false;
          }
        }
#else
        solved = false;
#endif
      } else {
        memcpy(work.header + 76, &nonce, 4);
        minerSha256dFromMidstate(work.midstate, work.header + 64, hash);
        // The cheap reject the hardware path gets from its digest register:
        // any hash worth submitting ends in at least 16 zero bits.
        solved = (hash[30] == 0 && hash[31] == 0);
      }

      if (solved) {
        double d = minerDiffFromHash(hash);
        if (d >= poolDiff) {
          lockTake();
          uint8_t next = (uint8_t)((s_solHead + 1) % MINER_SOLUTIONS);
          if (next != s_solTail) {        // ring full -> drop (pool is behind)
            s_sol[s_solHead].seq   = localSeq;
            s_sol[s_solHead].nonce = nonce;
            s_sol[s_solHead].diff  = d;
            memcpy(s_sol[s_solHead].hash, hash, 32);
            s_solHead = next;
          }
          lockGive();
        }
      }

      // Abandon the chunk promptly when the pool sends new work.
      if ((i & 0xFF) == 0xFF && s_workSeq != localSeq) { done = i + 1; break; }
    }
#if MINER_HAS_SHA_HW
    if (hwHeld) minerHwUnlock();
#else
    (void)hwHeld;
#endif

    s_hashCount[idx] += done;
    esp_task_wdt_reset();

    // Hand the core back briefly so the idle task can feed the watchdog.
    uint32_t now = millis();
    if (now - lastYield >= MINER_YIELD_EVERY_MS) {
      lastYield = now;
      vTaskDelay(1);
    }
  }
}

// ---------------------------------------------------------------------------
// Stratum task
// ---------------------------------------------------------------------------
struct PendingShare {
  unsigned long id;
  double        diff;
  bool          used;
};

// Publish freshly built work to the hash workers.
static void publishWork(const MinerWork& w, double poolDiff) {
  lockTake();
  s_work = w;
  s_poolDiff = poolDiff;
  s_workSeq++;                 // hands the new job to the workers
  s_nonceCursor = 0;
  s_solHead = s_solTail = 0;   // solutions for the old job are unsubmittable
  lockGive();
}

static void stratumTask(void*) {
  WiFiClient client;
  StratumSub sub;
  StratumJob job;
  PendingShare pending[MINER_PENDING] = {};

  bool     subscribed = false;
  uint32_t myEpoch = 0;
  String   host, user;
  uint16_t port = 0;
  bool     configured = false;

  double   poolDiff = MINER_SUGGEST_DIFF;
  uint32_t extranonce2Ctr = 0;
  String   extranonce2;

  // mining.authorize is answered asynchronously, so its reply is matched by id
  // from the dispatch loop below. Until it lands we submit optimistically (a
  // pool that never answers should not cost us shares); once it is refused we
  // stop, because from that point the pool turns every share down.
  enum { AUTH_PENDING, AUTH_OK, AUTH_FAILED } auth = AUTH_PENDING;
  unsigned long authId = 0;

  uint32_t lastTxMs = millis(), lastJobMs = millis();
  uint32_t lastRateMs = millis(), startedMs = millis();
  uint32_t lastPer[MINER_WORKERS] = {};   // previous per-worker counter reads
  uint64_t accum[MINER_WORKERS] = {};     // wrap-safe lifetime totals
  uint32_t backoffMs = 2000;

  client.setTimeout(5);   // seconds on ESP32; bounds a truncated line

  for (;;) {
    // --- pick up a config change ---
    if (myEpoch != s_cfg.epoch) {
      lockTake();
      myEpoch    = s_cfg.epoch;
      configured = s_cfg.configured;
      host       = s_cfg.host;
      port       = s_cfg.port;
      user       = s_cfg.user;
      lockGive();
      client.stop();
      subscribed = false;
      clearJob();
      backoffMs = 2000;
    }

    if (!configured || WiFi.status() != WL_CONNECTED) {
      if (subscribed) { client.stop(); subscribed = false; clearJob(); }
      lockTake();
      s_stats.state = configured ? MINER_CONNECTING : MINER_IDLE;
      s_stats.configured = configured;
      lockGive();
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      continue;
    }

    // --- connect + handshake ---
    if (!client.connected()) {
      subscribed = false;
      clearJob();
      lockTake(); s_stats.state = MINER_CONNECTING; lockGive();

      Serial.printf("[miner] connecting to %s:%u\n", host.c_str(), (unsigned)port);
      if (!client.connect(host.c_str(), port)) {
        Serial.println("[miner] connect failed");
        vTaskDelay(backoffMs / portTICK_PERIOD_MS);
        if (backoffMs < 60000) backoffMs *= 2;
        continue;
      }
      client.setNoDelay(true);

      if (!stratumSubscribe(client, sub)) {
        Serial.println("[miner] subscribe failed");
        client.stop();
        vTaskDelay(backoffMs / portTICK_PERIOD_MS);
        if (backoffMs < 60000) backoffMs *= 2;
        continue;
      }
      stratumClearError();
      auth = AUTH_PENDING;
      stratumAuthorize(client, user.c_str(), "x", authId);
      stratumSuggestDifficulty(client, MINER_SUGGEST_DIFF);

      subscribed = true;
      backoffMs = 2000;
      lastTxMs = lastJobMs = millis();
      for (auto& p : pending) p.used = false;
      lockTake();
      s_stats.state = MINER_SUBSCRIBED;
      s_stats.lastError[0] = 0;
      lockGive();
      Serial.printf("[miner] subscribed, extranonce1=%s size=%d\n",
                    sub.extranonce1.c_str(), sub.extranonce2Size);
    }

    // --- drain pool messages ---
    while (client.connected() && client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;

      switch (stratumParseMethod(line)) {
        case STRATUM_NOTIFY: {
          if (!stratumParseNotify(line, job)) {
            Serial.println("[miner] malformed notify, reconnecting");
            client.stop();
            break;
          }
          // A fresh extranonce2 per job so the 32-bit nonce range isn't the
          // only search dimension.
          char e2[24];
          int chars = sub.extranonce2Size * 2;
          if (chars > 16) chars = 16;
          snprintf(e2, sizeof(e2), "%0*lx", chars, (unsigned long)++extranonce2Ctr);
          extranonce2 = e2;

          MinerWork w;
          if (minerBuildWork(job.version.c_str(), job.prevHash.c_str(),
                             job.coinb1.c_str(), sub.extranonce1.c_str(),
                             extranonce2.c_str(), job.coinb2.c_str(),
                             job.merkle, job.merkleCount,
                             job.ntime.c_str(), job.nbits.c_str(), w)) {
            publishWork(w, poolDiff);
            lastJobMs = millis();
            lockTake();
            s_stats.templates++;
            // Jobs keep arriving after a refused authorize; don't let that
            // repaint the screen as if everything were fine.
            s_stats.state = (auth == AUTH_FAILED) ? MINER_AUTH_FAILED : MINER_MINING;
            lockGive();
          } else {
            Serial.println("[miner] could not build work from this job");
          }
          break;
        }
        case STRATUM_SET_DIFFICULTY:
          if (stratumParseSetDifficulty(line, poolDiff)) {
            Serial.printf("[miner] pool difficulty %.6f\n", poolDiff);
            lockTake();
            s_stats.poolDiff = poolDiff;
            s_poolDiff = poolDiff;        // applies to the job in flight too
            lockGive();
          }
          break;
        case STRATUM_SUCCESS: {
          unsigned long id = stratumExtractId(line);
          if (id && id == authId) {
            auth = AUTH_OK;
            Serial.println("[miner] authorized");
            break;
          }
          for (auto& p : pending)
            if (p.used && p.id == id) {
              p.used = false;
              lockTake(); s_stats.accepted++; lockGive();
              Serial.printf("[miner] share accepted (diff %.6f)\n", p.diff);
              break;
            }
          break;
        }
        case STRATUM_PARSE_ERROR: {
          unsigned long id = stratumExtractId(line);
          // Whatever the pool objected to, its wording is the diagnosis; keep
          // it whether or not the frame matches something we are tracking.
          lockTake();
          strlcpy(s_stats.lastError, stratumLastError(), sizeof(s_stats.lastError));
          lockGive();

          // An authorize the pool refuses is the one failure that used to be
          // silent: jobs keep arriving, hashing keeps working, and every share
          // bounces. Stop submitting and say so instead of racking up rejects.
          if (id && id == authId) {
            auth = AUTH_FAILED;
            Serial.printf("[miner] authorize refused: %s\n", stratumLastError());
            lockTake(); s_stats.state = MINER_AUTH_FAILED; lockGive();
            break;
          }
          for (auto& p : pending)
            if (p.used && p.id == id) {
              p.used = false;
              lockTake(); s_stats.rejected++; lockGive();
              Serial.printf("[miner] share rejected: %s\n", stratumLastError());
              break;
            }
          break;
        }
        default:
          break;
      }
    }

    // --- submit solved nonces ---
    // Nothing to gain by submitting into a refused authorization: the pool
    // rejects each one, and the counter climbing hides the real fault.
    while (auth != AUTH_FAILED) {
      Solution sol;
      bool popped = false, fresh = false;
      uint32_t vMid[8];
      uint8_t  vBlock[64];
      lockTake();
      if (s_solTail != s_solHead) {
        sol = s_sol[s_solTail];
        s_solTail = (uint8_t)((s_solTail + 1) % MINER_SOLUTIONS);
        popped = true;
        fresh = (sol.seq == s_workSeq);  // stale job -> nothing to submit against
        if (fresh) {                     // the work this nonce was found against
          memcpy(vMid, s_work.midstate, sizeof(vMid));
          memcpy(vBlock, s_work.header + 64, sizeof(vBlock));
        }
      }
      lockGive();
      if (!popped) break;
      if (!fresh) continue;
      if (!client.connected()) break;

      // Reproduce the hash in software before it goes anywhere. Software is the
      // implementation the self-test checks against real block data; hardware is
      // the one running a fixed 95-cycle spin instead of polling the busy flag,
      // so a digest read a cycle early would otherwise reach the pool as a share
      // that cannot be verified. This costs one double hash per submitted share
      // — a handful per hour — and turns "the pool says no" into a local answer
      // about which engine produced the bad share.
      uint8_t verify[32];
      memcpy(vBlock + 12, &sol.nonce, 4);   // header offset 76 -> block offset 12
      minerSha256dFromMidstate(vMid, vBlock, verify);
      if (memcmp(verify, sol.hash, 32) != 0) {
        lockTake();
        s_stats.unverified++;
        strlcpy(s_stats.lastError, "bad digest (not sent)", sizeof(s_stats.lastError));
        lockGive();
        Serial.printf("[miner] discarded unverifiable solution, nonce %08lx\n",
                      (unsigned long)sol.nonce);
        continue;
      }

      unsigned long id = 0;
      if (stratumSubmit(client, user.c_str(), job, extranonce2, sol.nonce, id)) {
        lastTxMs = millis();
        lockTake();
        s_stats.shares++;
        if (sol.diff > s_stats.bestDiff) s_stats.bestDiff = sol.diff;
        bool isBlock = minerHashMeetsTarget(sol.hash, s_work.targetLE);
        lockGive();
        if (isBlock) Serial.println("[miner] *** hash meets the NETWORK target ***");
        for (auto& p : pending)
          if (!p.used) { p.used = true; p.id = id; p.diff = sol.diff; break; }
      }
    }

    // --- keepalive / stall detection ---
    uint32_t now = millis();
    if (now - lastTxMs > MINER_KEEPALIVE_MS) {
      lastTxMs = now;
      stratumSuggestDifficulty(client, MINER_SUGGEST_DIFF);
    }
    if (now - lastJobMs > MINER_NOJOB_MS) {
      Serial.println("[miner] no job for 10 min, reconnecting");
      client.stop();
      subscribed = false;
      clearJob();
    }

    // --- 1 Hz stats ---
    if (now - lastRateMs >= 1000) {
      uint32_t dt = now - lastRateMs;
      uint32_t rate[MINER_WORKERS];
      for (uint8_t i = 0; i < MINER_WORKERS; i++) {
        uint32_t c = s_hashCount[i];
        uint32_t d = c - lastPer[i];      // unsigned: survives the 32-bit wrap
        lastPer[i] = c;
        accum[i] += d;
        rate[i] = (uint32_t)((uint64_t)d * 1000ULL / dt);
      }
      lastRateMs = now;

      lockTake();
      uint64_t total = 0;
      uint32_t sum = 0;
      for (uint8_t i = 0; i < MINER_WORKERS; i++) {
        total += accum[i];
        sum += rate[i];
        if (i < 2) s_stats.workerRate[i] = rate[i];
      }
      s_stats.totalHashes = total;
      s_stats.hashrate = sum;
      s_stats.uptimeSec = (now - startedMs) / 1000;
      s_stats.hwFaulted = s_hwFaulted;
      lockGive();
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void storeConfig(const Settings& s) {
  String user = s.miner.btcAddress;
  if (s.miner.workerName.length()) user += "." + s.miner.workerName;

  lockTake();
  s_cfg.configured = s.miner.enabled && s.miner.btcAddress.length() > 0;
  s_cfg.host = s.miner.poolHost;
  s_cfg.port = s.miner.poolPort;
  s_cfg.user = user;
  s_cfg.epoch++;
  s_engineHybrid = (s.miner.engine == MINER_ENGINE_HYBRID);
  s_stats.configured = s_cfg.configured;
  strlcpy(s_stats.poolHost, s.miner.poolHost.c_str(), sizeof(s_stats.poolHost));
  lockGive();
}

// Tasks are created once, the first time mining is configured, and then idle
// harmlessly if it is later switched off (they hold ~16 KB of stacks).
static void ensureTasks() {
  if (s_tasksStarted) return;
  s_tasksStarted = true;

  // Workers sit at priority 1 — the same as the Arduino loop task, which sleeps
  // 5 ms per iteration, so they get the cores' leftovers while the display and
  // web server still schedule within a tick. WiFi/lwIP run far above both.
  xTaskCreatePinnedToCore(stratumTask, "miner-net", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(minerWorkerTask, "miner-w0", 4096, (void*)0, 1, &s_workerTask[0], 0);
  xTaskCreatePinnedToCore(minerWorkerTask, "miner-w1", 4096, (void*)1, 1, &s_workerTask[1], 1);
}

void minerCoreBegin(const Settings& s) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.state = MINER_IDLE;
  storeConfig(s);
  if (s_cfg.configured) ensureTasks();
}

void minerCoreApplyConfig(const Settings& s) {
  if (!s_lock) return;
  storeConfig(s);
  if (s_cfg.configured) ensureTasks();
}

void minerCoreReportHwFault(const char* why) {
  Serial.printf("[miner] hardware SHA disabled: %s\n", why ? why : "unspecified");
  s_hwFaulted = true;
  if (s_lock) { lockTake(); s_stats.hwFaulted = true; lockGive(); }
}

void minerCoreSnapshot(MinerStats& out) {
  if (!s_lock) { memset(&out, 0, sizeof(out)); return; }
  lockTake();
  out = s_stats;
  lockGive();
}

#endif  // WITH_MINER
