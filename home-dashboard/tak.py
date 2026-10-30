"""TAK connector: the fleet on a TAK server's map (ATAK / WinTAK / iTAK see it live).

Sends Cursor-on-Target (CoT) events for what has a position:
  nodes       friendly ground sensors (a-f-G-E-S), where they are placed or their own GPS
  drones      Remote ID drones as unknown UAVs (a-u-A-M-H-Q), with course and speed
  pilots      their operators (a-u-G), linked to the drone in the remarks
  devices     triangulated devices (a-u-G), the fix's error as the circle (ce)
  wardrive    wardriven WiFi networks / BLE devices with a GPS position, heard in the last
              5 minutes (a-u-G-E-S)

Off until enabled in Settings. Transports: tcp (TAK server streaming, usually 8087), tls
(8089, with a client certificate: .p12 + password, or PEM cert + key) or udp (a TAK
server's input or the 239.2.3.1:6969 SA multicast). Every event repeats each interval so
clients joining later see the picture; stale times let markers fade when we stop.
"""
import os
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from xml.sax.saxutils import escape, quoteattr

DEFAULTS = {
    "enabled": False, "proto": "tcp", "host": "", "port": 8087,
    "cert": "", "cert_pass": "", "key": "",       # tls: .p12 (+ password) or PEM cert + key
    "user": "", "password": "",                   # tcp with auth="file": a user from the server's UserAuthenticationFile
    "callsign": "OUI-SPY", "interval": 5,
    "send": {"nodes": True, "drones": True, "pilots": True, "devices": False, "wardrive": False},
}
WD_RECENT_S = 300
WD_MAX = 300       # most wardriven devices sent per round


def _iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + f".{int((t % 1) * 1000):03d}Z"


def cot(uid, typ, lat, lon, callsign, *, hae=None, ce=None, stale_s=60, how="m-g", course=None,
        speed=None, remarks="", color=None):
    now = time.time()
    track = (f'<track course="{course:.1f}" speed="{speed:.1f}"/>'
             if course is not None and speed is not None else "")
    col = f'<color argb="{color}"/>' if color is not None else ""
    # No XML declaration: on a TCP stream the server reads one <event> after another.
    return (f'<event version="2.0" uid={quoteattr(uid)} type="{typ}" how="{how}" time="{_iso(now)}" '
            f'start="{_iso(now)}" stale="{_iso(now + stale_s)}">'
            f'<point lat="{lat:.7f}" lon="{lon:.7f}" hae="{hae if hae is not None else 9999999.0}" '
            f'ce="{ce if ce is not None else 9999999.0}" le="9999999.0"/>'
            f'<detail><contact callsign={quoteattr(callsign)}/>{track}{col}'
            f'<remarks>{escape(remarks)}</remarks></detail></event>').encode()


class Tak:
    def __init__(self, get_settings, fleet, drones, wardrive, node_name, data_dir):
        self.get_settings = get_settings
        self.fleet, self.drones, self.wardrive = fleet, drones, wardrive
        self.node_name = node_name
        self.data_dir = data_dir
        self.lock = threading.Lock()
        self.sock = None
        self.cfg = None
        self.status = {"state": "off", "error": "", "sent": 0, "last": None, "items": 0}
        self._wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="tak").start()

    # ---------------------------------------------------------------- control
    def reload(self):
        """Settings changed: drop the connection; the loop reconnects with the new ones."""
        self._close()
        self._wake.set()

    def snapshot(self):
        with self.lock:
            return dict(self.status)

    def _set(self, **kw):
        with self.lock:
            self.status.update(kw)

    def _close(self):
        s, self.sock = self.sock, None
        if s:
            try:
                s.close()
            except OSError:
                pass

    # ---------------------------------------------------------------- transport
    def _tls_context(self, c):
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE   # TAK servers run their own CA; we only authenticate ourselves
        cert, key = c.get("cert", ""), c.get("key", "")
        if cert.lower().endswith((".p12", ".pfx")):
            # The system openssl turns the bundle into a PEM pair kept beside the database.
            pem = os.path.join(self.data_dir, "tak-client.pem")
            src_m = os.path.getmtime(cert)
            if not os.path.exists(pem) or os.path.getmtime(pem) < src_m:
                with tempfile.NamedTemporaryFile("w", delete=False) as pw:
                    pw.write(c.get("cert_pass", ""))
                try:
                    r = subprocess.run(["openssl", "pkcs12", "-in", cert, "-nodes", "-passin", f"file:{pw.name}",
                                        "-out", pem], capture_output=True, text=True, timeout=20)
                    if r.returncode != 0:
                        r = subprocess.run(["openssl", "pkcs12", "-legacy", "-in", cert, "-nodes", "-passin",
                                            f"file:{pw.name}", "-out", pem], capture_output=True, text=True, timeout=20)
                finally:
                    os.unlink(pw.name)
                if r.returncode != 0:
                    raise OSError("could not read the .p12 (wrong password?): " + r.stderr.strip()[:120])
                os.chmod(pem, 0o600)
            ctx.load_cert_chain(pem)
        elif cert:
            ctx.load_cert_chain(cert, key or None)
        return ctx

    def _connect(self, c):
        proto, host, port = c["proto"], c["host"], int(c["port"])
        if proto == "udp":
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)   # SA multicast stays on this LAN
            s.connect((host, port))
            return s
        raw = socket.create_connection((host, port), timeout=8)
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if proto == "tls":
            return self._tls_context(c).wrap_socket(raw, server_hostname=host)
        if c.get("user"):   # TAK's streaming login, before any event
            raw.sendall(f'<auth><cot username={quoteattr(c["user"])} password={quoteattr(c.get("password", ""))}/></auth>'.encode())
        return raw

    def _send(self, events):
        for e in events:
            self.sock.sendall(e) if self.cfg["proto"] != "udp" else self.sock.send(e)

    # ---------------------------------------------------------------- picture
    def events(self, c):
        send = {**DEFAULTS["send"], **(c.get("send") or {})}
        cs = (c.get("callsign") or "OUI-SPY").strip()
        stale = max(30, int(c.get("interval") or 5) * 6)
        out = []
        if send["nodes"]:
            for n in self.fleet.snapshot(lists=False)["nodes"]:
                if n.get("lat") is None or n.get("lon") is None:
                    continue
                cfg = n.get("cfg") or {}
                gps = str(cfg.get("fw") or "").strip()[-1:] in ("G", "Y")
                role = "home" if n.get("is_home") else "node"
                out.append(cot(f"ouispy-node-{n['id']}", "a-f-G-E-S", n["lat"], n["lon"],
                               f"{cs} {self.node_name(n['id'])}", stale_s=max(stale, 120),
                               how="m-g" if gps else "h-e",
                               remarks=f"OUI-SPY {role} {n['id']} {'online' if n.get('online') else 'OFFLINE'}"
                                       f" fw {cfg.get('fw', '?')}"))
        if send["drones"] or send["pilots"]:
            for d in self.drones.snapshot(track=0):
                if not d.get("live"):
                    continue
                faa = d.get("faa") if isinstance(d.get("faa"), dict) else {}
                name = d.get("alias") or faa.get("model") or d.get("id") or d["mac"]
                if send["drones"] and d.get("lat") is not None:
                    out.append(cot(f"ouispy-drone-{d['mac']}", "a-u-A-M-H-Q", d["lat"], d["lon"], f"UAS {name}",
                                   hae=d.get("alt"), stale_s=stale, course=d.get("hdg"), speed=d.get("spd"),
                                   remarks=f"Remote ID {d.get('id') or ''} {d.get('ua_type') or ''} "
                                           f"{d.get('status') or ''} heard by {d.get('closest') or '?'}"))
                if send["pilots"] and d.get("plat") is not None:
                    out.append(cot(f"ouispy-pilot-{d['mac']}", "a-u-G", d["plat"], d["plon"], f"PILOT {name}",
                                   hae=d.get("palt"), stale_s=stale,
                                   remarks=f"operator of {name} ({d.get('op_loc') or 'location type unknown'})"
                                           + (f", {d['pilot_dist_m']} m from it" if d.get("pilot_dist_m") is not None else "")))
        if send["devices"]:
            for fx in list(self.fleet.tri_fixes.values()):
                if not fx.get("ok") or time.time() - fx.get("ts", 0) > 120:
                    continue
                out.append(cot(f"ouispy-dev-{fx['mac']}", "a-u-G", fx["lat"], fx["lon"], f"DEV {fx['mac'][-8:]}",
                               ce=fx.get("err"), stale_s=stale,
                               remarks=f"{fx['mac']} triangulated by {fx.get('n', '?')} nodes"
                                       f"{' (rough)' if fx.get('rough') else ''}, +/- {round(fx.get('err') or 0)} m"))
        if send["wardrive"] and self.wardrive is not None:
            cut = time.time() - WD_RECENT_S
            for w in self.wardrive.devices("history", limit=WD_MAX, placed=True):
                if (w.get("last") or 0) < cut:
                    break   # newest first
                wifi = w["kind"] == "wifi"
                label = w.get("name") or w.get("vendor") or ("hidden network" if wifi else "BLE device")
                out.append(cot(f"ouispy-wd-{w['kind']}-{w['mac']}", "a-u-G-E-S", w["lat"], w["lon"],
                               f"{'WIFI' if wifi else 'BLE'} {label}"[:40], ce=w.get("acc"), stale_s=max(stale, WD_RECENT_S),
                               remarks=f"{w['mac']} {('ch ' + str(w.get('ch')) + ' ' + str(w.get('sec') or '')) if wifi else 'BLE'}"
                                       f" {w.get('rssi')} dBm, wardriven by {self.node_name(w.get('node') or '?')}"))
        return out

    # ---------------------------------------------------------------- loop
    def _run(self):
        backoff = 2
        while True:
            try:
                c = {**DEFAULTS, **(self.get_settings() or {})}
            except Exception:   # the dashboard is still starting up
                time.sleep(2)
                continue
            if not c.get("enabled") or not c.get("host"):
                self._close()
                self._set(state="off", error="")
                self._wake.wait(5)
                self._wake.clear()
                continue
            try:
                if self.sock is None:
                    self.cfg = c
                    self._set(state="connecting", error="")
                    self.sock = self._connect(c)
                    self._set(state="connected", error="")
                    backoff = 2
                ev = self.events(c)
                self._send(ev)
                self._set(sent=self.status["sent"] + len(ev), last=time.time(), items=len(ev))
            except (OSError, ssl.SSLError, ValueError) as e:
                self._close()
                self._set(state="error", error=str(e)[:160])
                self._wake.wait(backoff)
                self._wake.clear()
                backoff = min(60, backoff * 2)
                continue
            self._wake.wait(max(2, int(c.get("interval") or 5)))
            self._wake.clear()
