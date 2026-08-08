// Stratum.h — stratum-v1 mining protocol client (subscribe / authorize /
// notify / set_difficulty / submit) over a plain TCP WiFiClient.
//
// Ported from BitMaker-hub/NerdMiner_v2 src/stratum.{h,cpp} with two changes:
// the JSON calls use the ArduinoJson 7 API this repo ships, and a job's merkle
// branches are decoded to bytes at parse time instead of holding a JsonArray
// reference into a reused document (which dangles after the next parse).
#pragma once
#include "config.h"
#if WITH_MINER

#include <Arduino.h>
#include <WiFi.h>

#define STRATUM_MAX_MERKLE 32

// mining.subscribe result — the per-connection coinbase extranonce space.
struct StratumSub {
  String extranonce1;      // hex, pool-assigned
  int    extranonce2Size;  // bytes of extranonce2 the pool expects
};

// One mining.notify job, merkle branches already hex-decoded.
struct StratumJob {
  String  jobId;
  String  prevHash;        // hex, 64 chars
  String  coinb1, coinb2;  // hex halves of the coinbase around the extranonces
  String  version, nbits, ntime;  // hex, 8 chars each
  bool    cleanJobs;
  uint8_t merkle[STRATUM_MAX_MERKLE][32];
  uint8_t merkleCount;
};

enum StratumMethod {
  STRATUM_SUCCESS,        // result frame with error:null (ack for one of our ids)
  STRATUM_UNKNOWN,
  STRATUM_PARSE_ERROR,    // includes result frames carrying an error
  STRATUM_NOTIFY,
  STRATUM_SET_DIFFICULTY,
};

bool stratumSubscribe(WiFiClient& c, StratumSub& sub);   // tx + parse the reply
bool stratumAuthorize(WiFiClient& c, const char* user, const char* pass);
bool stratumSuggestDifficulty(WiFiClient& c, double difficulty);

StratumMethod stratumParseMethod(const String& line);
bool stratumParseNotify(const String& line, StratumJob& job);
bool stratumParseSetDifficulty(const String& line, double& difficulty);
unsigned long stratumExtractId(const String& line);

// mining.submit for a solved nonce; submitId receives the rpc id used, so the
// later SUCCESS/error frame can be matched back to this share.
bool stratumSubmit(WiFiClient& c, const char* user, const StratumJob& job,
                   const String& extranonce2, uint32_t nonce,
                   unsigned long& submitId);

#endif  // WITH_MINER
