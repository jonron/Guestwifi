#!/usr/bin/env python3
"""Attrapp av UDM Pro Max (UniFi OS + Network-appen) för simulering.

Implementerar de anrop firmwaren använder, med samma beteende som riktig UniFi OS:
  POST /api/auth/login                                  -> Set-Cookie TOKEN, X-CSRF-Token
  GET  /proxy/network/api/s/default/rest/wlanconf       -> WLAN-lista (chunked, som UDM)
  PUT  /proxy/network/api/s/default/rest/wlanconf/<id>  -> kräver cookie + CSRF

Felinjicering styrs via POST /__sim/faults med JSON, t.ex.
  {"expire_session": true}     nästa anrop får 401 (sessionen har gått ut)
  {"drop_put_response": true}  nästa PUT tillämpas men anslutningen stängs utan svar
  {"apply_after_reads": 3}     nästa PUT syns först efter 3 GET (AP-omprovisionering)
  {"reject_put": true}         nästa PUT nekas med rc=error
  {"rotate_csrf": true}        varje PUT ger ny CSRF-token via X-Updated-CSRF-Token
GET /__sim/state visar aktuellt tillstånd.
"""
import argparse
import json
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WLAN_BASE = "/proxy/network/api/s/default/rest/wlanconf"


class Udm:
    def __init__(self, user, password):
        self.user, self.password = user, password
        self.lock = threading.Lock()
        self.sessions = {}  # token -> csrf
        self.wlans = [
            {"_id": "65a1f0c2e4b0a1b2c3d4e5f1", "name": "VidebergKraft_IOT",
             "security": "wpapsk", "wpa3_support": True, "x_passphrase": "iot-secret", "enabled": True},
            {"_id": "65a1f0c2e4b0a1b2c3d4e5f2", "name": "VidebergKraft_Guest",
             "security": "wpapsk", "is_guest": True, "x_passphrase": "Start-Pass-22", "enabled": True},
        ]
        self.faults = {}
        self.pending = None  # (id, passphrase, reads_left)
        self.log = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    udm: Udm = None

    def log_message(self, fmt, *args):
        pass

    # --- hjälpare -----------------------------------------------------------
    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _send(self, status, obj, headers=None, chunked=False):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json;charset=UTF-8")
        for k, v in (headers or []):
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(data), 97):  # flera chunkar
                part = data[i:i + 97]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        self.close_connection = True

    def _session(self):
        cookie = self.headers.get("Cookie", "")
        for part in cookie.split(";"):
            k, _, v = part.strip().partition("=")
            if k == "TOKEN" and v in self.udm.sessions:
                return v
        return None

    def _unauthorized(self):
        self._send(401, {"meta": {"rc": "error", "msg": "api.err.LoginRequired"}, "data": []})

    def _record(self, what):
        self.udm.log.append(what)
        print(f"[udm] {what}", flush=True)

    # --- routes -------------------------------------------------------------
    def do_GET(self):
        u = self.udm
        if self.path == "/__sim/state":
            with u.lock:
                return self._send(200, {"wlans": u.wlans, "faults": u.faults, "log": u.log})
        with u.lock:
            token = self._session()
            if not token or u.faults.pop("expire_session", False):
                u.sessions.pop(token, None)
                self._record(f"GET {self.path} -> 401")
                return self._unauthorized()
            if self.path == WLAN_BASE:
                if u.pending:
                    wid, pw, left = u.pending
                    if left <= 0:
                        next(w for w in u.wlans if w["_id"] == wid)["x_passphrase"] = pw
                        u.pending = None
                    else:
                        u.pending = (wid, pw, left - 1)
                self._record("GET wlanconf")
                return self._send(200, {"meta": {"rc": "ok"}, "data": u.wlans}, chunked=True)
        self._send(404, {"meta": {"rc": "error", "msg": "api.err.NotFound"}, "data": []})

    def do_POST(self):
        u = self.udm
        body = self._body()
        if self.path == "/__sim/faults":
            with u.lock:
                u.faults.update(json.loads(body or b"{}"))
            return self._send(200, {"ok": True})
        if self.path == "/__sim/reset":
            with u.lock:
                u.faults.clear()
                u.sessions.clear()
                u.log.clear()
            return self._send(200, {"ok": True})
        if self.path == "/api/auth/login":
            req = json.loads(body or b"{}")
            with u.lock:
                if req.get("username") != u.user or req.get("password") != u.password:
                    self._record("login -> 401 (fel uppgifter)")
                    return self._send(401, {"code": "AUTHENTICATION_FAILED_INVALID_CREDENTIALS"})
                token, csrf = secrets.token_hex(16), secrets.token_hex(8)
                u.sessions[token] = csrf
                self._record("login ok")
            return self._send(200, {"unique_id": "sim", "username": u.user}, headers=[
                ("Set-Cookie", f"TOKEN={token}; path=/; samesite=strict; secure; httponly"),
                ("X-CSRF-Token", csrf),
            ])
        self._send(404, {})

    def do_PUT(self):
        u = self.udm
        body = self._body()
        with u.lock:
            token = self._session()
            if not token or u.faults.pop("expire_session", False):
                u.sessions.pop(token, None)
                self._record("PUT -> 401")
                return self._unauthorized()
            if self.headers.get("X-CSRF-Token") != u.sessions[token]:
                self._record("PUT -> 403 (CSRF)")
                return self._send(403, {"meta": {"rc": "error", "msg": "api.err.InvalidCsrfToken"}})
            if not self.path.startswith(WLAN_BASE + "/"):
                return self._send(404, {})
            wid = self.path.rsplit("/", 1)[1]
            wlan = next((w for w in u.wlans if w["_id"] == wid), None)
            if wlan is None:
                return self._send(400, {"meta": {"rc": "error", "msg": "api.err.IdInvalid"}, "data": []})
            if u.faults.pop("reject_put", False):
                self._record("PUT -> rc=error (injicerat)")
                return self._send(200, {"meta": {"rc": "error", "msg": "api.err.InvalidPayload"}, "data": []})
            pw = json.loads(body)["x_passphrase"]
            if not 8 <= len(pw) <= 63:
                return self._send(200, {"meta": {"rc": "error", "msg": "api.err.InvalidPassphrase"}, "data": []})
            delay = u.faults.pop("apply_after_reads", 0)
            if delay:
                u.pending = (wid, pw, delay)
            else:
                wlan["x_passphrase"] = pw
            self._record(f"PUT {wlan['name']} x_passphrase={pw}" + (f" (syns efter {delay} läsningar)" if delay else ""))
            headers = []
            if u.faults.get("rotate_csrf"):
                u.sessions[token] = secrets.token_hex(8)
                headers.append(("X-Updated-CSRF-Token", u.sessions[token]))
            if u.faults.pop("drop_put_response", False):
                self._record("PUT-svaret tappas (anslutningen stängs)")
                self.close_connection = True
                return
        self._send(200, {"meta": {"rc": "ok"}, "data": [wlan]}, headers=headers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--user", default="sim-admin")
    ap.add_argument("--password", default="sim-password")
    a = ap.parse_args()
    Handler.udm = Udm(a.user, a.password)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"[udm] mock UDM lyssnar på 127.0.0.1:{a.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
