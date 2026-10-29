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
import re
import itertools
import json
import threading
import time

import filters as flt
import firmware as fw

ACK_TEXT = {0: "applied", 1: "rejected", 2: "fleet key mismatch", 3: "replayed"}
LINE_TIMEOUT = 4.0       # s for the home to answer one console line
BUSY_RETRIES = 12        # "mesh busy" = a previous transfer is still going out
BUSY_WAIT = 1.5
REPLY_WINDOW = {"ack": 20, "rst": 12, "rfl": 30, "ota": 2400}
LABELS = {
    "status": "Read status", "alias": "Name node", "audio": "Set buzzer/LED", "identify": "Identify",
    "reboot": "Reboot", "preset": "Install preset", "filters_get": "Read filters",
    "filters_set": "Push filters", "filters_add": "Add filters", "filters_del": "Remove filters",
    "rechan": "Re-channel fleet", "meshpush": "Push home filters",
    "setkey": "Set fleet key", "wardrive": "Wardriver settings", "locate": "Locate beep",
    "fw_install": "Install firmware",
    "fw_stage": "Stage firmware on home", "fw_send": "Update firmware", "fw_self": "Update home firmware",
}


# Console verbs that prefix their own ERR lines ("ERR rstatus: ..."). A
# line's answer may also come under the mesh op it floods (mfend -> filters
# / fadd / fdel).
KNOWN_VERBS = {"nodealias", "ralias", "status", "filters", "audio", "ident", "reboot", "rstatus", "rfilters", "raudio",
               "rreboot", "rident", "rpreset", "mfbegin", "mfadd", "mfdel", "mfdata", "mfend",
               "fadd", "fdel", "meshpush", "meshrechan", "fwbegin", "fwdata", "fwend", "fwsend",
               "fwself", "fwabort", "preset", "meshkey", "rwd", "wd", "locate", "rlocate"}
ERR_VERBS = {"mfend": {"mfend", "filters", "fadd", "fdel"}}


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
    if op == "alias":
        # The node keeps its own name (NVS), so its status, its web UI and a
        # display on its USB all show what the dashboard calls it.
        a = str(args.get("alias") or "").strip()[:32]
        if any(c in a for c in '"\\') or any(not (0x20 <= ord(c) < 0x7f) for c in a):
            raise PlanError("node names are plain ASCII without quotes or backslashes")
        label = f"Name node {a!r}" if a else "Clear node name"
        return ([(f"nodealias {a or '-'}", "OK")], "local", label) if local else \
               ([(f"ralias {tgt} {a or '-'}", "CMD")], "ack", label)
    if op == "locate":
        # 0 stop, 1 one triple beep, 2 triple beeps until stopped (find a node by ear).
        mode = max(0, min(2, int(args.get("mode", 1))))
        label = ["Stop locate beep", "Beep 3 times", "Locate beep on"][mode]
        return ([(f"locate {mode}", "OK")], "local", label) if local else \
               ([(f"rlocate {tgt} {mode}", "CMD")], "ack", label)
    if op == "wardrive":
        # A wardriver node's switches (src/wardrive.h): on / off, raw packets, channel plan.
        words = []
        if "on" in args:
            words.append("on" if args["on"] else "off")
        if "raw" in args:
            words.append("raw on" if args["raw"] else "raw off")
        if args.get("ch"):
            ch = str(args["ch"]).strip().replace(" ", "")
            if not re.fullmatch(r"all|(\d{1,2}(-\d{1,2})?)(,\d{1,2}(-\d{1,2})?)*", ch):
                raise PlanError("channels: all, 6, 1-6, 1,6,11 or 1-3,11")
            words.append("ch " + ch)
        if not words:
            raise PlanError("nothing to change")
        label = "Wardriver: " + ", ".join(words)
        if local:
            return ([(f"wd {w}", "OK") for w in words], "local", label)
        return ([(f"rwd {tgt} {w}", "CMD") for w in words], "ack", label)
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
    if op == "fw_stage":
        # Lines are generated while it runs (2,500+ of them): see _stage_firmware.
        if not local:
            raise PlanError("firmware is staged on the home node, over USB")
        if args.get("kind"):
            if args["kind"] not in fw.library():
                raise PlanError(f"no {args['kind']} build in this checkout")
            return [], "local", f"Stage {args['kind']} firmware on home"
        meta = fw.current()
        if not meta:
            raise PlanError("upload a firmware image first")
        return [], "local", f"Stage firmware {meta.get('build') or meta['date']} on home"
    if op == "fw_send":
        if local or target == "all":
            raise PlanError("pick one remote node (the home updates itself with fw_self)")
        return [(f"fwsend {tgt}", "CMD")], "ota", (f"Install {args['kind']} firmware" if args.get("kind") else "Update firmware")
    if op == "fw_self":
        if not local:
            raise PlanError("fw_self is for the home node")
        return [("fwself", "OK")], "local", label
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
        self.list_reads = {}                     # node -> last automatic filter-list read

    # ------------------------------------------------------------------
    # public
    # ------------------------------------------------------------------
    def submit(self, op, target, args=None, auto=False):
        # A list bigger than one transfer goes out in parts: a replace sends
        # its first part as the replace and the rest as adds; adds and
        # removes are simply batched. Each part is its own job, in order.
        if op in ("filters_set", "filters_add", "filters_del") and (args or {}).get("filters"):
            try:
                parts = flt.split_for_transfer(args["filters"])
            except flt.FilterError as e:
                raise PlanError(str(e))
            if len(parts) > 1:
                jobs = [self.submit(op if i == 0 or op != "filters_set" else "filters_add", target,
                                    {**args, "filters": part}, auto) for i, part in enumerate(parts)]
                return jobs[-1]
        home = self.fleet.home_id
        if op == "fw_install":
            # Any of the three firmwares onto any node: stage that build on the home,
            # then send it over the mesh (or the home updates itself). Each job loads its
            # kind's image when it runs, so queued installs of different kinds can't mix.
            kind = (args or {}).get("kind")
            if kind not in fw.library():
                raise PlanError(f"no {kind} build in this checkout (build it first)")
            self.submit("fw_stage", home, {"kind": kind}, auto)
            if target == home:
                return self.submit("fw_self", home, {"kind": kind, "convert": True}, auto)
            return self.submit("fw_send", target, {"kind": kind, "convert": True}, auto)
        if op in ("fw_send", "fw_self") and not (args or {}).get("kind"):
            # Never turn a detector into a Sky-Spy node (or back) by accident.
            meta = fw.current() or {}
            node = (self.fleet.nodes.get(target) or {}) if hasattr(self.fleet, "nodes") else {}
            have = fw.node_kind(((node.get("cfg") or {}).get("fw")))
            want = meta.get("kind", "detector")
            if want != have and not (args or {}).get("convert"):
                raise PlanError(f"the uploaded image is a {want} build but {target} is a {have} node "
                                f"(upload the {have} build, or convert it on purpose)")
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
        if op == "filters_get" and target not in ("all", home) and not (args or {}).get("force"):
            # The node's fcrc says exactly which list it holds: when we already
            # have a list with that CRC (the home's, read over USB in a blink,
            # or this node's last read), show it without a mesh round trip.
            hit = self.fleet.filters_by_crc(target)
            if hit:
                lst, src = hit
                self.fleet._node_filters({"node": target, "ok": 1, "filters": lst},
                                         via=None if src == target else src)
                job["replies"][target] = {"st": 0, "ts": time.time(),
                                          "text": f"{len(lst)} filters (CRC verified, no mesh read needed)"}
                self._finish(job, "done")
                return self.public(job)
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
        if head == "OTA":
            self._ota_line(line)
            return
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
        if job["op"] == "fw_stage":
            return self._stage_firmware(job, sender)
        for i, (line, kind) in enumerate(zip(job["lines"], job["kinds"])):
            for attempt in range(BUSY_RETRIES):
                with self.cv:
                    self.answers.clear()
                if not sender.send(line):
                    return self._finish(job, "failed", "serial write failed")
                ans = self._await(kind, line)
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

    def _await_fw(self, timeout):
        """Next answer to a firmware staging line: "OK fw..." or "ERR fw..."
        (other commands' lines are skipped)."""
        deadline = time.time() + timeout
        with self.cv:
            while True:
                while self.answers:
                    a = self.answers.popleft()
                    if a.startswith(("OK fw", "ERR fw")):
                        return a
                left = deadline - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def _stage_firmware(self, job, sender):
        """Copy the uploaded image into the home's PSRAM, then fwend writes
        it to its spare slot. Each fwdata line carries its offset and every
        reply names the byte count, so a lost line or reply is resent and a
        stale reply can't be mistaken for the current one."""
        try:
            kind = (job.get("args") or {}).get("kind")
            img, meta = fw.load_kind(kind) if kind else fw.load()
        except fw.FirmwareError as e:
            return self._finish(job, "failed", str(e))
        with self.cv:
            self.answers.clear()
        sender.send(f"fwbegin {meta['size']} {meta['sha256']}")
        a = self._await_fw(15)
        if not a or not a.startswith("OK fwbegin"):
            return self._finish(job, "failed", (a or "no answer to fwbegin")[4:] if a else "no answer to fwbegin")
        off, misses, last_pub = 0, 0, 0.0
        while off < len(img):
            end = min(off + fw.CHUNK, len(img))
            sender.send(f"fwdata {off} {img[off:end].hex()}")
            got = None
            deadline = time.time() + 5
            while time.time() < deadline:
                a = self._await_fw(deadline - time.time())
                if a is None:
                    break
                m = re.match(r"OK fwdata (\d+)$", a)
                if m and int(m.group(1)) == end:
                    got = end
                    break
                m = re.search(r"expected offset (\d+)", a)
                if m:
                    got = int(m.group(1))
                    break
                if a.startswith("ERR fwdata") and "bad hex" not in a:
                    return self._finish(job, "failed", a[4:])
            if got is None:
                misses += 1
                if misses > 20:
                    return self._finish(job, "failed", f"the home stopped answering at byte {off}")
                continue
            off = got
            if time.time() - last_pub > 0.5:
                job["progress"] = int(off * 100 / len(img))
                self._publish(job)
                last_pub = time.time()
        job["progress"] = 100
        self._publish(job)
        sender.send("fwend")
        a = self._await_fw(90)
        if not a or not a.startswith("OK fwend"):
            return self._finish(job, "failed", a[4:] if a else "no answer to fwend")
        return self._finish(job, "done")

    def _await(self, kind, line=""):
        """Wait for the answer to the line in flight. An ERR naming another
        console command (a late "ERR rstatus: mesh busy" from an earlier
        poll) is not this line's answer: taking it as one re-sent upload
        lines and broke filter pushes."""
        verb = line.split(" ", 1)[0]
        mine = ERR_VERBS.get(verb, {verb})
        deadline = time.time() + LINE_TIMEOUT
        with self.cv:
            while True:
                while self.answers:
                    a = self.answers.popleft()
                    if a.startswith("ERR"):
                        named = a[4:].split(":", 1)[0].strip() if ":" in a else ""
                        if named in KNOWN_VERBS and named not in mine:
                            continue
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

    def _ota_line(self, line):
        """OTA {node, state, pct, ...}: progress of an update the home is sending."""
        try:
            obj = json.loads(line[4:])
        except ValueError:
            return
        node = obj.get("node")
        for job in list(self.waiting):
            if job["expect"] != "ota" or job["target"] != node:
                continue
            job["progress"] = obj.get("pct", job.get("progress", 0))
            job["ota"] = {k: obj.get(k) for k in ("state", "bytes", "size", "kbps", "direct", "err")}
            st = obj.get("state")
            if st == "done":
                job["replies"][node] = {"st": 0, "text": "updated, rebooting into it", "ts": time.time()}
                self._settle(job)
            elif st == "failed":
                job["replies"][node] = {"st": 1, "text": obj.get("err") or "update failed", "ts": time.time()}
                self._settle(job)
            else:
                self._publish(job)

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
            self.fleet.forget_fcrc(who)   # until the next RST: no CRC shortcut to the old list
            self.join_polls[who] = time.time() + 2
            self.filter_polls[who] = time.time() + 3
        elif op in ("reboot", "rechan") and tgt == self.fleet.home_id:
            self.join_polls[tgt] = time.time() + 12   # home is back on USB by then
        # Rebooted remotes are read again when they rejoin (node_joined).

    def _keep_lists_current(self, now):
        """Every drawer shows the node's real list without anyone pressing Read:
        the home is re-read over USB whenever its fcrc moves, and a remote whose
        fcrc matches a list we hold gets that list (CRC-verified, no radio)."""
        home = self.fleet.home_id
        nodes = self.fleet.snapshot()["nodes"]
        for n in nodes:
            nid = n["id"]
            if not (n.get("cfg") or {}).get("fcrc") or self.fleet.filters_current(nid):
                continue
            if nid == home:
                if now - self.list_reads.get(nid, 0) >= 20:
                    self.list_reads[nid] = now
                    self._quiet("filters_get", nid)
                continue
            hit = self.fleet.filters_by_crc(nid)
            if hit:
                lst, src = hit
                self.fleet._node_filters({"node": nid, "ok": 1, "filters": lst},
                                         via=None if src == nid else src)

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
        self._keep_lists_current(now)
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
