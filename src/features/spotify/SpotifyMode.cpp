#include "SpotifyMode.h"
#if WITH_SPOTIFY

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "AlbumArt.h"

SpotifyMode g_spotifyMode;

#define C_SPOT  0x1E6C   // Spotify green #1db954
#define C_DIM   0xB574
#define C_PANEL 0x18E3

// The cover takes the top of the screen and everything else stacks under it.
static const int ART_X = (TFT_WIDTH - SPOTIFY_ART_PX) / 2;
static const int ART_Y = 30;

// Progress bar geometry, shared by the full paint and the incremental update.
static const int BAR_X = 18;
static const int BAR_Y = 222;
static const int BAR_W = TFT_WIDTH - 36;
static const int BAR_H = 6;
static const int TIME_Y = BAR_Y - 11;

void SpotifyMode::begin(const Settings& s) {
  spotifyInit(s);
  invalidate(s);
}

void SpotifyMode::invalidate(const Settings& s) {
  spotifyInit(s);
  spotifyForceRefresh();
  needFull_ = true;
  drawnTrack_[0] = drawnArtist_[0] = 0;
  drawnArt_[0] = 0;
  barW_ = elapsed_ = -1;
}

void SpotifyMode::onContextAction(Settings& s) {
  spotifyForceRefresh();     // long-press: poll now instead of waiting
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

void SpotifyMode::renderAll(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const SpotifyData& d = spotifyGet();

  gfx->fillScreen(C_BLACK);

  gfx->fillCircle(22, 16, 7, C_SPOT);
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(36, 12);
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

  // Cover first: it is the slow part, and drawing it before the text means the
  // screen is never blank while the download runs.
  if (d.artUrl[0]) {
    if (strcmp(d.artUrl, drawnArt_) != 0 || artFailed_) {
      artFailed_ = !albumArtDraw(d.artUrl, ART_X, ART_Y);
      strlcpy(drawnArt_, d.artUrl, sizeof(drawnArt_));
    }
    if (artFailed_) {
      // A cover that would not load leaves a plate rather than a hole, so the
      // layout below it does not move about between tracks.
      gfx->fillRoundRect(ART_X, ART_Y, SPOTIFY_ART_PX, SPOTIFY_ART_PX, 6, C_PANEL);
      gfx->fillCircle(TFT_WIDTH / 2, ART_Y + SPOTIFY_ART_PX / 2, 18, C_SPOT);
    }
  } else {
    drawnArt_[0] = 0;
    gfx->fillRoundRect(ART_X, ART_Y, SPOTIFY_ART_PX, SPOTIFY_ART_PX, 6, C_PANEL);
  }

  const int textY = ART_Y + SPOTIFY_ART_PX + 8;
  const uint8_t tsz = (strlen(d.track) > 26) ? 1 : 2;
  drawTitle(gfx, d.track, textY, tsz);
  gfxDrawCentered(d.artist, textY + (tsz == 1 ? 22 : 30), 1, C_DIM);

  // Bar track and total duration are static for this song; only the fill and
  // the elapsed label move from here on.
  gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_PANEL);
  char b[12];
  fmtTime(d.durationMs, b, sizeof(b));
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(BAR_X + BAR_W - gfxTextW(b, 1), TIME_Y);
  gfx->print(b);

  barW_ = 0;
  elapsed_ = -1;
  renderProgress(s);
}

// Called once a second while playing. Touches only the pixels that changed: the
// newly-filled slice of the bar, and the elapsed-time label. No screen clear, so
// nothing blinks.
void SpotifyMode::renderProgress(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const SpotifyData& d = spotifyGet();
  if (!d.valid || !d.playing || !d.durationMs) return;

  const uint32_t pos = spotifyProgressNow();

  int fw = (int)((uint64_t)BAR_W * pos / d.durationMs);
  if (fw > BAR_W) fw = BAR_W;
  if (fw != barW_) {
    if (fw > barW_ && barW_ >= 0) {
      // Extend the fill rather than redrawing it: a few pixels of new colour.
      const int from = (barW_ < BAR_H) ? 0 : barW_;
      if (fw >= BAR_H) {
        gfx->fillRoundRect(BAR_X, BAR_Y, fw, BAR_H, BAR_H / 2, C_SPOT);
      } else if (fw > 0) {
        gfx->fillRect(BAR_X + from, BAR_Y, fw - from, BAR_H, C_SPOT);
      }
    } else {
      // Jumped backwards (seek, or a new song): repaint the whole bar once.
      gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_PANEL);
      if (fw >= BAR_H)     gfx->fillRoundRect(BAR_X, BAR_Y, fw, BAR_H, BAR_H / 2, C_SPOT);
      else if (fw > 0)     gfx->fillRect(BAR_X, BAR_Y, fw, BAR_H, C_SPOT);
    }
    barW_ = fw;
  }

  const int sec = (int)(pos / 1000);
  if (sec != elapsed_) {
    elapsed_ = sec;
    char a[12];
    fmtTime(pos, a, sizeof(a));
    // Erase just this label's box; "88:88" at size 1 is the widest it gets.
    gfx->fillRect(BAR_X, TIME_Y, gfxTextW("88:88", 1) + 6, 8, C_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(C_DIM);
    gfx->setCursor(BAR_X, TIME_Y);
    gfx->print(a);
  }
}

void SpotifyMode::service(const Settings& s) {
  spotifyService(s);

  const SpotifyData& d = spotifyGet();
  const uint32_t now = millis();
  const bool linked = s.spotify.refreshToken.length() > 0;

  // A full repaint only when what the panel says has actually changed.
  const bool contentChanged =
      needFull_ ||
      linked != drawnLinked_ ||
      d.valid != drawnValid_ ||
      d.error != drawnError_ ||
      d.playing != drawnPlaying_ ||
      strncmp(d.track, drawnTrack_, sizeof(drawnTrack_)) != 0 ||
      strncmp(d.artist, drawnArtist_, sizeof(drawnArtist_)) != 0;

  if (contentChanged) {
    needFull_ = false;
    drawnLinked_  = linked;
    drawnValid_   = d.valid;
    drawnError_   = d.error;
    drawnPlaying_ = d.playing;
    strlcpy(drawnTrack_, d.track, sizeof(drawnTrack_));
    strlcpy(drawnArtist_, d.artist, sizeof(drawnArtist_));
    lastTick_ = now;
    renderAll(s);
    return;
  }

  if (d.playing && (now - lastTick_) >= 500) {
    lastTick_ = now;
    renderProgress(s);
  }
}

#endif  // WITH_SPOTIFY
