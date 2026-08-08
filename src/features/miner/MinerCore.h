// MinerCore.h — the mining engine: owns the stratum connection, prepares jobs,
// runs the hash worker tasks, and publishes stats for MinerMode to render.
//
// Threading (classic ESP32, dual core):
//   - Stratum task: pinned core 0, low priority. Owns the pool socket; on each
//     mining.notify it builds the 80-byte header, precomputes midstate + bake,
//     and pushes nonce-range jobs into the queue. Drains solved nonces and
//     submits them.
//   - Hash workers: one per core, priority below the Arduino loop task so the
//     display and web server always preempt. They grind nonce ranges and push
//     results back.
// Mining runs whenever it is enabled and a BTC address is set, independent of
// the active display mode. The engine starts on first begin() and is
// reconfigured (reconnect) via applyConfig().
#pragma once
#include "config.h"
#if WITH_MINER

#include <Arduino.h>
#include "Settings.h"

enum MinerPoolState : uint8_t {
  MINER_IDLE = 0,     // disabled or no BTC address
  MINER_CONNECTING,   // resolving / opening the socket
  MINER_SUBSCRIBED,   // subscribed + authorized, waiting for first job
  MINER_MINING,       // has a job, hashing
};

// A snapshot of engine state for the UI. Copied under the engine lock so the
// renderer never sees a torn multi-field read.
struct MinerStats {
  MinerPoolState state;
  bool     configured;     // enabled && address set
  uint32_t templates;      // jobs received from the pool
  double   poolDiff;       // current pool share difficulty
  double   bestDiff;       // best share difficulty seen this run
  uint32_t shares;         // shares submitted (met pool diff)
  uint32_t accepted;       // shares the pool acknowledged
  uint32_t rejected;       // shares the pool rejected
  uint64_t totalHashes;    // lifetime hashes this run
  uint32_t hashrate;       // H/s, updated ~1 Hz
  uint32_t uptimeSec;      // seconds since the engine started
  char     poolHost[64];
};

void minerCoreBegin(const Settings& s);
void minerCoreApplyConfig(const Settings& s);   // pool/address changed -> reconnect
void minerCoreSnapshot(MinerStats& out);

#endif  // WITH_MINER
