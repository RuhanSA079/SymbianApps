#!/usr/bin/env python3
"""Log in to Google Drive once on this PC and produce a token file for rDrive.

Symbian's browser cannot do a modern Google sign-in, so rDrive gets its
long-lived refresh token from here: OAuth 2.0 for desktop apps (loopback
redirect to 127.0.0.1 + PKCE), using YOUR OWN Google Cloud OAuth client.

One-time setup in https://console.cloud.google.com/ :
  1. Create a project, enable the "Google Drive API".
  2. OAuth consent screen: External; add yourself as a test user.
     (In "Testing" status Google expires Drive refresh tokens after 7 days;
     set it to "In production" to avoid that - you will then see an
     "unverified app" warning at login, which you can accept for your own app.)
  3. Credentials -> Create OAuth client ID -> "Desktop app"; download the JSON.

Then:
  env/rdrive-auth.py path/to/client_secret_XXXX.json [--emulator]

Writes keys/rdrive-token.json (git-ignored, mode 600). Copy it to the phone
as E:\\rDrive\\rdrive-token.json (or C:\\Data\\rDrive\\); rDrive imports it into
its private folder and deletes the public copy. --emulator copies it into the
EKA2L1 emulator's C:\\Data\\rDrive\\ directly.

Standard library only. Runs on the HOST.
"""
import base64, hashlib, http.server, json, os, secrets, sys, threading, time
import urllib.error, urllib.parse, urllib.request, webbrowser

AUTH_URL = 'https://accounts.google.com/o/oauth2/v2/auth'
TOKEN_URL = 'https://oauth2.googleapis.com/token'
SCOPE = 'https://www.googleapis.com/auth/drive'

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'keys', 'rdrive-token.json')
EMU_DIR = os.path.join(ROOT, 'emu-data', 'EKA2L1', 'data', 'drives', 'c', 'Data', 'rDrive')


def load_client(path):
    data = json.load(open(path))
    info = data.get('installed') or data.get('web')
    if not info or 'client_id' not in info:
        sys.exit('not a Google OAuth client JSON (expected an "installed" section)')
    if 'installed' not in data:
        sys.exit('this is a "Web application" client; create a "Desktop app" client instead')
    return info['client_id'], info.get('client_secret', '')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    client_id, client_secret = load_client(args[0])

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()).rstrip(b'=').decode()
    state = secrets.token_urlsafe(16)
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if q.get('state', [''])[0] != state:
                self.send_response(400)
                self.end_headers()
                return
            result['code'] = q.get('code', [None])[0]
            result['error'] = q.get('error', [None])[0]
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.end_headers()
            self.wfile.write(b'rDrive: you can close this tab and return to the terminal.\n')

        def log_message(self, *a):
            pass

    server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
    redirect = 'http://127.0.0.1:%d' % server.server_port
    threading.Thread(target=server.handle_request, daemon=True).start()

    url = AUTH_URL + '?' + urllib.parse.urlencode({
        'client_id': client_id, 'redirect_uri': redirect, 'response_type': 'code',
        'scope': SCOPE, 'access_type': 'offline', 'prompt': 'consent',
        'state': state, 'code_challenge': challenge, 'code_challenge_method': 'S256',
    })
    print('Opening your browser to sign in to Google. If it does not open, visit:\n\n%s\n' % url)
    webbrowser.open(url)

    deadline = time.time() + 300
    while 'code' not in result and 'error' not in result and time.time() < deadline:
        time.sleep(0.2)
    if result.get('error') or not result.get('code'):
        sys.exit('login failed or timed out: %s' % (result.get('error') or 'no response'))

    body = urllib.parse.urlencode({
        'code': result['code'], 'client_id': client_id, 'client_secret': client_secret,
        'redirect_uri': redirect, 'grant_type': 'authorization_code',
        'code_verifier': verifier,
    }).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(TOKEN_URL, data=body), timeout=30) as r:
            tok = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit('token exchange failed: %s %s' % (e.code, e.read().decode(errors='replace')))
    if 'refresh_token' not in tok:
        sys.exit('Google returned no refresh token: %s' % tok)

    token = {
        'type': 'rdrive-token',
        'client_id': client_id,
        'client_secret': client_secret,
        'refresh_token': tok['refresh_token'],
        'token_uri': TOKEN_URL,
        'scope': tok.get('scope', SCOPE),
        'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fd = os.open(OUT, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(token, f, indent=1)
    print('Saved %s' % OUT)
    print('Copy it to the phone as E:\\rDrive\\rdrive-token.json (or C:\\Data\\rDrive\\).')

    if '--emulator' in sys.argv:
        os.makedirs(EMU_DIR, exist_ok=True)
        dest = os.path.join(EMU_DIR, 'rdrive-token.json')
        with open(dest, 'w') as f:
            json.dump(token, f, indent=1)
        os.chmod(dest, 0o600)
        print('Copied into the emulator: %s' % dest)


if __name__ == '__main__':
    main()
