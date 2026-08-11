# Putting the SmallTV tether on henryratterman.com

*A guide for standing up the tether page — no server code, no build step, five
static files.*

## What this is

The tether page lets the SmallTV cube borrow a computer's internet through its
USB cable, and doubles as the cube's control panel: settings, weather and
calendar setup, firmware updates — all of it happens in this one page. There
is no backend. Hosting it means copying five files to the web server and
nothing else.

## The files

| File | What it is | Required? |
|---|---|---|
| `tether.html` | The entire app | **Yes** |
| `tether-sw.js` | Lets the page install as an app and open offline | Optional |
| `tether.webmanifest` | The app's name and icon for installing | Optional |
| `tether-icon-192.png` | App icon | Optional |
| `tether-icon-512.png` | App icon | Optional |

`tether.html` alone is a fully working page. The other four add the
"Install as app" button; without them nothing else changes.

## Where to put them

- All five files go in the **same folder**. Any path works —
  `henryratterman.com/tether/` is a fine choice.
- **Pick the final URL once and keep it.** If the Google Calendar linking is
  ever used, the page's exact URL becomes part of the Google OAuth
  registration; moving the page afterwards breaks that link setup until the
  registration is updated.

## What the hosting must do

Most ordinary hosting already does all of this. Checking takes one minute
(see "Verifying" below).

1. **HTTPS.** The browser features the page depends on (Web Serial, the
   install-as-app machinery) only work on `https://` pages. Any normal host
   provides this.
2. **Serve the files byte-for-byte.** The page's code is inline in the HTML.
   Anything that rewrites or "optimizes" scripts can silently kill it — if
   the site sits behind Cloudflare, leave *Rocket Loader* and *Auto Minify*
   off for this path; skip any build pipeline entirely. Just copy the files.
3. **No blocking headers.** Two site-wide security headers, *if the site sends
   them*, need a carve-out:
   - `Content-Security-Policy`: its `script-src` must include
     `'unsafe-inline'` for this path (or exempt the path).
   - `Permissions-Policy`: must not disable `serial`.
   A site that doesn't send these headers (most don't) is already fine.
4. **Never put the page in an iframe.** Link to it as its own page.
5. **One MIME nicety** (rarely needed): if the "Install as app" button never
   appears, the server may be serving `.webmanifest` with the wrong type —
   it should be `application/manifest+json`. GitHub Pages, Netlify and
   friends already do this.

## Verifying — no cube needed

Open the page in **Chrome or Edge** and press **Connect**.

- **The browser's device-picker dialog appears** (an empty list is fine) →
  the hosting is correct. Done.
- **Nothing happens, or a "no Web Serial" notice shows** → the page is
  either not on HTTPS, being script-rewritten, or blocked by one of the
  headers above.

For the app install: after loading the page once or twice, an
**Install as app** button appears in the header (Chrome/Edge decide when to
offer it). DevTools → Application → Manifest will show any complaint.

## Day-to-day use (the two-minute version)

- Use **Chrome, Edge or Opera on a desktop or laptop** — Safari and Firefox
  don't have Web Serial.
- Plug the cube into the computer with its USB cable, open the page, press
  **Connect**, pick the USB entry (it may be named "CP2102", "CH340" or
  "USB Serial" — that's the cube).
- The cube is online for as long as the page is open. Best setup: click
  **Install as app**, then add the installed app to the computer's login
  items — the cube then comes online whenever the computer does.
- Everything about the cube is managed from this page: what it shows, the
  weather location (type a city, pick a match), calendars, and firmware
  updates (drop the new `firmware.bin` on the Firmware card — about three
  minutes, and a pulled cable just means starting over, never a broken cube).

## If something doesn't work

| Symptom | Likely cause |
|---|---|
| Connect button does nothing | Not Chrome/Edge/Opera, or the page isn't on HTTPS |
| Device picker opens but is empty | The USB cable is power-only (try another), or the cube isn't plugged in |
| Connects but "waiting for the cube" forever | Wrong entry picked — Disconnect and pick the other one |
| A mode on the cube shows a red error | Open the page's **Network health** card and run the checks — red rows mean the computer's network is blocking that service, not the cube |

## What this hosting does *not* involve

No server-side code, no database, no API keys, no accounts, no renewals, no
maintenance. Updating the page ever = overwriting the files with newer ones.
