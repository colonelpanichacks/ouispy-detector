"""Feeds into the Fleet: USB serial, MQTT, demo generator, log replay.

Each source is a daemon thread with a stop() and a status() dict that the UI
shows in the header. Sources never raise: hardware comes and goes.
"""
import json
import random
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:      # pragma: no cover - surfaced in status()
    serial = None
    list_ports = None

try:
    import paho.mqtt.client as mqtt
except ImportError:      # pragma: no cover
    mqtt = None

ESPRESSIF_VID = 0x303A   # XIAO ESP32-S3 native USB (USB-Serial/JTAG + TinyUSB CDC)


def list_serial_ports():
    if list_ports is None:
        return []
    out = []
    for p in list_ports.comports():
        out.append({"device": p.device, "desc": p.description or "",
                    "vid": p.vid, "pid": p.pid, "serial": p.serial_number or "",
                    "espressif": p.vid == ESPRESSIF_VID})
    out.sort(key=lambda p: (not p["espressif"], p["device"]))
    return out


class Source(threading.Thread):
    kind = "source"

    def __init__(self, fleet, on_status):
        super().__init__(daemon=True)
        self.fleet = fleet
        self.on_status = on_status
        self.stop_evt = threading.Event()
        self.state = {"kind": self.kind, "connected": False, "detail": "starting"}

    def set_state(self, **kw):
        changed = any(self.state.get(k) != v for k, v in kw.items())
        self.state.update(kw)
        if changed:
            self.on_status()

    def status(self):
        return dict(self.state)

    def stop(self):
        self.stop_evt.set()


# ----------------------------------------------------------------------
# USB serial
# ----------------------------------------------------------------------
class SerialSource(Source):
    kind = "serial"

    def __init__(self, fleet, on_status, port="auto", baud=115200, record=None,
                 get_pref=None, set_pref=None):
        super().__init__(fleet, on_status)
        self.port_cfg = port or "auto"
        self.baud = baud
        self.record = record
        self.ser = None
        self.wlock = threading.Lock()
        # Several boards can be on USB at once (flashing, debugging). Auto
        # mode wants the HOME: prefer the USB serial number last seen saying
        # role=home, and skip a port whose telemetry says role=remote.
        self.get_pref = get_pref or (lambda: None)
        self.set_pref = set_pref or (lambda sn: None)
        self.skip = {}          # usb serial -> time it said role=remote
        self.cur_sn = None

    def _pick_port(self):
        if self.port_cfg != "auto":
            return self.port_cfg
        now = time.time()
        cands = [p for p in list_serial_ports() if p["espressif"]
                 and now - self.skip.get(p["serial"], 0) > 60]
        if not cands:
            return None
        pref = self.get_pref()
        cands.sort(key=lambda p: p["serial"] != pref)
        self.cur_sn = cands[0]["serial"]
        return cands[0]["device"]

    def _role_check(self, line):
        """True = keep this port; False = it's a remote, go find the home."""
        if self.port_cfg != "auto" or not line.startswith("TLM "):
            return True
        try:
            role = json.loads(line[4:]).get("role")   # top-level: the roster lists roles too
        except ValueError:
            return True
        if role == "home":
            if self.cur_sn and self.cur_sn != self.get_pref():
                self.set_pref(self.cur_sn)
            return True
        if role == "remote":
            others = [p for p in list_serial_ports() if p["espressif"] and p["serial"] != self.cur_sn]
            if others:
                self.skip[self.cur_sn] = time.time()
                return False
        return True

    def run(self):
        if serial is None:
            self.set_state(detail="pyserial not installed")
            return
        rec = open(self.record, "a", buffering=1, encoding="utf-8") if self.record else None
        while not self.stop_evt.is_set():
            port = self._pick_port()
            if not port:
                self.set_state(connected=False, port=None,
                               detail="waiting for XIAO (USB VID 0x303A)")
                self.stop_evt.wait(0.5)      # fast rescan: plug-in shows up at once
                continue
            try:
                # The S3's USB-Serial/JTAG maps DTR/RTS onto its reset and
                # boot-mode lines (esptool resets it that way). USB CDC sends
                # both lines in ONE control message, so the OS-default open
                # (both asserted) and close (both dropped) are atomic and
                # harmless. Setting them one at a time is NOT: pyserial drops
                # DTR before RTS, and that instant (RTS high, DTR low) is a
                # reset pulse. So: plain open, never touch dtr/rts.
                self.ser = serial.Serial(port, self.baud, timeout=1)
                # A newline is ignored by the console but gets the board's
                # USB-CDC sending again if an earlier host left it stalled.
                self.ser.write(b"\n")
                last_rx = time.time()
                opened_at = time.time()
                proven = self.port_cfg != "auto"   # an explicit --serial port is trusted
                self.set_state(connected=True, port=port, detail=f"{port} @ {self.baud}")
                buf = b""
                while not self.stop_evt.is_set():
                    # Auto mode keeps a port only once it proves to be an
                    # OUI-SPY node (a TLM line). Other Espressif boards on
                    # USB (other projects!) are released within 12 s, never
                    # held, and skipped for a while.
                    if not proven and time.time() - opened_at > 12:
                        if self.cur_sn:
                            self.skip[self.cur_sn] = time.time()
                        self.set_state(connected=False, detail=f"{port} is not an OUI-SPY node, released")
                        break
                    chunk = self.ser.read(self.ser.in_waiting or 1)
                    if not chunk:
                        # The firmware prints every 5 s. Silent for 12 s with the
                        # port still open means a stalled CDC: reopen (atomic,
                        # never resets the board).
                        if time.time() - last_rx > 12:
                            self.set_state(detail=f"{port}: silent 12 s, reopening")
                            break
                        continue
                    last_rx = time.time()
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").rstrip("\r")
                        if rec:
                            rec.write(f"{time.time():.3f}\t{line}\n")
                        if line.startswith("TLM "):
                            proven = True
                        if not self._role_check(line):
                            raise serial.SerialException("that port is a remote node, looking for the home")
                        self.fleet.feed_line(line, "serial")
                    if len(buf) > 8192:      # no newline in 8 KB: junk, drop it
                        buf = b""
            except (OSError, serial.SerialException) as e:
                # The port vanishes on every reboot of a native-USB board;
                # back off and re-detect (the device path may even change).
                self.set_state(connected=False, detail=f"lost {port}: {str(e)[:80]}")
            finally:
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                self.ser = None
            self.stop_evt.wait(0.5)
        if rec:
            rec.close()
        self.set_state(connected=False, detail="stopped")

    def send(self, line):
        with self.wlock:
            if not self.ser:
                return False
            try:
                self.ser.write((line.strip() + "\n").encode())
                return True
            except Exception:
                return False


# ----------------------------------------------------------------------
# MQTT (the home node's detection publishes)
# ----------------------------------------------------------------------
class MqttSource(Source):
    kind = "mqtt"

    def __init__(self, fleet, on_status, host, port=1883, user="", password="",
                 topic="+/detection"):
        super().__init__(fleet, on_status)
        self.host, self.port = host, int(port or 1883)
        self.user, self.password = user, password
        self.topic = topic or "+/detection"
        self.client = None

    def run(self):
        if mqtt is None:
            self.set_state(detail="paho-mqtt not installed")
            return
        try:
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                            client_id=f"ouispy-dash-{random.randint(0, 0xFFFFFF):06x}")
        except AttributeError:          # paho 1.x
            c = mqtt.Client(client_id=f"ouispy-dash-{random.randint(0, 0xFFFFFF):06x}")
        if self.user:
            c.username_pw_set(self.user, self.password or None)
        c.on_connect = self._on_connect
        c.on_disconnect = self._on_disconnect
        c.on_message = self._on_message
        c.reconnect_delay_set(1, 30)
        self.client = c
        self.set_state(detail=f"connecting {self.host}:{self.port}")
        while not self.stop_evt.is_set():
            try:
                c.connect(self.host, self.port, keepalive=30)
                break
            except Exception as e:
                self.set_state(connected=False, detail=f"{self.host}:{self.port} {str(e)[:60]}")
                self.stop_evt.wait(5)
        if self.stop_evt.is_set():
            return
        c.loop_start()
        self.stop_evt.wait()
        c.loop_stop()
        try:
            c.disconnect()
        except Exception:
            pass
        self.set_state(connected=False, detail="stopped")

    def _on_connect(self, client, userdata, flags, rc, props=None):
        ok = (rc == 0) if isinstance(rc, int) else not rc.is_failure
        if ok:
            client.subscribe(self.topic)
            base = self.topic.rsplit("/", 1)[0] if "/" in self.topic else "+"
            client.subscribe(f"{base}/availability")
            self.set_state(connected=True, detail=f"{self.host}:{self.port} {self.topic}")
        else:
            self.set_state(connected=False, detail=f"refused: {rc}")

    def _on_disconnect(self, client, userdata, *args):
        self.set_state(connected=False, detail=f"{self.host}:{self.port} disconnected")

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8", "replace").strip()
        if msg.topic.endswith("/availability"):
            self.set_state(device=msg.topic.split("/")[0], availability=payload)
            return
        try:
            obj = json.loads(payload)
        except ValueError:
            return
        if isinstance(obj, dict) and obj.get("mac"):
            self.fleet.feed_detection(obj, "mqtt")


# ----------------------------------------------------------------------
# Demo: a fake fleet, rendered as real firmware lines through the parser
# ----------------------------------------------------------------------
DEMO_HOME = "DE0A01"
DEMO_REMOTES = ["DE0B02", "DE0C03", "DE0D04", "DE0E05"]
DEMO_TARGETS = [
    ("00:25:DF", "OUI", "AXON", "ble"), ("00:25:DF", "PROBE", "AXON", "wifi"),
    ("B4:1E:52", "OUI", "FLOCK SAFETY", "ble"), ("70:C9:4E", "BEACON", "FLOCK SAFETY", "wifi"),
    ("D4:2D:C5", "PROBE", "I-PRO", "wifi"), ("E0:A7:00", "BEACON", "Verkada", "wifi"),
    ("0C:9A:E6", "OUI", "DJI", "ble"), ("7C:2A:9E", "OUI", "META/RAYBAN SMARTGLASSES", "ble"),
]

# --demo-showcase: screenshot-safe sample data. No police/ALPR vendors, and
# every MAC ends in an obviously synthetic 00:00:xx, so nothing in a
# published screenshot can point at a real device.
SHOWCASE_TARGETS = [
    ("60:60:1F", "OUI", "DJI", "ble"), ("60:60:1F", "BEACON", "DJI", "wifi"),
    ("A0:14:3D", "OUI", "PARROT", "ble"), ("38:1D:14", "PROBE", "SKYDIO", "wifi"),
    ("7C:2A:9E", "OUI", "META/RAYBAN SMARTGLASSES", "ble"), ("E0:A7:00", "BEACON", "Verkada", "wifi"),
    ("DE:AD:BE", "BEACON", "PWNAGOTCHI", "wifi"),
]
SHOWCASE = False


def use_showcase():
    global SHOWCASE
    SHOWCASE = True
    DEMO_TARGETS[:] = SHOWCASE_TARGETS


def demo_mac(oui):
    if SHOWCASE:
        return oui + ":00:00:" + f"{random.randint(1, 40):02X}"
    return oui + ":" + ":".join(f"{random.randint(0, 255):02X}" for _ in range(3))


class DemoSource(Source):
    kind = "demo"

    def __init__(self, fleet, on_status):
        super().__init__(fleet, on_status)
        import filters as flt
        self.flt = flt
        self.t0 = time.time()
        self.pkts = {n: random.randint(40, 400) for n in DEMO_REMOTES}
        self.rssi = {"DE0B02": -58, "DE0C03": -71, "DE0D04": -84, "DE0E05": -66}
        self.counters = {"rx": 0, "tx": 0, "txf": 0, "dup": 0}
        self.flap_until = 0
        self.macs = [self._mac(o) for o, *_ in DEMO_TARGETS for _ in range(2)]
        self.ch = 6
        self.down_until = {}          # node -> time it comes back from a reboot
        swap = {"axon": "meta"} if SHOWCASE else {}
        preset = lambda *names: [{"t": t, "id": i, "d": d, "full": 0}
                                 for n in names for t, i, d in flt.PRESETS[swap.get(n, n)]]
        # Simulated per-node settings. DE0C03 has drifted from the home's
        # filters; DE0E05 runs pre-remote-management firmware and refuses
        # every new command, so the UI's failure paths show up too.
        self.nodes = {
            DEMO_HOME: {"buzzer": 1, "led": 1, "locked": 1, "key": 1, "old": False,
                        "filters": preset("axon", "verkada"), "boot": self.t0 - 3600},
            "DE0B02": {"buzzer": 1, "led": 1, "locked": 1, "key": 1, "old": False,
                       "filters": preset("axon", "verkada"), "boot": self.t0 - 7200},
            "DE0C03": {"buzzer": 0, "led": 1, "locked": 1, "key": 1, "old": False,
                       "filters": preset("axon"), "boot": self.t0 - 5400},
            "DE0D04": {"buzzer": 1, "led": 0, "locked": 1, "key": 1, "old": False,
                       "filters": preset("axon", "verkada"), "boot": self.t0 - 900},
            "DE0E05": {"buzzer": 1, "led": 1, "locked": 0, "key": 0, "old": True,
                       "filters": preset("ipro"), "boot": self.t0 - 86000},
        }
        self.mf = None

    @staticmethod
    def _mac(oui):
        return demo_mac(oui)

    def run(self):
        self.set_state(connected=True, detail="simulated fleet (5 nodes)")
        self.fleet.feed_line(f"[MESH] ESP-NOW LR up: HOME node, channel 6, id {DEMO_HOME}", "demo")
        next_tlm = 0
        next_det = time.time() + 2
        while not self.stop_evt.wait(0.25):
            now = time.time()
            if now >= next_tlm:
                next_tlm = now + 5
                self._tlm(now)
            if now >= next_det:
                next_det = now + random.uniform(2.5, 9)
                self._detection()
        self.set_state(connected=False, detail="stopped")

    def _up(self, n, now=None):
        now = now or time.time()
        if now < self.down_until.get(n, 0):
            return False
        return not (n == "DE0D04" and now < self.flap_until)

    def _tlm(self, now):
        if not self._up(DEMO_HOME, now):
            return
        # DE0D04 is the far node: it drops out for ~40 s every few minutes.
        if self.flap_until < now and random.random() < 0.05:
            self.flap_until = now + 40
        nodes = []
        for n in DEMO_REMOTES:
            if not self._up(n, now):
                continue
            self.pkts[n] += random.randint(14, 22)
            self.rssi[n] = max(-95, min(-40, self.rssi[n] + random.randint(-3, 3)))
            nodes.append({"id": n, "role": "remote", "last": random.randint(0, 2),
                          "pkts": self.pkts[n], "rssi": self.rssi[n]})
        c = self.counters
        c["rx"] += sum(random.randint(15, 25) for _ in nodes)
        c["tx"] += 16
        c["txf"] += 1 if random.random() < 0.1 else 0
        c["dup"] += random.randint(0, 6)
        h = self.nodes[DEMO_HOME]
        tlm = {"v": 1, "id": DEMO_HOME, "role": "home", "mode": "scan", "mesh": 1, "ch": self.ch,
               "up": int(now - h["boot"]), "heap": random.randint(148000, 156000),
               "dev": len(self.macs), "filters": len(h["filters"]), "n": len(nodes),
               "rx": c["rx"], "q": c["rx"] - c["dup"], "dup": c["dup"], "self": 0, "bad": 0,
               "tx": c["tx"], "txf": c["txf"], "txcb": 0, "pend": random.randint(0, 2),
               "lrssi": nodes[0]["rssi"] if nodes else 0, "heal": 0, "mqtt": 1,
               "nodes": nodes}
        self.fleet.feed_line(f"[HEARTBEAT] phase=1 ch={self.ch} dev={len(self.macs)} mesh={len(nodes)}", "demo")
        self.fleet.feed_line("TLM " + json.dumps(tlm, separators=(",", ":")), "demo")

    def _detection(self):
        oui, ftype, label, dom = random.choice(DEMO_TARGETS)
        if random.random() < 0.05:
            mac = self._mac(oui)
            self.macs.append(mac)
        else:
            cands = [m for m in self.macs if m.startswith(oui)] or [self._mac(oui)]
            mac = random.choice(cands)
        live = [n for n in DEMO_REMOTES if self._up(n)]
        if not self._up(DEMO_HOME):
            return
        node = random.choice([DEMO_HOME] + live + live)
        desc = label if node == DEMO_HOME else f"{label} [mesh {node}]"
        det = {"mac": mac, "alias": "", "rssi": random.randint(-92, -48), "type": ftype,
               "match": oui.replace(":", ""), "desc": desc, "node": node, "dom": dom}
        self.fleet.feed_line(json.dumps(det), "demo")

    # ------------------------------------------------------------------
    # console simulator: answers like the firmware's remote management
    # ------------------------------------------------------------------
    def _say(self, line, delay=0.05):
        threading.Timer(delay, lambda: self.fleet.feed_line(line, "demo")).start()

    def _rst(self, n):
        c = self.nodes[n]
        return "RST " + json.dumps({
            "node": n, "role": "home" if n == DEMO_HOME else "remote", "mode": "scan",
            "ch": self.ch, "buzzer": c["buzzer"], "led": c["led"], "key": c["key"],
            "locked": c["locked"], "mqtt": 1 if n == DEMO_HOME else 0, "heal": 0,
            "peers": 4 if n == DEMO_HOME else random.randint(1, 3),
            "lrssi": self.rssi.get(n, -60), "filters": len(c["filters"]),
            "fcrc": self.flt.crc(c["filters"]), "up": int(time.time() - c["boot"]),
            "heap": random.randint(140000, 158000), "dev": random.randint(2, 30),
            **self._delivery(),
            "fw": "Oct 26 2026 13:10"}, separators=(",", ":"))

    @staticmethod
    def _delivery():
        sent = random.randint(40, 400)
        lost = random.randint(0, 3)
        return {"dsent": sent, "dacked": sent - lost, "dexp": lost}

    def _rfl(self, n):
        return "RFL " + json.dumps({"node": n, "ok": 1, "filters": self.nodes[n]["filters"]},
                                   separators=(",", ":"))

    def _reboot(self, n, secs=9):
        self.down_until[n] = time.time() + secs
        self.nodes[n]["boot"] = time.time() + secs

    def _targets(self, t):
        if t == "all":
            return [n for n in DEMO_REMOTES if self._up(n)]
        t = t.upper()
        return [t] if t in self.nodes and t != DEMO_HOME and self._up(t) else []

    def _cmd(self, op, t, code):
        cid = random.randint(1, 0xFFFF)
        self._say("CMD " + json.dumps({"op": op, "id": cid, "target": t if t == "all" else t.upper(),
                                       "cmd": code}, separators=(",", ":")))
        return cid

    def _ack(self, n, code, cid, st, cnt=0, delay=None):
        self._say("ACK " + json.dumps({"node": n, "cmd": code, "id": cid, "st": st, "n": cnt},
                                      separators=(",", ":")), delay or random.uniform(0.3, 2.0))

    def send(self, line):
        if not self._up(DEMO_HOME):
            return False
        parts = line.strip().split()
        if not parts:
            return True
        verb, a = parts[0], parts[1:]
        home = self.nodes[DEMO_HOME]
        legacy = {"raudio": 1, "rreboot": 3, "filters": 2}   # codes old firmware obeys unwrapped

        if verb == "status":
            self._say(self._rst(DEMO_HOME))
        elif verb == "filters":
            self._say(self._rfl(DEMO_HOME))
        elif verb == "audio" and len(a) == 2:
            home["buzzer"], home["led"] = int(a[0] != "0"), int(a[1] != "0")
            self._say(f"OK audio buzzer={home['buzzer']} led={home['led']}")
        elif verb == "reboot":
            self._say("OK reboot - rebooting")
            self._reboot(DEMO_HOME, 6)
        elif verb == "ident":
            self._say(f"OK identify {a[0] if a else 10} s")
        elif verb == "preset" and a and a[0] in self.flt.PRESETS:
            added = self._merge(DEMO_HOME, a[0])
            self._say(f"OK preset {a[0]} installed live ({added} signatures)")
        elif verb == "meshrechan" and a:
            self.ch = int(a[0])
            self._say(f"OK re-channeled fleet to {self.ch} - rebooting onto it")
            for n in self.nodes:
                self._reboot(n, 8)
        elif verb == "meshpush":
            self._say(f"OK filter push flooded to fleet ({len(home['filters'])} records)")
            cid = self._cmd("meshpush", "all", 2)
            for n in self._targets("all"):
                self.nodes[n]["filters"] = [dict(f) for f in home["filters"]]
                self._ack(n, 2, cid, 0, len(home["filters"]))
        elif verb in ("rstatus", "raudio", "rreboot", "rident", "rpreset", "rfilters") and a:
            t = a[0]
            codes = {"rstatus": 6, "raudio": 1, "rreboot": 3, "rident": 7, "rpreset": 8, "rfilters": 9}
            code = codes[verb]
            cid = self._cmd(verb, t, code)
            for n in self._targets(t):
                c = self.nodes[n]
                if c["old"] and not (t == "all" and verb in legacy):
                    if verb != "rstatus":
                        self._ack(n, code if t == "all" else 5, cid, 1)
                    continue
                if verb == "rstatus":
                    self._say(self._rst(n), random.uniform(0.3, 2.5))
                elif verb == "rfilters":
                    self._say(self._rfl(n), random.uniform(0.5, 3.0))
                elif verb == "raudio":
                    c["buzzer"], c["led"] = int(a[1] != "0"), int(a[2] != "0")
                    self._ack(n, code, cid, 0)
                elif verb == "rreboot":
                    self._ack(n, code, cid, 0)
                    self._reboot(n)
                elif verb == "rident":
                    self._ack(n, code, cid, 0, int(a[1]) if len(a) > 1 else 10)
                elif verb == "rpreset":
                    added = self._merge(n, a[1])
                    self._ack(n, code, cid, 0, added)
        elif verb in ("mfbegin", "mfadd", "mfdel") and a:
            self.mf = {"t": a[0], "op": verb, "buf": bytearray()}
            self._say("OK mfbegin")
        elif verb == "mfdata" and self.mf is not None and a:
            self.mf["buf"] += bytes.fromhex(a[0])
            self._say(f"OK mfdata {len(self.mf['buf'])}")
        elif verb == "mfend" and self.mf is not None:
            lst = self.flt.decode(bytes(self.mf["buf"]))
            t, op, self.mf = self.mf["t"], self.mf["op"], None
            if t.lower() == "self" or t.upper() == DEMO_HOME:
                n = self._edit(DEMO_HOME, op, lst)
                self._say(f"OK filters applied live: {n} installed" if n >= 0 else
                          "ERR mfend: refused (it would leave this node with no filters)")
            else:
                code = {"mfbegin": 2, "mfadd": 11, "mfdel": 12}[op]
                cid = self._cmd({"mfbegin": "filters", "mfadd": "fadd", "mfdel": "fdel"}[op], t, code)
                for n in self._targets(t):
                    c = self.nodes[n]
                    if c["old"] and (t != "all" or op != "mfbegin"):
                        self._ack(n, code if t == "all" else 5, cid, 1)
                        continue
                    k = self._edit(n, op, lst)
                    self._ack(n, code, cid, 0 if k >= 0 else 1, max(k, 0))
        else:
            self._say(f"ERR unknown command: {line.strip()[:40]}")
        return True

    def _edit(self, n, op, lst):
        """Firmware filtersEditLive: installed count afterwards, or -1 if refused."""
        key = lambda f: (f["t"], f["id"].upper())
        cur = [] if op == "mfbegin" else [dict(f) for f in self.nodes[n]["filters"]]
        for r in lst:
            hit = next((f for f in cur if key(f) == key(r)), None)
            if op == "mfdel":
                if hit:
                    cur.remove(hit)
            elif hit:
                if r["d"]:
                    hit["d"] = r["d"]
            elif len(cur) < 100:
                cur.append(dict(r))
        if not cur:
            return -1
        self.nodes[n]["filters"] = cur
        return len(cur)

    def _merge(self, n, preset):
        have = {(f["t"], f["id"].upper()) for f in self.nodes[n]["filters"]}
        added = 0
        for t, i, d in self.flt.PRESETS[preset]:
            if (t, i.upper()) not in have:
                self.nodes[n]["filters"].append({"t": t, "id": i, "d": d, "full": 0})
                added += 1
        return added


# ----------------------------------------------------------------------
# Replay a log recorded with --record (or any raw serial capture)
# ----------------------------------------------------------------------
class ReplaySource(Source):
    kind = "replay"

    def __init__(self, fleet, on_status, path, speed=1.0, loop=True):
        super().__init__(fleet, on_status)
        self.path, self.speed, self.loop = path, max(0.1, speed), loop

    def run(self):
        try:
            with open(self.path, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError as e:
            self.set_state(detail=str(e))
            return
        self.set_state(connected=True, detail=f"{self.path} x{self.speed}")
        while not self.stop_evt.is_set():
            prev = None
            for raw in lines:
                if self.stop_evt.is_set():
                    break
                ts, _, line = raw.partition("\t")
                try:
                    t = float(ts)
                except ValueError:
                    t, line = None, raw
                gap = (t - prev) / self.speed if (t is not None and prev is not None) else 0.05
                prev = t if t is not None else prev
                if self.stop_evt.wait(min(max(gap, 0), 10)):
                    break
                self.fleet.feed_line(line, "replay")
            if not self.loop:
                break
        self.set_state(connected=False, detail="replay finished")
