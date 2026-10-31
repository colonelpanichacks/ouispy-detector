"""Meshtastic connector: OUI-SPY alerts as text messages on a Meshtastic mesh.

Off until enabled in Settings. Talks to one Meshtastic radio on this computer's USB (or one
on the network, "tcp:<host>"), never one of our own nodes: it refuses the home node's port
and anything that isn't a Meshtastic radio answering the Meshtastic protocol.

Where messages go:
  recipients set   direct messages to those node IDs (!a1b2c3d4), end-to-end encrypted
  otherwise        the radio's channel <channel>, which must be a PRIVATE channel

It never sends on a public channel: a channel with no key, or the well-known default key
(the LongFast primary every new radio ships with), is refused, so alerts never go out to
everyone in radio range. Airtime is precious: one message per 15 s at most, each under the
200-byte limit, new devices batched into one message a minute.

  new devices   batched, at most 5 a message
  drones        when one first shows up, with its position
  lost nodes    a node reported lost is heard again
  nodes         a node drops off the mesh / comes back (off by default)

It also listens: radios it hears and text sent to it show in Settings.

Tab5 link: a Tab5 out of Wi-Fi range keeps in touch through a Meshtastic radio on its Grove
port (Unit C6L, serial module in TEXTMSG mode). Short coded lines, one per text message:
  car -> home   OSM1 P <node> <lat> <lon> <acc>          where it is (a fleet node's fix)
                OSM1 D <node> <mac> <rssi> <dom> <name>  a detection its USB node made
  home -> car   OSM1 S <on>/<all> <d24> <id>:<up>:<rssi>,...   the mesh, every minute while
                a Tab5 link was heard in the last 30 min (nothing on the air otherwise)
"""
import json
import threading
import time

DEFAULTS = {
    "enabled": False, "port": "", "channel": 1, "recipients": "",
    "send": {"new_devices": True, "drones": True, "lost_nodes": True, "nodes": False},
}
BATCH_S = 60
GAP_S = 15          # between messages: Meshtastic airtime is shared by every radio in range
MAX_BYTES = 200
DEFAULT_PSK = b"\x01"   # "AQ==": the public default key (LongFast and friends)
SUMMARY_S = 60        # home -> Tab5 link: the mesh summary
LINK_FRESH_S = 1800   # ...only while a Tab5 link was heard this recently


def _clip(text):
    b = text.encode()[:MAX_BYTES]
    return b.decode(errors="ignore")


class Mesh:
    def __init__(self, get_settings, node_name, busy_ports, summary=None, feed_line=None, feed_det=None):
        self.get_settings = get_settings
        self.node_name = node_name
        self.busy_ports = busy_ports      # () -> serial ports our own nodes are on
        self.summary = summary            # () -> "OSM1 S ..." for a Tab5 link
        self.feed_line = feed_line        # (line, source): a car's position into the fleet
        self.feed_det = feed_det          # (detection dict, source): a car's detection
        self.link_heard = 0
        self.summary_at = 0
        self.lock = threading.Lock()
        self.status = {"state": "off", "error": "", "radio": "", "channel": "", "sent": 0, "failed": 0,
                       "queued": 0, "last": None, "heard": 0, "inbox": [], "link": None}
        self.iface = None
        self.iface_port = None
        self.outbox = []
        self.new_devs = []
        self.batch_at = 0
        self.last_tx = 0
        self._wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="meshtastic").start()

    # ---------------------------------------------------------------- input
    def notify(self, event, p):
        c = self._cfg()
        if not c or not c.get("enabled"):
            return
        send = c["send"]
        if event == "det" and send["new_devices"] and p.get("is_new"):
            name = p.get("alias") or p.get("descr") or p.get("match") or "device"
            with self.lock:
                if len(self.new_devs) < 20:
                    self.new_devs.append(f"{name[:24]} {p.get('mac')} {p.get('rssi')}dBm @{self.node_name(p.get('closest') or p.get('node'))[:12]}")
                self.batch_at = self.batch_at or time.time() + BATCH_S
        elif event == "drone" and send["drones"] and p.get("reports") == 1:
            faa = p.get("faa") if isinstance(p.get("faa"), dict) else {}
            name = p.get("alias") or faa.get("model") or p.get("id") or p.get("mac")
            where = f"{p['lat']:.5f},{p['lon']:.5f}" if p.get("lat") is not None else "no fix"
            pilot = f" pilot {p['plat']:.5f},{p['plon']:.5f}" if p.get("plat") is not None else ""
            self.queue(f"OUI-SPY DRONE {name} {where}" + (f" {round(p['alt'])}m" if p.get("alt") is not None else "") + pilot)
        elif event == "lost_node" and send["lost_nodes"]:
            self.queue(f"OUI-SPY LOST NODE {self.node_name(p.get('node'))} ({p.get('node')}) is back: {p.get('how')}")
        elif event == "node_event" and send["nodes"] and p.get("event") in ("online", "offline"):
            self.queue(f"OUI-SPY node {self.node_name(p.get('id'))} ({p.get('id')}) {p['event']}")

    def queue(self, text):
        with self.lock:
            if len(self.outbox) < 50:
                self.outbox.append(_clip(text))
            self.status["queued"] = len(self.outbox)
        self._wake.set()

    # ---------------------------------------------------------------- state
    def _cfg(self):
        try:
            return self.get_settings()
        except Exception:
            return None

    @staticmethod
    def recipients(c):
        out = []
        for tok in (c.get("recipients") or "").replace(",", " ").split():
            t = tok.strip().lower().lstrip("!")
            if len(t) == 8 and all(ch in "0123456789abcdef" for ch in t):
                out.append("!" + t)
        return out

    def snapshot(self):
        with self.lock:
            return {**self.status, "inbox": list(self.status["inbox"])}

    def _set(self, **kw):
        with self.lock:
            self.status.update(kw)

    def reload(self):
        self._wake.set()

    # ---------------------------------------------------------------- radio
    def _close(self):
        if self.iface is not None:
            try:
                self.iface.close()
            except Exception:
                pass
        self.iface = self.iface_port = None

    def _open(self, c):
        port = (c.get("port") or "").strip()
        if not port:
            raise RuntimeError("pick the Meshtastic radio's port in Settings")
        if port.startswith("tcp:"):
            from meshtastic.tcp_interface import TCPInterface
            iface = TCPInterface(hostname=port[4:])
        else:
            if port in self.busy_ports():
                raise RuntimeError(f"{port} is one of our own nodes, not a Meshtastic radio")
            from meshtastic.serial_interface import SerialInterface
            iface = SerialInterface(devPath=port)
        from pubsub import pub
        pub.subscribe(self._on_text, "meshtastic.receive.text")
        pub.subscribe(self._on_lost, "meshtastic.connection.lost")
        self.iface, self.iface_port = iface, port
        me = iface.getMyNodeInfo() or {}
        user = me.get("user") or {}
        self._set(radio=f"{user.get('longName') or '?'} ({user.get('id') or '?'})", error="")

    def _channel_ok(self, c):
        """(index, name) of the private channel to broadcast on; raises when it isn't private."""
        idx = int(c.get("channel") or 0)
        chans = self.iface.localNode.channels or []
        ch = next((x for x in chans if x.index == idx), None)
        if ch is None or ch.role == 0:   # DISABLED
            raise RuntimeError(f"channel {idx} isn't set up on the radio: add a private channel there first")
        psk = bytes(ch.settings.psk or b"")
        name = ch.settings.name or ("LongFast" if idx == 0 else f"channel {idx}")
        if not psk:
            raise RuntimeError(f"channel {idx} ({name}) has no encryption: refusing to broadcast alerts in the clear")
        if psk == DEFAULT_PSK:
            raise RuntimeError(f"channel {idx} ({name}) uses the public default key: anyone could read it. "
                               "Pick a channel with its own key")
        return idx, name

    def _on_link(self, frm, text):
        """A coded line from a Tab5 link (OSM1 ...): into the fleet like a node's own line."""
        parts = text.split(" ", 6)
        try:
            if parts[1] == "P" and len(parts) >= 6 and self.feed_line:
                node, lat, lon, acc = parts[2].upper(), float(parts[3]), float(parts[4]), float(parts[5])
                self.feed_line("POS " + json.dumps({"node": node, "lat": lat, "lon": lon, "acc": acc}), "meshtastic")
            elif parts[1] == "D" and len(parts) >= 6 and self.feed_det:
                node, mac, rssi, dom = parts[2].upper(), parts[3].upper(), int(parts[4]), parts[5]
                name = parts[6] if len(parts) > 6 else ""
                self.feed_det({"mac": mac, "rssi": rssi, "node": node, "dom": dom if dom in ("ble", "wifi") else "ble",
                               "desc": name or "via Tab5 link"}, "meshtastic")
            elif parts[1] != "H":   # H: hello, a link with nothing else to say
                return
        except (ValueError, IndexError):
            return
        self.link_heard = time.time()
        with self.lock:
            self.status["link"] = {"from": frm, "ts": self.link_heard}

    def _on_text(self, packet, interface=None):
        try:
            d = packet.get("decoded") or {}
            frm = packet.get("fromId") or "?"
            # A serial-module radio (a Tab5's Grove link) reads a glitch on its line as a stray
            # NUL in front of the next text, or as a text of its own: drop both.
            text = str(d.get("text") or "").lstrip("\x00\r\n\t ")
            if not text.strip("\x00"):
                return
            if text.startswith("OSM1 "):
                self._on_link(frm, text)
                return
            who = ((self.iface.nodes or {}).get(frm) or {}).get("user", {}).get("longName") if self.iface else None
            with self.lock:
                self.status["inbox"] = ([{"from": frm, "name": who or "", "text": text[:200],
                                          "ts": time.time()}] + self.status["inbox"])[:20]
        except Exception:
            pass

    def _on_lost(self, interface=None):
        self._set(state="error", error="radio disconnected")
        self._wake.set()

    def _send(self, c, text):
        to = self.recipients(c)
        if to:
            for r in to:
                self.iface.sendText(text, destinationId=r, wantAck=True)
        else:
            idx, _ = self._channel_ok(c)
            self.iface.sendText(text, channelIndex=idx)
        self.last_tx = time.time()
        self._set(sent=self.status["sent"] + 1, last=self.last_tx)

    def _run(self):
        while True:
            self._wake.wait(3)
            self._wake.clear()
            c = self._cfg()
            if not c or not c.get("enabled"):
                if self.iface is not None:
                    self._close()
                self._set(state="off", error="")
                continue
            try:
                if self.iface is not None and (self.iface_port != (c.get("port") or "").strip()
                                               or self.status["state"] == "error"):
                    self._close()
                if self.iface is None:
                    self._set(state="connecting")
                    self._open(c)
                to = self.recipients(c)
                if to:
                    self._set(channel="direct to " + ", ".join(to))
                else:
                    idx, name = self._channel_ok(c)
                    self._set(channel=f"{idx}: {name}")
                self._set(state="ready", heard=len(self.iface.nodes or {}))
                with self.lock:
                    due = self.new_devs and time.time() >= self.batch_at
                    lines, self.new_devs = (self.new_devs, []) if due else ([], self.new_devs)
                    if due:
                        self.batch_at = 0
                if lines:
                    more = f" +{len(lines) - 5} more" if len(lines) > 5 else ""
                    self.queue(f"OUI-SPY {len(lines)} new: " + "; ".join(lines[:5]) + more)
                if (self.summary and self.link_heard and time.time() - self.link_heard < LINK_FRESH_S
                        and time.time() - self.summary_at >= SUMMARY_S):
                    self.summary_at = time.time()
                    try:
                        self.queue(self.summary())
                    except Exception:
                        pass
                if self.outbox and time.time() - self.last_tx >= GAP_S:
                    with self.lock:
                        text = self.outbox.pop(0)
                        self.status["queued"] = len(self.outbox)
                    try:
                        self._send(c, text)
                    except Exception:
                        self._set(failed=self.status["failed"] + 1)
                        raise
            except Exception as e:
                self._set(state="error", error=str(e)[:200])
                if self.iface is not None and "channel" not in str(e):
                    self._close()
                time.sleep(5)
