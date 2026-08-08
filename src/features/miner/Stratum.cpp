#include "config.h"
#if WITH_MINER

#include "Stratum.h"
#include "MinerJob.h"
#include <ArduinoJson.h>

// rpc ids are per-connection; subscribe (always the first tx) resets to 1.
static unsigned long s_id = 1;
static unsigned long nextId() {
  if (s_id == ULONG_MAX) s_id = 1;
  return ++s_id;
}

// Most recent pool error text, for the UI. Written and read only by the stratum
// task, which copies it into the stats snapshot under the engine lock.
static char s_lastError[64] = "";

const char* stratumLastError() { return s_lastError; }
void        stratumClearError() { s_lastError[0] = 0; }

// A result frame with a non-empty "error" array (logged, treated as failure).
static bool frameHasError(JsonDocument& doc) {
  if (doc["error"].isNull() || doc["error"].size() == 0) return false;
  const char* msg = doc["error"][1] | "?";
  Serial.printf("[miner] pool error %d: %s\n", (int)doc["error"][0], msg);
  strlcpy(s_lastError, msg, sizeof(s_lastError));
  return true;
}

bool stratumSubscribe(WiFiClient& c, StratumSub& sub) {
  char payload[192];
  s_id = 1;
  snprintf(payload, sizeof(payload),
           "{\"id\":%lu,\"method\":\"mining.subscribe\",\"params\":[\"%s/%s\"]}\n",
           s_id, FW_NAME, FW_VERSION);
  Serial.printf("[miner] > %s", payload);
  c.print(payload);
  vTaskDelay(200 / portTICK_PERIOD_MS);

  String line = c.readStringUntil('\n');
  line.trim();
  if (!line.length()) return false;
  Serial.printf("[miner] < %s\n", line.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, line) || frameHasError(doc)) return false;
  if (doc["result"].isNull()) return false;

  sub.extranonce1 = (const char*)(doc["result"][1] | "");
  sub.extranonce2Size = doc["result"][2] | 0;
  return sub.extranonce1.length() > 0 && sub.extranonce2Size > 0;
}

bool stratumAuthorize(WiFiClient& c, const char* user, const char* pass,
                      unsigned long& authId) {
  char payload[256];
  authId = nextId();
  snprintf(payload, sizeof(payload),
           "{\"params\":[\"%s\",\"%s\"],\"id\":%lu,\"method\":\"mining.authorize\"}\n",
           user, pass, authId);
  Serial.printf("[miner] > %s", payload);
  // The reply is read from the main dispatch loop: notifications may already
  // be interleaved by the time it arrives. The caller matches it by authId.
  return c.print(payload) > 0;
}

bool stratumSuggestDifficulty(WiFiClient& c, double difficulty) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"id\":%lu,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n",
           nextId(), difficulty);
  return c.print(payload) > 0;
}

StratumMethod stratumParseMethod(const String& line) {
  if (!line.length()) return STRATUM_PARSE_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, line)) return STRATUM_PARSE_ERROR;
  if (frameHasError(doc)) return STRATUM_PARSE_ERROR;

  if (doc["method"].isNull())
    return doc["error"].isNull() ? STRATUM_SUCCESS : STRATUM_UNKNOWN;

  const char* m = doc["method"];
  if (!strcmp(m, "mining.notify"))         return STRATUM_NOTIFY;
  if (!strcmp(m, "mining.set_difficulty")) return STRATUM_SET_DIFFICULTY;
  return STRATUM_UNKNOWN;
}

bool stratumParseNotify(const String& line, StratumJob& job) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;
  JsonArrayConst p = doc["params"];
  if (p.isNull() || p.size() < 9) return false;

  job.jobId    = (const char*)(p[0] | "");
  job.prevHash = (const char*)(p[1] | "");
  job.coinb1   = (const char*)(p[2] | "");
  job.coinb2   = (const char*)(p[3] | "");
  job.version  = (const char*)(p[5] | "");
  job.nbits    = (const char*)(p[6] | "");
  job.ntime    = (const char*)(p[7] | "");
  job.cleanJobs = p[8] | false;

  job.merkleCount = 0;
  for (JsonVariantConst b : p[4].as<JsonArrayConst>()) {
    const char* hex = b | "";
    if (strlen(hex) != 64 || job.merkleCount >= STRATUM_MAX_MERKLE) return false;
    minerHexToBytes(hex, 64, job.merkle[job.merkleCount++]);
  }

  return job.jobId.length() && job.prevHash.length() == 64 &&
         job.version.length() == 8 && job.nbits.length() == 8 &&
         job.ntime.length() == 8;
}

bool stratumParseSetDifficulty(const String& line, double& difficulty) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;
  if (doc["params"][0].isNull()) return false;
  difficulty = doc["params"][0];
  return true;
}

unsigned long stratumExtractId(const String& line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return 0;
  return doc["id"] | 0UL;
}

bool stratumSubmit(WiFiClient& c, const char* user, const StratumJob& job,
                   const String& extranonce2, uint32_t nonce,
                   unsigned long& submitId) {
  char payload[384];
  submitId = nextId();
  snprintf(payload, sizeof(payload),
           "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08lx\"]}\n",
           submitId, user, job.jobId.c_str(), extranonce2.c_str(),
           job.ntime.c_str(), (unsigned long)nonce);
  Serial.printf("[miner] > %s", payload);
  return c.print(payload) > 0;
}

#endif  // WITH_MINER
