#!/usr/bin/env python3
"""One-time calendar authorisation for the cube.

Prints a refresh token to paste into the device's web UI (Calendar tab). After
that the cube renews its own access tokens forever without you involved — over
WiFi or over the tether; both providers' token endpoints answer a browser.

Google (needs a client secret — its Desktop clients are issued one):
  1. console.cloud.google.com -> APIs & Services -> Credentials
     -> Create OAuth client ID, type "Desktop app"
  2. Enable the "Google Calendar API" for the project
  3. python3 tools/calendar_auth.py google --id <CLIENT_ID> --secret <SECRET>
     (a browser opens; approve; the token prints here)

Microsoft (no secret; device-code flow, nothing listens locally):
  1. portal.azure.com -> App registrations -> New
     -> Supported accounts: personal + org, no redirect URI needed
  2. Authentication -> "Allow public client flows" -> Yes
  3. python3 tools/calendar_auth.py microsoft --id <CLIENT_ID>
     (visit the printed URL, enter the printed code; the token prints here)

Only read scopes are requested: calendar.readonly / Calendars.Read. Revoke at
myaccount.google.com/permissions or account.live.com/consent/Manage.
"""
import argparse
import http.server
import json
import secrets
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser

PORT = 8890
REDIRECT = f"http://127.0.0.1:{PORT}/callback"

G_AUTH = "https://accounts.google.com/o/oauth2/v2/auth"
G_TOKEN = "https://oauth2.googleapis.com/token"
G_SCOPE = "https://www.googleapis.com/auth/calendar.readonly"

MS_DEVICE = "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode"
MS_TOKEN = "https://login.microsoftonline.com/common/oauth2/v2.0/token"
MS_SCOPE = "offline_access Calendars.Read"

_result = {}


def post_form(url, fields):
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    try:
        with urllib.request.urlopen(req) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return json.load(e)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_error(404)
            return
        q = urllib.parse.parse_qs(parsed.query)
        _result.update({k: v[0] for k, v in q.items()})
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(b"<h2>Done - go back to the terminal.</h2>")

    def log_message(self, *a):
        pass


def google(args):
    if not args.secret:
        sys.exit("Google needs --secret (Desktop clients are issued one).")
    state = secrets.token_urlsafe(16)
    url = G_AUTH + "?" + urllib.parse.urlencode({
        "client_id": args.id,
        "redirect_uri": REDIRECT,
        "response_type": "code",
        "scope": G_SCOPE,
        # Both matter: without them Google omits the refresh token for any
        # client it has approved before, which looks like a bug here but is a
        # re-consent it never asked for.
        "access_type": "offline",
        "prompt": "consent",
        "state": state,
    })
    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("Opening the consent page (or open this yourself):\n  " + url + "\n")
    webbrowser.open(url)
    while "code" not in _result and "error" not in _result:
        time.sleep(0.2)
    server.shutdown()
    if "error" in _result:
        sys.exit("Refused: " + _result["error"])
    if _result.get("state") != state:
        sys.exit("State mismatch - try again.")

    tok = post_form(G_TOKEN, {
        "client_id": args.id,
        "client_secret": args.secret,
        "code": _result["code"],
        "grant_type": "authorization_code",
        "redirect_uri": REDIRECT,
    })
    if "refresh_token" not in tok:
        sys.exit("No refresh token in the reply: " + json.dumps(tok, indent=1))
    done(tok["refresh_token"])


def microsoft(args):
    dev = post_form(MS_DEVICE, {"client_id": args.id, "scope": MS_SCOPE})
    if "device_code" not in dev:
        sys.exit("Device-code request refused: " + json.dumps(dev, indent=1))
    print("\n  1. Open   " + dev["verification_uri"])
    print("  2. Enter  " + dev["user_code"] + "\n")
    print("Waiting for you to approve it...")
    interval = int(dev.get("interval", 5))
    deadline = time.time() + int(dev.get("expires_in", 900))
    while time.time() < deadline:
        time.sleep(interval)
        tok = post_form(MS_TOKEN, {
            "client_id": args.id,
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "device_code": dev["device_code"],
        })
        err = tok.get("error", "")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval += 5
            continue
        if err:
            sys.exit("Refused: " + err + " - " + tok.get("error_description", ""))
        if "refresh_token" not in tok:
            sys.exit("No refresh token in the reply: " + json.dumps(tok, indent=1))
        done(tok["refresh_token"])
        return
    sys.exit("The code expired before it was approved - run this again.")


def done(refresh_token):
    print("\n" + "=" * 64)
    print("Refresh token (paste into the web UI -> Calendar tab):\n")
    print(refresh_token)
    print("=" * 64)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("provider", choices=["google", "microsoft"])
    ap.add_argument("--id", required=True, help="OAuth client ID")
    ap.add_argument("--secret", help="OAuth client secret (Google only)")
    args = ap.parse_args()
    (google if args.provider == "google" else microsoft)(args)


if __name__ == "__main__":
    main()
