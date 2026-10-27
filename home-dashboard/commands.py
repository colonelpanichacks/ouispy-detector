"""Remote management: turns dashboard actions into home-node console lines.

One job at a time goes down the USB console. Each line waits for its answer
(CMD/OK/ERR, or RST/RFL for local reads); "mesh busy" refusals are retried.
Once the home has flooded a command (CMD {"id": cmdId}), replies from the
fleet are matched to the job: ACK lines by cmdId, RST/RFL lines by node.

Firmware lines (src/main.cpp "Remote management"):
  CMD {"op","id","target","cmd"}      accepted + flooded
  ACK {"node","cmd","id","st","n"}    st: 0 applied, 1 rejected, 2 key, 3 replay
  RST {"node", ...settings}           status reply
  RFL {"node","ok","filters":[...]}   filter list
"""
import collections
import itertools
import json
import threading
import time

import filters as flt

ACK_TEXT = {0: "applied", 1: "rejected", 2: "fleet key mismatch", 3: "replayed"}
LINE_TIMEOUT = 4.0       # s for the home to answer one console line
BUSY_RETRIES = 12        # "mesh busy" = a previous transfer is still going out
BUSY_WAIT = 1.5
REPLY_WINDOW = {"ack": 20, "rst": 12, "rfl": 30}
LABELS = {
    "status": "Read status", "audio": "Set buzzer/LED", "identify": "Identify",
    "reboot": "Reboot", "preset": "Install preset", "filters_get": "Read filters",
    "filters_set": "Push filters", "filters_add": "Add filters", "filters_del": "Remove filters",
    "rechan": "Re-channel fleet", "meshpush": "Push home filters",
    "setkey": "Set fleet key",
}


class PlanError(ValueError):
    pass


def plan(op, target, args, home_id):
    """-> (lines, expect, label). lines = [(console line, answer kind)]."""
    args = args or {}
    local = bool(home_id) and target == home_id
    tgt = "all" if target == "all" else target
    if not local and target != "all" and not (isinstance(target, str) and len(target) == 6):
        raise PlanError("target must be a node ID or all")
    label = LABELS.get(op, op)

    if op == "status":
        return ([("status", "RST:self")], "local", label) if local else \
               ([(f"rstatus {tgt}", "CMD")], "rst", label)
    if op == "audio":
        b, l = int(bool(args.get("buzzer"))), int(bool(args.get("led")))
        label = f"Buzzer {'on' if b else 'off'}, LED {'on' if l else 'off'}"
        return ([(f"audio {b} {l}", "OK")], "local", label) if local else \
               ([(f"raudio {tgt} {b} {l}", "CMD")], "ack", label)
    if op == "identify":
        secs = max(1, min(120, int(args.get("secs") or 10)))
        label = f"Identify {secs} s"
        return ([(f"ident {secs}", "OK")], "local", label) if local else \
               ([(f"rident {tgt} {secs} 3", "CMD")], "ack", label)
    if op == "reboot":
        return ([("reboot", "OK")], "local", label) if local else \
               ([(f"rreboot {tgt}", "CMD")], "ack", label)
    if op == "preset":
        name = str(args.get("name", "")).lower()
        if name not in flt.PRESETS:
            raise PlanError("unknown preset")
        label = f"Install preset {name}"
        return ([(f"preset {name}", "OK")], "local", label) if local else \
               ([(f"rpreset {tgt} {name}", "CMD")], "ack", label)
    if op == "filters_get":
        return ([("filters", "RFL:self")], "local", label) if local else \
               ([(f"rfilters {tgt}", "CMD")], "rfl", label)
    if op in ("filters_set", "filters_add", "filters_del"):
        # Replace / merge / remove, all applied live on the node. Add and
        # remove use their own console verbs: an older home answers
        # "unknown command" instead of treating them as a full replace.
        try:
            buf = flt.encode(args.get("filters") or [])
        except flt.FilterError as e:
            raise PlanError(str(e))
        verb = {"filters_set": "mfbegin", "filters_add": "mfadd", "filters_del": "mfdel"}[op]
        lines = [(f"{verb} {'self' if local else tgt}", "OK")]
        lines += [(f"mfdata {h}", "OK") for h in flt.chunks(buf)]
        lines.append(("mfend", "OK" if local else "CMD"))
        recs = flt.decode(buf)
        n = len(recs)
        what = f"{recs[0]['t']} {recs[0]['id']}" if n == 1 else f"{n} filters"
        label = {"filters_set": f"Push {n} filters", "filters_add": f"Add {what}",
                 "filters_del": f"Remove {what}"}[op]
        return lines, "local" if local else "ack", label
    if op == "rechan":
        ch = int(args.get("ch") or 0)
        if not 1 <= ch <= 11:
            raise PlanError("channel must be 1-11")
        return [(f"meshrechan {ch}", "OK")], "local", f"Re-channel fleet to {ch}"
    if op == "meshpush":
        return [("meshpush", "CMD")], "ack", label
    if op == "setkey":
        # USB console of the home ONLY: a key never crosses the radio.
        if not local:
            raise PlanError("the fleet key is only ever set over USB (or the node's setup portal), never over the mesh")
        k = str(args.get("key", ""))
        if not 8 <= len(k) <= 63 or " " in k:
            raise PlanError("invalid fleet key")
        return [(f"meshkey {k}", "OK")], "local", "Set fleet key on home"
    raise PlanError(f"unknown op {op!r}")


class Commander(threading.Thread):
    def __init__(self, fleet, store, emit, get_sender, get_settings):
        super().__init__(daemon=True)
        self.fleet, self.store, self.emit = fleet, store, emit
        self.get_sender, self.get_settings = get_sender, get_settings
        self.queue = collections.deque()
        self.cv = threading.Condition()
        self.answers = collections.deque()       # console answers for the line in flight
        self.waiting = []                        # jobs sent, collecting fleet replies
        self.jobs = collections.OrderedDict()    # recent jobs by id (newest last)
        self.ids = itertools.count(int(time.time()) % 100000)
        self.last_poll = 0.0
        self.join_polls = {}                     # node -> due time for a status read
        self.filter_polls = {}                   # node or "all" -> due time for a filter-list read

    # ------------------------------------------------------------------
    # public
    # ------------------------------------------------------------------
    def submit(self, op, target, args=None, auto=False):
        home = self.fleet.home_id
        lines, expect, label = plan(op, target, args, home)
        if not self.get_sender():
            raise PlanError("no USB connection to the home node: remote settings need the serial feed")
        if target == "all" and not auto and op not in ("rechan",) and not any(
                n["online"] and not n["is_home"] for n in self.fleet.snapshot()["nodes"]):
            raise PlanError("no remotes are in range of the home right now, so nothing would receive it")
        job = {"id": next(self.ids), "op": op, "target": target, "label": label,
               "expect": expect, "lines": [l for l, _ in lines], "kinds": [k for _, k in lines],
               "state": "queued", "cmd_id": None, "replies": {}, "error": "",
               "created": time.time(), "finished": None, "auto": auto,
               "args": {} if op == "setkey" else (args or {})}   # a key is never kept in a job
        self._remember(job)
        with self.cv:
            self.queue.append(job)
            self.cv.notify_all()
        self._publish(job)
        return self.public(job)

    def node_joined(self, nid):
        """Read a node's settings shortly after it (re)joins the mesh."""
        self.join_polls[nid] = time.time() + 4

    def recent(self, node=None, limit=60):
        jobs = list(self.jobs.values())[::-1]
        if node:
            jobs = [j for j in jobs if j["target"] in (node, "all")]
        return [self.public(j) for j in jobs[:limit]]

    @staticmethod
    def public(job):
        return {k: v for k, v in job.items() if k not in ("lines", "kinds")} | \
               {"steps": len(job["lines"])}   # lines may hold a key (setkey): never published

    def feed_line(self, line):
        """Every console line from the home; called by Fleet."""
        head = line.split(" ", 1)[0]
        if head in ("OK", "ERR", "CMD", "RST", "RFL"):
            with self.cv:
                self.answers.append(line)
                self.cv.notify_all()
        if head in ("ACK", "RST", "RFL"):
            try:
                obj = json.loads(line.split(" ", 1)[1])
            except (ValueError, IndexError):
                return
            self._match_reply(head, obj)

    # ------------------------------------------------------------------
    # worker
    # ------------------------------------------------------------------
    def run(self):
        while True:
            with self.cv:
                if not self.queue:
                    self.cv.wait(1.0)
                job = self.queue.popleft() if self.queue else None
            if job:
                self._execute(job)
            self._sweep()
            self._auto_poll()

    def _execute(self, job):
        sender = self.get_sender()
        if not sender:
            return self._finish(job, "failed", "home node disconnected")
        job["state"] = "sending"
        self._publish(job)
        for i, (line, kind) in enumerate(zip(job["lines"], job["kinds"])):
            for attempt in range(BUSY_RETRIES):
                with self.cv:
                    self.answers.clear()
                if not sender.send(line):
                    return self._finish(job, "failed", "serial write failed")
                ans = self._await(kind)
                if ans is None:
                    return self._finish(job, "failed", f"no answer from the home to '{line.split(' ')[0]}'")
                if ans.startswith("ERR"):
                    if "busy" in ans and attempt < BUSY_RETRIES - 1:
                        time.sleep(BUSY_WAIT)
                        continue
                    return self._finish(job, "failed", ans[4:])
                if ans.startswith("CMD "):
                    try:
                        job["cmd_id"] = json.loads(ans[4:]).get("id")
                    except ValueError:
                        pass
                if ans.startswith(("RST ", "RFL ")):
                    self._match_reply(ans[:3], json.loads(ans[4:]))
                break
        if job["expect"] == "local":
            return self._finish(job, "done")
        job["state"] = "sent"
        job["deadline"] = time.time() + REPLY_WINDOW[job["expect"]]
        if job["target"] == "all":
            # Settle early once every remote online right now has answered.
            job["expected"] = sorted(n["id"] for n in self.fleet.snapshot()["nodes"]
                                     if n["online"] and not n["is_home"])
        self.waiting.append(job)
        self._publish(job)

    def _await(self, kind):
        """Wait for the answer to the line in flight."""
        deadline = time.time() + LINE_TIMEOUT
        with self.cv:
            while True:
                while self.answers:
                    a = self.answers.popleft()
                    if a.startswith("ERR"):
                        return a
                    if kind == "CMD" and a.startswith("CMD "):
                        return a
                    if kind == "OK" and (a.startswith("OK") or a.startswith("CMD ")):
                        return a
                    if kind in ("RST:self", "RFL:self") and a.startswith(kind[:3] + " "):
                        try:
                            if json.loads(a[4:]).get("node") == self.fleet.home_id:
                                return a
                        except ValueError:
                            pass
                left = deadline - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def _match_reply(self, kind, obj):
        node = obj.get("node")
        for job in list(self.waiting):
            if job["target"] not in ("all", node):
                continue
            if job["target"] == "all" and node == self.fleet.home_id:
                continue   # fleet-wide commands go to the remotes, not the home
            if kind == "ACK" and job["expect"] == "ack" and obj.get("id") == job["cmd_id"]:
                st = obj.get("st", 1)
                job["replies"][node] = {"st": st, "text": ACK_TEXT.get(st, f"status {st}"),
                                        "n": obj.get("n"), "ts": time.time()}
            elif kind == "RST" and job["expect"] == "rst":
                job["replies"][node] = {"st": 0, "text": "status received", "ts": time.time()}
            elif kind == "RFL" and job["expect"] == "rfl":
                job["replies"][node] = {"st": 0 if obj.get("ok") else 1,
                                        "text": f"{len(obj.get('filters', []))} filters",
                                        "ts": time.time()}
            else:
                continue
            if job["target"] == node or (job["target"] == "all" and job.get("expected") and
                                         set(job["expected"]) <= set(job["replies"])):
                self._settle(job)
            else:
                self._publish(job)

    def _sweep(self):
        now = time.time()
        for job in list(self.waiting):
            if now >= job.get("deadline", now):
                self._settle(job)

    def _settle(self, job):
        if job in self.waiting:
            self.waiting.remove(job)
        r = job["replies"]
        if not r and job["op"] in ("status", "filters_get") and not job.get("retried"):
            # Reads are harmless to repeat: give a lost exchange one more go.
            job.update(state="queued", replies={}, cmd_id=None, retried=True)
            with self.cv:
                self.queue.append(job)
                self.cv.notify_all()
            return self._publish(job)
        if not r:
            return self._finish(job, "failed",
                                "no reply over the mesh: the command or its answer was lost, or the node "
                                "is offline. Try again." if job["target"] != "all" else
                                "no node replied over the mesh. Try again.")
        bad = [n for n, x in r.items() if x["st"] != 0]
        if job["target"] != "all":
            x = r.get(job["target"])
            state = "done" if x and x["st"] == 0 else "failed"
            return self._finish(job, state, "" if state == "done" else (x["text"] if x else "no reply"))
        self._finish(job, "partial" if bad else "done",
                     f"{len(bad)} of {len(r)} refused" if bad else "")

    def _finish(self, job, state, error=""):
        job["state"], job["error"], job["finished"] = state, error, time.time()
        self.store.save_command(job)
        self._publish(job)
        if state in ("done", "partial") and not job["auto"]:
            self._refresh_after(job)

    def _refresh_after(self, job):
        """Re-read settings a change touched, so the drawer shows the result."""
        op, tgt = job["op"], job["target"]
        if op in ("audio", "setkey"):
            self.join_polls[tgt] = time.time() + 1.5
        elif op in ("filters_set", "filters_add", "filters_del", "preset", "meshpush"):
            # Applied live (no reboot): read the new list and its CRC back.
            who = "all" if op == "meshpush" else tgt
            self.join_polls[who] = time.time() + 2
            self.filter_polls[who] = time.time() + 3
        elif op in ("reboot", "rechan") and tgt == self.fleet.home_id:
            self.join_polls[tgt] = time.time() + 12   # home is back on USB by then
        # Rebooted remotes are read again when they rejoin (node_joined).

    def _auto_poll(self):
        if not self.get_sender() or not self.fleet.home_id:
            return
        now = time.time()
        for nid, due in list(self.join_polls.items()):
            if now >= due:
                del self.join_polls[nid]
                self._quiet("status", nid)
        for nid, due in list(self.filter_polls.items()):
            if now >= due:
                del self.filter_polls[nid]
                self._quiet("filters_get", nid)
        every = int(self.get_settings().get("status_poll") or 0)
        if every and now - self.last_poll >= every and not self.queue:
            self.last_poll = now
            self._quiet("status", self.fleet.home_id)
            if any(n["online"] and not n["is_home"] for n in self.fleet.snapshot()["nodes"]):
                self._quiet("status", "all")

    def _quiet(self, op, target):
        try:
            self.submit(op, target, auto=True)
        except PlanError:
            pass

    # ------------------------------------------------------------------
    def _remember(self, job):
        self.jobs[job["id"]] = job
        while len(self.jobs) > 200:
            self.jobs.popitem(last=False)

    def _publish(self, job):
        self.emit("job", self.public(job))
