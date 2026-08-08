#include "SpotifyMode.h"
#if WITH_SPOTIFY

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "SpotifyClient.h"

SpotifyMode g_spotifyMode;

#define C_SPOT  0x1E6C   // Spotify green #1db954
#define C_DIM   0xB574
#define C_PANEL 0x18E3

void SpotifyMode::begin(const Settings& s) {
  spotifyInit(s);
  needFull_ = true;
}

void SpotifyMode::invalidate(const Settings& s) {
  spotifyInit(s);
  spotifyForceRefresh();
  needFull_ = true;
}

void SpotifyMode::onContextAction(Settings& s) {
  spotifyForceRefresh();     // long-press: poll now instead of waiting
  needFull_ = true;
}

static void fmtTime(uint32_t ms, char* out, size_t n) {
  const uint32_t sec = ms / 1000;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

// Two lines of track title, wrapped on a word where possible.
static void drawTitle(Arduino_GFX* gfx, const char* text, int y, uint8_t size) {
  const int perLine = (TFT_WIDTH - 16) / (6 * size);
  char line[40];
  const char* p = text;
  for (int row = 0; row < 2 && *p; row++) {
    while (*p == ' ') p++;
    if (!*p) break;
    int take = 0, lastSpace = -1;
    while (p[take] && take < perLine) {
      if (p[take] == ' ') lastSpace = take;
      take++;
    }
    if (p[take] && lastSpace > 0) take = lastSpace;
    if (take > (int)sizeof(line) - 1) take = sizeof(line) - 1;
    memcpy(line, p, take);
    line[take] = 0;
    // A second row that still has text left gets an ellipsis rather than a cut.
    if (row == 1 && p[take]) {
      int L = strlen(line);
      if (L > 3) strcpy(line + L - 3, "...");
    }
    gfxDrawCentered(line, y + row * (8 * size + 4), size, C_WHITE);
    p += take;
  }
}

void SpotifyMode::render(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const SpotifyData& d = spotifyGet();

  gfx->fillScreen(C_BLACK);

  // Header: the wordmark dot plus state.
  gfx->fillCircle(22, 22, 9, C_SPOT);
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(40, 18);
  gfx->print(d.playing ? "NOW PLAYING" : "SPOTIFY");

  if (!s.spotify.refreshToken.length()) {
    gfxDrawCentered("not linked", 100, 2, C_WHITE);
    gfxDrawCentered("run tools/spotify_auth.py", 130, 1, C_DIM);
    gfxDrawCentered("then paste the token in the web UI", 148, 1, C_DGRAY);
    return;
  }
  if (d.error) {
    gfxDrawCentered("Spotify error", 100, 2, C_RED);
    gfxDrawCentered(d.errorMsg, 130, 1, C_DIM);
    return;
  }
  if (!d.valid) {
    gfxDrawCentered("connecting...", 110, 2, C_DIM);
    return;
  }
  if (!d.playing) {
    gfxDrawCentered("nothing playing", 106, 2, C_DIM);
    return;
  }

  // Track, then artist.
  const uint8_t tsz = (strlen(d.track) > 22) ? 2 : 3;
  drawTitle(gfx, d.track, 62, tsz);
  gfxDrawCentered(d.artist, 132, 2, C_DIM);

  // Progress bar with elapsed / total.
  const uint32_t pos = spotifyProgressNow();
  const int bx = 18, by = 178, bw = TFT_WIDTH - 36, bh = 8;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_PANEL);
  if (d.durationMs) {
    int fw = (int)((uint64_t)bw * pos / d.durationMs);
    if (fw > bw) fw = bw;
    if (fw >= bh)     gfx->fillRoundRect(bx, by, fw, bh, bh / 2, C_SPOT);
    else if (fw > 0)  gfx->fillRect(bx, by, fw, bh, C_SPOT);
  }

  char a[12], b[12];
  fmtTime(pos, a, sizeof(a));
  fmtTime(d.durationMs, b, sizeof(b));
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(bx, by + 14);
  gfx->print(a);
  gfx->setCursor(bx + bw - gfxTextW(b, 1), by + 14);
  gfx->print(b);
}

void SpotifyMode::service(const Settings& s) {
  spotifyService(s);

  const SpotifyData& d = spotifyGet();
  const uint32_t now = millis();

  // Repaint when the track changes, and once a second while playing so the
  // progress bar moves; otherwise leave the panel alone.
  bool changed = needFull_ || d.lastOkMs != seenOk_;
  if (!changed && d.playing && (now - lastDraw_) >= 1000) changed = true;
  if (!changed) return;

  needFull_ = false;
  seenOk_ = d.lastOkMs;
  lastDraw_ = now;
  render(s);
}

#endif  // WITH_SPOTIFY
