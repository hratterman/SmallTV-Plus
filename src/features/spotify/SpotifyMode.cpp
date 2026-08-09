#include "SpotifyMode.h"
#if WITH_SPOTIFY

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "AlbumArt.h"
#include "WorkMask.h"

SpotifyMode g_spotifyMode;

// What to put on the glass for a track, once work mode has had its say.
// Applied at render time rather than at fetch time on purpose: the poll should
// keep the real values so turning work mode off shows the right thing
// immediately, without waiting for the next poll.
static void workText(const Settings& s, const SpotifyData& d,
                     char* title, size_t titleN, char* artist, size_t artistN) {
  strlcpy(title, d.track, titleN);
  strlcpy(artist, d.artist, artistN);
  if (!s.work.enabled) return;
  if (s.work.hideExplicit && d.explicitTrack) {
    // The cover, the artist and the progress all stay — enough to know what is
    // playing without the title announcing itself across an open-plan office.
    strlcpy(title, "(explicit track)", titleN);
  }
  workMaskWords(title, s.work.blocklist.c_str());
  workMaskWords(artist, s.work.blocklist.c_str());
}

#define C_SPOT  0x1E6C   // Spotify green #1db954
#define C_DIM   0xB574
#define C_PANEL 0x18E3

// The cover takes the top of the screen and everything else stacks under it.
// The 58 px left under a 152 px cover has to carry a title of up to two lines,
// the artist, the two time labels and the bar; the old spacing did not add up,
// so the artist sat underneath the progress bar. Laid out explicitly now, with
// the arithmetic checked at compile time below.
static const int ART_X = (TFT_WIDTH - SPOTIFY_ART_PX) / 2;
static const int ART_Y = 26;

static const int TITLE_Y  = 186;   // one line, size 2, scrolled if it overruns
static const int ARTIST_Y = 208;   // one line, size 1, same
static const int TEXT_X   = 6;     // side margin the scrolling bands live in
static const int TEXT_W   = TFT_WIDTH - 2 * TEXT_X;
static const int BAR_X = 18;
static const int BAR_W = TFT_WIDTH - 36;
static const int BAR_H = 6;
static const int TIME_Y = 219;
static const int BAR_Y  = 231;

static_assert(ART_Y + SPOTIFY_ART_PX <= TITLE_Y, "cover overlaps the title");
static_assert(TITLE_Y + 16 <= ARTIST_Y, "the title band overlaps the artist");
static_assert(ARTIST_Y + 8 <= TIME_Y, "artist overlaps the time labels");
static_assert(TIME_Y + 8 <= BAR_Y, "time labels overlap the bar");
static_assert(BAR_Y + BAR_H <= TFT_HEIGHT, "bar runs off the bottom");

void SpotifyMode::begin(const Settings& s) {
  spotifyInit(s);
  invalidate(s);
}

void SpotifyMode::invalidate(const Settings& s) {
  spotifyInit(s);
  spotifyForceRefresh();
  needFull_ = true;
  drawnTrack_[0] = drawnArtist_[0] = drawnArt_[0] = 0;
  artOnGlass_ = false;
  barW_ = elapsed_ = -1;
}

void SpotifyMode::onContextAction(Settings& s) {
  spotifyForceRefresh();     // long-press: poll now instead of waiting
}

static void fmtTime(uint32_t ms, char* out, size_t n) {
  const uint32_t sec = ms / 1000;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

// The two scrolling bands. A title that does not fit used to wrap to a second
// line and then end in an ellipsis, which silently drops the rest of the name;
// these scroll instead, so everything is eventually readable.
static const GfxMarquee kTitleBand  = {TEXT_X, TITLE_Y,  TEXT_W, 2, C_WHITE, C_BLACK};
static const GfxMarquee kArtistBand = {TEXT_X, ARTIST_Y, TEXT_W, 1, C_DIM,   C_BLACK};

void SpotifyMode::renderAll(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const SpotifyData& d = spotifyGet();

  // The cover is the one thing on this screen that costs a network round trip,
  // so it is worth not destroying. When the same cover is already on the glass,
  // clear around the art box instead of over it and leave the picture alone.
  //
  // The two obvious versions of this are both wrong. Clearing the whole screen
  // and only re-fetching on a URL change leaves a black square on every repaint
  // of the same track. Clearing the whole screen and always re-fetching turns
  // every repaint into a blocking download on the render thread. Preserving the
  // box gives the first version's cost and the second version's correctness.
  const bool showingTrack = s.spotify.refreshToken.length() && !d.error &&
                            d.valid && d.playing;
  const bool keepArt = showingTrack && artOnGlass_ && d.artUrl[0] &&
                       strcmp(d.artUrl, drawnArt_) == 0;
  if (keepArt) {
    gfx->fillRect(0, 0, TFT_WIDTH, ART_Y, C_BLACK);                       // above
    gfx->fillRect(0, ART_Y, ART_X, SPOTIFY_ART_PX, C_BLACK);              // left
    gfx->fillRect(ART_X + SPOTIFY_ART_PX, ART_Y,
                  TFT_WIDTH - ART_X - SPOTIFY_ART_PX, SPOTIFY_ART_PX, C_BLACK);
    gfx->fillRect(0, ART_Y + SPOTIFY_ART_PX, TFT_WIDTH,
                  TFT_HEIGHT - ART_Y - SPOTIFY_ART_PX, C_BLACK);           // below
  } else {
    gfx->fillScreen(C_BLACK);
    artOnGlass_ = false;
  }

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
  // screen is never blank while the download runs. Skipped entirely when the
  // right cover is already sitting in the box untouched.
  if (!keepArt) {
    if (d.artUrl[0]) {
      artFailed_ = !albumArtDraw(d.artUrl, ART_X, ART_Y);
      strlcpy(drawnArt_, d.artUrl, sizeof(drawnArt_));
      artOnGlass_ = !artFailed_;
    } else {
      artFailed_ = true;
      drawnArt_[0] = 0;
    }
    if (artFailed_) {
      // A cover that would not load leaves a plate rather than a hole, so the
      // layout below it does not move about between tracks — and the plate says
      // why, because "no art" on its own has never once been enough to act on.
      gfx->fillRoundRect(ART_X, ART_Y, SPOTIFY_ART_PX, SPOTIFY_ART_PX, 6, C_PANEL);
      gfx->fillCircle(TFT_WIDTH / 2, ART_Y + SPOTIFY_ART_PX / 2, 16, C_SPOT);
      const char* why = d.artUrl[0] ? albumArtStatus() : "poll sent no cover url";
      gfxDrawCentered(why, ART_Y + SPOTIFY_ART_PX - 24, 1, C_DIM);
      char n[24];
      snprintf(n, sizeof(n), "%u covers offered", (unsigned)spotifyArtCandidates());
      gfxDrawCentered(n, ART_Y + SPOTIFY_ART_PX - 14, 1, C_DIM);
    }
  }

  char title[SPOTIFY_TRACK_LEN], artist[SPOTIFY_ARTIST_LEN];
  workText(s, d, title, sizeof(title), artist, sizeof(artist));
  scrolling_ = gfxMarqueeDraw(kTitleBand, title, millis());
  scrolling_ |= gfxMarqueeDraw(kArtistBand, artist, millis());

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
      strncmp(d.artist, drawnArtist_, sizeof(drawnArtist_)) != 0 ||
      d.explicitTrack != drawnExplicit_;

  if (contentChanged) {
    needFull_ = false;
    drawnLinked_  = linked;
    drawnValid_   = d.valid;
    drawnError_   = d.error;
    drawnPlaying_ = d.playing;
    strlcpy(drawnTrack_, d.track, sizeof(drawnTrack_));
    strlcpy(drawnArtist_, d.artist, sizeof(drawnArtist_));
    drawnExplicit_ = d.explicitTrack;
    lastTick_ = now;
    renderAll(s);
    return;
  }

  if (d.playing && (now - lastTick_) >= 500) {
    lastTick_ = now;
    renderProgress(s);
  }

  // Advance the scrolling bands. Only when something actually overruns, and
  // only on their own cadence: at 25 fps two short bands cost a few KB a second
  // over the bus, and nothing at all when both titles fit.
  if (d.playing && scrolling_ && (now - scrollTick_) >= 40) {
    scrollTick_ = now;
    char title[SPOTIFY_TRACK_LEN], artist[SPOTIFY_ARTIST_LEN];
    workText(s, d, title, sizeof(title), artist, sizeof(artist));
    gfxMarqueeDraw(kTitleBand, title, now);
    gfxMarqueeDraw(kArtistBand, artist, now);
  }
}

#endif  // WITH_SPOTIFY
