#!/usr/bin/env python3
"""
Antigravity / Google AI Quota OAuth Token Helper
Retrieves, verifies, and optionally uploads the OAuth Refresh Token to the CYD ESP32 Console Clock.
"""

import os
import sys
import json
import urllib.request
import urllib.parse
import http.server
import socketserver
import webbrowser
import threading

USER_AGENT = "antigravity/cli/1.4.2 (aidev_client; os_type=linux; arch=amd64; cl=1001591392; auth_method=consumer)"
QUOTA_ENDPOINT = "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary"
CLIENT_SECRET_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client_secret.json")

def load_client_credentials():
    """Load OAuth client_id and client_secret from client_secret.json in the same directory."""
    if not os.path.exists(CLIENT_SECRET_FILE):
        print(f"[!] Error: '{CLIENT_SECRET_FILE}' not found.")
        print("    Please place 'client_secret.json' in the tools/ directory.")
        sys.exit(1)
    try:
        with open(CLIENT_SECRET_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        section = data.get("installed") or data.get("web") or data
        client_id = section.get("client_id")
        client_secret = section.get("client_secret")
        if not client_id or not client_secret:
            raise ValueError("Missing 'client_id' or 'client_secret' in JSON")
        return client_id, client_secret
    except Exception as e:
        print(f"[!] Failed to load credentials from '{CLIENT_SECRET_FILE}': {e}")
        sys.exit(1)

CLIENT_ID, CLIENT_SECRET = load_client_credentials()

def try_read_keyring_token():
    """Attempt to retrieve existing Antigravity CLI token from OS secret service."""
    try:
        import dbus
        bus = dbus.SessionBus()
        service = bus.get_object("org.freedesktop.secrets", "/org/freedesktop/secrets")
        s_iface = dbus.Interface(service, "org.freedesktop.Secret.Service")
        output, session = s_iface.OpenSession("plain", dbus.String("", variant_level=1))
        results = s_iface.SearchItems({"service": "gemini", "username": "antigravity"})
        if results and len(results[0]) > 0:
            item = bus.get_object("org.freedesktop.secrets", results[0][0])
            secret = dbus.Interface(item, "org.freedesktop.Secret.Item").GetSecret(session)
            data = json.loads(bytes(secret[2]).decode())
            tok = data.get("token", {})
            return tok.get("refresh_token")
    except Exception:
        pass
    return None

def verify_and_print_quota(refresh_token):
    """Verify refresh token and display live quota buckets."""
    print("\n[+] 1. Fetching Google OAuth Access Token...")
    body = urllib.parse.urlencode({
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "refresh_token": refresh_token,
        "grant_type": "refresh_token"
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req) as resp:
            token_res = json.loads(resp.read().decode())
            access_token = token_res["access_token"]
            print("    [OK] Access Token obtained successfully.")
    except Exception as e:
        print(f"    [!] Failed to refresh token: {e}")
        return False

    print("\n[+] 2. Querying Antigravity Quota API...")
    quota_req = urllib.request.Request(
        QUOTA_ENDPOINT,
        data=json.dumps({"project": "aicode-consumers"}).encode(),
        method="POST"
    )
    quota_req.add_header("Authorization", f"Bearer {access_token}")
    quota_req.add_header("Content-Type", "application/json")
    quota_req.add_header("User-Agent", USER_AGENT)

    try:
        with urllib.request.urlopen(quota_req) as resp:
            data = json.loads(resp.read().decode())
            print("    [OK] Quota data received successfully!\n")
            print("=" * 64)
            print("         Antigravity Live Quota Status")
            print("=" * 64)
            for grp in data.get("groups", []):
                grp_name = grp.get("displayName", "")
                for b in grp.get("buckets", []):
                    b_name = b.get("displayName", "")
                    frac = b.get("remainingFraction", 0.0)
                    pct = int(frac * 100)
                    bar = "#" * (pct // 5) + "-" * (20 - (pct // 5))
                    reset = b.get("resetTime", "")
                    print(f"[{grp_name}] {b_name}")
                    print(f"  [{bar}] {pct:>3}% (Reset: {reset})")
            print("=" * 64)
            return True
    except Exception as e:
        print(f"    [!] Failed to retrieve quota: {e}")
        return False

def interactive_browser_auth():
    """Launch local webserver and initiate browser OAuth authorization flow."""
    print("\nOpening browser for Google OAuth sign-in...")
    auth_code = None
    server_done = threading.Event()

    class OAuthHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            nonlocal auth_code
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            if "code" in qs:
                auth_code = qs["code"][0]
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"<h1>Authentication successful!</h1><p>You can close this tab now and return to your terminal.</p>")
            else:
                self.send_response(400)
                self.end_headers()
            server_done.set()

        def log_message(self, format, *args):
            pass

    server = socketserver.TCPServer(("127.0.0.1", 8085), OAuthHandler)
    t = threading.Thread(target=server.serve_forever)
    t.daemon = True
    t.start()

    params = {
        "client_id": CLIENT_ID,
        "redirect_uri": "http://localhost:8085/",
        "response_type": "code",
        "scope": "https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/cclog",
        "access_type": "offline",
        "prompt": "consent"
    }
    auth_url = "https://accounts.google.com/o/oauth2/v2/auth?" + urllib.parse.urlencode(params)
    print(f"\nIf browser does not open automatically, visit this URL:\n{auth_url}\n")
    webbrowser.open(auth_url)

    server_done.wait(timeout=120)
    server.shutdown()

    if not auth_code:
        print("[!] Authorization code was not received.")
        return None

    body = urllib.parse.urlencode({
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "code": auth_code,
        "grant_type": "authorization_code",
        "redirect_uri": "http://localhost:8085/"
    }).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    with urllib.request.urlopen(req) as resp:
        res = json.loads(resp.read().decode())
        return res.get("refresh_token")

def send_token_to_esp(token, esp_ip):
    """Optionally send refresh token and OAuth credentials directly to ESP32 Web Portal API."""
    url = f"http://{esp_ip}/save"
    post_data = urllib.parse.urlencode({
        "refr": token,
        "cid": CLIENT_ID,
        "sec": CLIENT_SECRET
    }).encode("utf-8")
    print(f"\n[+] Sending OAuth credentials (Token, Client ID, Secret) to ESP32 at {url}...")
    try:
        req = urllib.request.Request(url, data=post_data, headers={
            "Content-Type": "application/x-www-form-urlencoded"
        })
        with urllib.request.urlopen(req, timeout=8) as resp:
            if resp.status == 200:
                print("[SUCCESS] OAuth credentials successfully saved to ESP32!")
                print("The console dashboard will update its quota display momentarily.")
                return True
            else:
                print(f"[!] Server returned status {resp.status}")
    except Exception as e:
        print(f"[!] Could not reach ESP32 at {esp_ip}: {e}")
    return False

def main():
    print("=" * 64)
    print("   Antigravity / Google AI Pro OAuth Token Helper")
    print("=" * 64)

    token = try_read_keyring_token()
    if token:
        print("[+] Existing Antigravity CLI token detected automatically!")
    else:
        print("[*] No saved token found. Starting browser OAuth sign-in flow...")
        token = interactive_browser_auth()

    if not token:
        print("[!] Failed to obtain token.")
        sys.exit(1)

    print("\n" + "#" * 64)
    print("  Your OAuth Refresh Token:")
    print(f"  {token}")
    print("#" * 64 + "\n")

    verify_and_print_quota(token)

    print("\n[+] Next Steps:")
    print(" Option A (Browser Web Portal):")
    print("   1. Connect to Wi-Fi 'CYD-AgyClock-Setup' (or your local network IP).")
    print("   2. Open http://192.168.4.1/ in your browser.")
    print("   3. Paste the Refresh Token above, enter your Wi-Fi credentials, and click [Save and Apply].")
    print("\n Option B (Direct Send):")
    target_ip = input("   Enter ESP32 IP to send token directly (e.g. 192.168.4.1 or 192.168.0.x, or press Enter to skip): ").strip()
    if target_ip:
        send_token_to_esp(token, target_ip)

if __name__ == "__main__":
    main()
