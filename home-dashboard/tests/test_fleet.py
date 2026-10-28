"""Parser + store tests: python -m unittest discover -s tests (from home-dashboard/)."""
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fleet import Fleet  # noqa: E402
from store import Store  # noqa: E402

TLM = ('TLM {"v":1,"id":"A1B2C3","role":"home","mode":"scan","mesh":1,"ch":6,"up":120,'
       '"heap":150000,"dev":2,"filters":8,"n":2,"rx":40,"q":30,"dup":10,"self":0,"bad":0,'
       '"tx":20,"txf":1,"txcb":0,"pend":0,"lrssi":-61,"heal":0,"mqtt":0,"nodes":['
       '{"id":"D4E5F6","role":"remote","last":1,"pkts":33,"rssi":-61},'
       '{"id":"7A8B9C","role":"remote","last":0,"pkts":50,"rssi":-70}]}')
TLM_ONE = TLM.replace(',{"id":"7A8B9C","role":"remote","last":0,"pkts":50,"rssi":-70}', "")


class FleetTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.tmp.name, "t.db"))
        self.events = []
        self.fleet = Fleet(self.store, lambda e, p: self.events.append((e, p)))

    def tearDown(self):
        self.tmp.cleanup()

    def nodes(self):
        return {n["id"]: n for n in self.fleet.snapshot()["nodes"]}

    def test_tlm_roster_and_home(self):
        self.fleet.feed_line(TLM)
        snap = self.fleet.snapshot()
        self.assertEqual(snap["home"]["id"], "A1B2C3")
        self.assertTrue(snap["home"]["alive"])
        self.assertEqual(snap["home"]["txf"], 1)
        n = self.nodes()
        self.assertTrue(n["A1B2C3"]["is_home"])
        self.assertEqual(n["D4E5F6"]["rssi"], -61)
        self.assertEqual(n["7A8B9C"]["pkts"], 50)
        self.assertTrue(all(x["online"] for x in n.values()))

    def test_tlm_drops_missing_node(self):
        self.fleet.feed_line(TLM)
        self.fleet.feed_line(TLM_ONE)
        n = self.nodes()
        self.assertFalse(n["7A8B9C"]["online"])
        self.assertTrue(n["D4E5F6"]["online"])
        self.assertIn(("node_event", {"id": "7A8B9C", "event": "offline"}),
                      [(e, {k: p[k] for k in ("id", "event")}) for e, p in self.events
                       if e == "node_event"])

    def test_detection_mesh_suffix_and_node_field(self):
        self.fleet.feed_line(TLM)
        self.fleet.feed_line('{"mac":"00:25:df:11:22:33","alias":"","rssi":-66,"type":"OUI",'
                             '"match":"0025DF","desc":"AXON [mesh D4E5F6]"}')
        self.fleet.feed_line('DET {"mac":"00:25:DF:44:55:66","rssi":-50,"type":"PROBE",'
                             '"match":"0025DF","desc":"AXON","node":"A1B2C3","dom":"wifi"}')
        _, rows = self.store.query_detections()
        by_mac = {r["mac"]: r for r in rows}
        a = by_mac["00:25:DF:11:22:33"]
        self.assertEqual((a["node"], a["descr"], a["dom"], a["is_new"]), ("D4E5F6", "AXON", "ble", 1))
        b = by_mac["00:25:DF:44:55:66"]
        self.assertEqual((b["node"], b["dom"]), ("A1B2C3", "wifi"))

    def test_seen_by_and_closest(self):
        self.fleet.feed_line(TLM)
        def det(node, rssi):
            self.fleet.feed_detection({"mac": "00:13:37:AA:BB:CC", "rssi": rssi, "type": "BEACON",
                                       "desc": "Pineapple", "node": node, "dom": "wifi"})
            return [p for e, p in self.events if e == "det"][-1]
        d = det("A1B2C3", -70)
        self.assertEqual((d["seen_by"], d["closest"]), ([["A1B2C3", -70, 0]], None))   # one node: no claim
        d = det("D4E5F6", -48)
        self.assertEqual([s[:2] for s in d["seen_by"]], [["D4E5F6", -48], ["A1B2C3", -70]])
        self.assertEqual(d["closest"], "D4E5F6")
        # A node's repeat inside the dedup window still updates its signal.
        self.fleet.feed_detection({"mac": "00:13:37:AA:BB:CC", "rssi": -40, "type": "BEACON",
                                   "desc": "Pineapple", "node": "A1B2C3", "dom": "wifi"})
        self.assertEqual(self.fleet.sightings["00:13:37:AA:BB:CC"]["A1B2C3"][0], -40)

    def test_signal_update_moves_target_not_feed(self):
        self.fleet.feed_line(TLM)
        self.fleet.feed_line('{"mac":"00:13:37:AA:BB:CC","rssi":-60,"type":"BEACON","match":"001337",'
                             '"desc":"Pineapple","node":"A1B2C3","dom":"wifi"}')
        self.fleet.feed_line('{"mac":"00:13:37:AA:BB:CC","rssi":-41,"type":"BEACON","match":"001337",'
                             '"desc":"Pineapple","node":"D4E5F6","dom":"wifi","upd":1}')
        self.assertEqual(self.store.query_detections()[0], 1)                 # no row for the update
        sight = [p for e, p in self.events if e == "sighting"]
        self.assertEqual((len(sight), sight[0]["node"], sight[0]["seen_by"][0][:2]), (1, "D4E5F6", ["D4E5F6", -41]))

    def test_dedup_across_sources(self):
        line = '{"mac":"AA:BB:CC:DD:EE:FF","rssi":-60,"type":"OUI","match":"AABBCC","desc":"X"}'
        self.fleet.feed_line(line, "serial")
        self.fleet.feed_detection({"mac": "AA:BB:CC:DD:EE:FF", "rssi": -60, "type": "OUI",
                                   "desc": "X"}, "mqtt")
        self.assertEqual(self.store.query_detections()[0], 1)

    def test_repeat_mac_not_new(self):
        for i in range(2):
            self.fleet.recent.clear()
            self.fleet.feed_line('{"mac":"AA:BB:CC:00:00:01","rssi":-60,"type":"OUI","desc":"X"}')
        rows = self.store.query_detections()[1]
        self.assertEqual(sorted(r["is_new"] for r in rows), [0, 1])

    def test_legacy_lines(self):
        self.fleet.feed_line("[MESH] ESP-NOW LR up: HOME node, channel 6, id A1B2C3")
        self.fleet.feed_line("[MESH] node D4E5F6 joined (remote)")
        self.fleet.feed_line("[HEARTBEAT] phase=1 ch=6 dev=2 mesh=1 rx=50 q=40 dup=10 self=0 "
                             "bad=0 tx=25/1/0 led=0 flag=1 idle=100 lrssi=-62")
        snap = self.fleet.snapshot()
        self.assertTrue(snap["home"]["legacy"])
        self.assertEqual((snap["home"]["rx"], snap["home"]["lrssi"], snap["home"]["ch"]), (50, -62, 6))
        self.assertTrue(self.nodes()["D4E5F6"]["online"])
        self.fleet.feed_line("[MESH] node D4E5F6 offline (bye)")
        self.assertFalse(self.nodes()["D4E5F6"]["online"])

    def test_heartbeat_ignored_when_tlm_present(self):
        self.fleet.feed_line(TLM)
        self.fleet.feed_line("[HEARTBEAT] phase=1 ch=6 dev=2 mesh=1 rx=999 q=40 dup=10 self=0 "
                             "bad=0 tx=25/1/0 led=0 flag=1 idle=100 lrssi=-62")
        snap = self.fleet.snapshot()
        self.assertFalse(snap["home"]["legacy"])
        self.assertEqual(snap["home"]["rx"], 40)

    def test_garbage_lines_are_harmless(self):
        for line in ["", "TLM {broken", '{"mac":', "random log", "\x00\xff", "DET nope"]:
            self.fleet.feed_line(line)
        self.assertEqual(self.store.query_detections()[0], 0)


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.tmp.name, "t.db"))

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, mac, node="A1B2C3", ts=None, **kw):
        d = {"ts": ts or time.time(), "node": node, "mac": mac, "rssi": -60, "type": "OUI",
             "match": mac[:8], "descr": "AXON", "dom": "ble", "source": "test"}
        d.update(kw)
        self.store.add_detection(d)

    def test_devices_and_alias_search(self):
        self.add("00:25:DF:00:00:01", node="D4E5F6")
        self.add("00:25:DF:00:00:01", node="7A8B9C", rssi=-50)
        self.add("B4:1E:52:00:00:02", descr="FLOCK")
        total, rows = self.store.devices()
        self.assertEqual(total, 2)
        d = {r["mac"]: r for r in rows}["00:25:DF:00:00:01"]
        self.assertEqual((d["hits"], d["best"], d["nodes"]), (2, -50, ["7A8B9C", "D4E5F6"]))
        self.store.set_mac_alias("B4:1E:52:00:00:02", "Lot camera")
        self.assertEqual(self.store.query_detections(q="lot cam")[0], 1)
        self.assertIn("Lot camera", self.store.export_detections("csv"))
        self.store.set_mac_alias("B4:1E:52:00:00:02", "")
        self.assertEqual(self.store.mac_aliases(), {})

    def test_clear_one_node(self):
        self.add("AA:00:00:00:00:01", node="D4E5F6")
        self.add("AA:00:00:00:00:02", node="D4E5F6")
        self.add("AA:00:00:00:00:03", node="7A8B9C")
        self.assertEqual(self.store.clear_detections("D4E5F6"), 2)
        self.assertEqual(self.store.detection_stats()["per_node"], {"7A8B9C": {"count": 1, "last": self.store.detection_stats()["per_node"]["7A8B9C"]["last"]}})
        self.add("BB:00:00:00:00:09", node="7A8B9C")
        self.add("BB:00:00:00:00:09", node="D4E5F6")
        self.assertEqual(self.store.clear_detections(mac="BB:00:00:00:00:09"), 2)
        self.assertEqual(self.store.clear_detections(), 1)
        self.assertEqual(self.store.query_detections()[0], 0)

    def test_hourly_and_prune(self):
        now = time.time()
        self.add("AA:00:00:00:00:01", ts=now)
        self.add("AA:00:00:00:00:02", ts=now - 7200)
        self.add("AA:00:00:00:00:03", ts=now - 10 * 86400)
        hrs = self.store.hourly()
        self.assertEqual(len(hrs), 24)
        self.assertEqual(sum(h["c"] for h in hrs), 2)
        self.assertEqual(hrs[-1]["c"], 1)
        self.assertEqual(self.store.prune(7), 1)
        self.assertEqual(self.store.query_detections()[0], 2)
        self.assertEqual(self.store.prune(0), 0)


if __name__ == "__main__":
    unittest.main()
