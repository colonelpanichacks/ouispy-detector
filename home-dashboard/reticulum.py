"""Reticulum connector: OUI-SPY alerts as LXMF messages (MeshChat, Sideband, NomadNet).

Off until enabled in Settings. Rides on this computer's shared Reticulum instance (rnsd, or
the program that owns it) and never starts its own: a private instance would bring up every
interface in ~/.reticulum (public hubs, RNodes on USB ports our nodes may be using).
The dashboard gets its own LXMF address (a random identity kept in the data folder, nothing
derived from anyone's name) and sends direct messages to the addresses listed in Settings:

  new devices   batched: one message per 30 s at most
  drones        when one first shows up, with its position and its pilot's
  lost nodes    a node reported lost is heard again
  nodes         a node drops off the mesh / comes back (off by default)

It never announces itself unless "announce" is on: an announce travels the whole network.
"""
import os
import re
import signal
import socket
import threading
import time

DEFAULTS = {
    "enabled": False, "recipients": "", "announce": False, "name": "OUI-SPY Mesh",
    "send": {"new_devices": True, "drones": True, "lost_nodes": True, "nodes": False},
}
BATCH_S = 30
PATH_WAIT_S = 120


class Rns:
    def __init__(self, get_settings, node_name, data_dir):
        self.get_settings = get_settings
        self.node_name = node_name
        self.data_dir = data_dir
        self.lock = threading.Lock()
        self.status = {"state": "off", "error": "", "address": "", "sent": 0, "failed": 0, "queued": 0, "last": None}
        self.router = self.src = None
        self.outbox = []          # (title, text, recipient hex, first tried at)
        self.new_devs = []        # pending "new device" lines for the next batch
        self.batch_at = 0
        self._wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="rns").start()

    # ---------------------------------------------------------------- input
    def notify(self, event, p):
        """Called with every dashboard event; picks out the ones worth a message."""
        c = self._cfg()
        if not c or not c.get("enabled"):
            return
        send = c["send"]
        if event == "det" and send["new_devices"] and p.get("is_new"):
            name = p.get("alias") or p.get("descr") or p.get("match") or "device"
            with self.lock:
                if len(self.new_devs) < 40:
                    self.new_devs.append(f"{name}  {p.get('mac')}  {p.get('rssi')} dBm at {self.node_name(p.get('closest') or p.get('node'))}")
                self.batch_at = self.batch_at or time.time() + BATCH_S
        elif event == "drone" and send["drones"] and p.get("reports") == 1:
            faa = p.get("faa") if isinstance(p.get("faa"), dict) else {}
            name = p.get("alias") or faa.get("model") or p.get("id") or p.get("mac")
            where = f"at {p['lat']:.5f}, {p['lon']:.5f}" if p.get("lat") is not None else "no position yet"
            pilot = f"\npilot at {p['plat']:.5f}, {p['plon']:.5f}" if p.get("plat") is not None else ""
            self._queue("DRONE", f"Drone {name} {where}" + (f", {round(p['alt'])} m" if p.get("alt") is not None else "")
                        + pilot + f"\nRemote ID {p.get('id') or '?'}, heard by {self.node_name(p.get('closest') or '?')}")
        elif event == "lost_node" and send["lost_nodes"]:
            self._queue("LOST NODE", f"Lost node {self.node_name(p.get('node'))} ({p.get('node')}) is back: {p.get('how')}."
                        " The fleet drops it.")
        elif event == "node_event" and send["nodes"] and p.get("event") in ("online", "offline"):
            self._queue("NODE", f"{self.node_name(p.get('id'))} ({p.get('id')}) is {p['event']}")

    def _queue(self, title, text):
        c = self._cfg()
        with self.lock:
            for r in self._recipients(c):
                self.outbox.append((title, text, r, time.time()))
            self.status["queued"] = len(self.outbox)
        self._wake.set()

    # ---------------------------------------------------------------- state
    def _cfg(self):
        try:
            return self.get_settings()
        except Exception:
            return None

    @staticmethod
    def _recipients(c):
        out = []
        for tok in (c.get("recipients") or "").replace(",", " ").split():
            tok = tok.strip().strip("<>").lower()
            if len(tok) == 32 and all(ch in "0123456789abcdef" for ch in tok):
                out.append(tok)
        return out

    def snapshot(self):
        with self.lock:
            return dict(self.status)

    def _set(self, **kw):
        with self.lock:
            self.status.update(kw)

    def reload(self):
        self._wake.set()

    # ---------------------------------------------------------------- reticulum
    @staticmethod
    def _shared_port():
        try:
            with open(os.path.expanduser("~/.reticulum/config")) as f:
                m = re.search(r"^\s*shared_instance_port\s*=\s*(\d+)", f.read(), re.M)
            return int(m.group(1)) if m else 37428
        except OSError:
            return 37428

    def _start(self, c):
        port = self._shared_port()
        try:
            socket.create_connection(("127.0.0.1", port), timeout=2).close()
        except OSError:
            raise RuntimeError(f"no shared Reticulum instance on port {port}: start rnsd first "
                               "(this dashboard never brings up the interfaces itself)")
        import RNS
        import LXMF
        # RNS installs SIGINT/SIGTERM handlers: not ours to replace (and not allowed off the
        # main thread). Skip them while it starts.
        real = signal.signal
        signal.signal = lambda *a, **k: None
        try:
            RNS.Reticulum()   # the shared instance when one runs, else our own
        finally:
            signal.signal = real
        idp = os.path.join(self.data_dir, "rns_identity")
        ident = RNS.Identity.from_file(idp) if os.path.exists(idp) else None
        if ident is None:
            ident = RNS.Identity()
            ident.to_file(idp)
            os.chmod(idp, 0o600)
        store = os.path.join(self.data_dir, "lxmf")
        os.makedirs(store, exist_ok=True)
        self.router = LXMF.LXMRouter(identity=ident, storagepath=store)
        self.src = self.router.register_delivery_identity(ident, display_name=c.get("name") or DEFAULTS["name"])
        self._set(state="ready", address=RNS.prettyhexrep(self.src.hash).strip("<>"), error="")
        self._announced = False

    def _deliver(self, title, text, rhex):
        """True: handed to LXMF. False: no path yet (asked for one). Raises when hopeless."""
        import RNS
        import LXMF
        h = bytes.fromhex(rhex)
        rid = RNS.Identity.recall(h)
        if rid is None:
            if not RNS.Transport.has_path(h):
                RNS.Transport.request_path(h)
            return False
        dest = RNS.Destination(rid, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
        msg = LXMF.LXMessage(dest, self.src, text, title, desired_method=LXMF.LXMessage.DIRECT)
        msg.register_delivery_callback(lambda m: self._set(sent=self.status["sent"] + 1, last=time.time()))
        msg.register_failed_callback(lambda m: self._set(failed=self.status["failed"] + 1))
        self.router.handle_outbound(msg)
        return True

    def _run(self):
        while True:
            self._wake.wait(5)
            self._wake.clear()
            c = self._cfg()
            if not c or not c.get("enabled"):
                if self.router is None:
                    self._set(state="off")
                continue
            try:
                if self.router is None:
                    self._set(state="starting")
                    self._start(c)
                if c.get("announce") and not self._announced:
                    self.router.announce(self.src.hash)
                    self._announced = True
                # the new-device batch
                with self.lock:
                    due = self.new_devs and time.time() >= self.batch_at
                    lines, self.new_devs = (self.new_devs, []) if due else ([], self.new_devs)
                    if due:
                        self.batch_at = 0
                if lines:
                    self._queue("NEW DEVICES", f"{len(lines)} new device{'s' if len(lines) > 1 else ''}:\n" + "\n".join(lines))
                with self.lock:
                    work, self.outbox = self.outbox, []
                keep = []
                for item in work:
                    title, text, r, t0 = item
                    if not self._deliver(title, text, r):
                        if time.time() - t0 < PATH_WAIT_S:
                            keep.append(item)
                        else:
                            self._set(failed=self.status["failed"] + 1,
                                      error=f"no path to {r[:8]}... (is it on the network and has it announced?)")
                with self.lock:
                    self.outbox = keep + self.outbox
                    self.status["queued"] = len(self.outbox)
                    if self.status["state"] != "ready":
                        self.status["state"] = "ready"
            except Exception as e:
                self._set(state="error", error=str(e)[:160])
                time.sleep(10)
