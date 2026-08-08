#!/usr/bin/env python3
"""One-time Spotify authorisation for the cube.

Spotify only accepts HTTPS redirect URIs or loopback (http://127.0.0.1), and the
cube can offer neither, so the browser half of OAuth has to happen on a real
computer. This runs a throwaway loopback server, opens the consent page, and
prints the refresh token to paste into the device's web UI. The device then
renews its own access tokens forever without you being involved.

Setup, once:
  1. https://developer.spotify.com/dashboard -> Create app
  2. Redirect URI: http://127.0.0.1:8888/callback   (must match exactly)
  3. Copy the Client ID and Client Secret
  4. python3 tools/spotify_auth.py --id <CLIENT_ID> --secret <CLIENT_SECRET>

Only "user-read-currently-playing" is requested — enough to read what is
playing, and nothing that can change playback or touch your library.
"""
import argparse
import base64
import http.server
import json
import secrets
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser

PORT = 8888
REDIRECT = f"http://127.0.0.1:{PORT}/callback"
SCOPE = "user-read-currently-playing"

_result = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_response(404)
            self.end_headers()
            return
        params = urllib.parse.parse_qs(parsed.query)
        _result["code"] = params.get("code", [None])[0]
        _result["state"] = params.get("state", [None])[0]
        _result["error"] = params.get("error", [None])[0]

        body = (b"<h2>Done.</h2><p>Back to the terminal.</p>"
                if _result["code"] else
                b"<h2>Authorisation failed.</h2><p>Check the terminal.</p>")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        threading.Thread(target=self.server.shutdown, daemon=True).start()

    def log_message(self, *args):
        pass          # keep the console clean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--id", required=True, help="Spotify app Client ID")
    ap.add_argument("--secret", required=True, help="Spotify app Client Secret")
    args = ap.parse_args()

    state = secrets.token_urlsafe(16)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": args.id,
        "response_type": "code",
        "redirect_uri": REDIRECT,
        "scope": SCOPE,
        "state": state,
    })

    print(f"Redirect URI this expects: {REDIRECT}")
    print("Opening your browser. Approve the request there.\n")
    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    webbrowser.open(auth_url)
    print(f"If nothing opened, visit:\n{auth_url}\n")
    server.serve_forever()

    if _result.get("error"):
        sys.exit(f"Spotify returned an error: {_result['error']}")
    if not _result.get("code"):
        sys.exit("No authorisation code came back.")
    if _result.get("state") != state:
        sys.exit("State mismatch — abandoning rather than trusting this response.")

    basic = base64.b64encode(f"{args.id}:{args.secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": _result["code"],
            "redirect_uri": REDIRECT,
        }).encode(),
        headers={
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )
    with urllib.request.urlopen(req) as resp:
        payload = json.load(resp)

    token = payload.get("refresh_token")
    if not token:
        sys.exit(f"No refresh token in the response: {payload}")

    print("=" * 66)
    print("Paste these into the cube's web UI, Spotify tab:\n")
    print(f"  Client ID:     {args.id}")
    print(f"  Client secret: {args.secret}")
    print(f"  Refresh token: {token}")
    print("\n" + "=" * 66)
    print("The refresh token does not expire. Treat it like a password: it can\n"
          "read what you are playing until you revoke the app at\n"
          "https://www.spotify.com/account/apps/")


if __name__ == "__main__":
    main()
