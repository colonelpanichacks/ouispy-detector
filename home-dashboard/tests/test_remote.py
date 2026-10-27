"""Remote management: filter wire format, command planning, and a full
commander round trip against the demo fleet's console simulator."""
import os
import sys
import tempfile
import time
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import commands  # noqa: E402
import filters as flt  # noqa: E402
from fleet import Fleet  # noqa: E402
from sources import DEMO_HOME, DemoSource  # noqa: E402
from store import Store  # noqa: E402


class FilterTest(unittest.TestCase):
    def test_roundtrip_and_normalize(self):
        lst = [{"t": "OUI", "id": "00:25:df", "d": "Axon"},
               {"t": "CID", "id": "0x034d", "d": "Taser"},
               {"t": "NAME", "id": "Axon", "d": ""},
               {"t": "MAC", "id": "00-25-DF-11-22-33", "d": "one cam"},
               {"t": "META", "id": "0x0D53+0xFD5F", "d": "meta"}]
        buf = flt.encode(lst)
        back = flt.decode(buf)
        self.assertEqual([(f["t"], f["id"]) for f in back],
                         [("OUI", "0025DF"), ("CID", "034D"), ("NAME", "Axon"),
                          ("MAC", "0025DF112233"), ("META", "0x0D53+0xFD5F")])
        self.assertEqual(back[3]["full"], 1)
        # Record layout: type, flags, idLen, descLen, id, desc
        self.assertEqual(buf[:4], bytes([0, 0, 6, 4]))
        self.assertEqual(flt.crc(lst), f"{zlib.crc32(buf):08X}")

    def test_rejects_bad_identifiers(self):
        for bad in ({"t": "OUI", "id": "0025D"}, {"t": "CID", "id": "34D"},
                    {"t": "SVC", "id": "XYZW"}, {"t": "NAME", "id": ""},
                    {"t": "NAME", "id": "x" * 17}, {"t": "WAT", "id": "00"}):
            with self.assertRaises(flt.FilterError, msg=bad):
                flt.encode([bad])
        with self.assertRaises(flt.FilterError):
            flt.encode([])

    def test_dedupe_and_empty_crc(self):
        lst = [{"t": "OUI", "id": "0025DF"}, {"t": "OUI", "id": "00:25:DF"}]
        self.assertEqual(len(flt.decode(flt.encode(lst))), 1)
        self.assertEqual(flt.crc([]), "00000000")

    def test_chunks_fit_console(self):
        buf = flt.encode([{"t": "NAME", "id": f"n{i:02d}", "d": "x" * 40} for i in range(60)])
        lines = flt.chunks(buf)
        self.assertTrue(all(len("mfdata " + l) < 250 for l in lines))
        self.assertEqual(bytes.fromhex("".join(lines)), buf)


class VendorDbTest(unittest.TestCase):
    """The portal's OUI database (BLEOUIs.md / WiFiOUIs.md in the repo)."""

    def test_parsed_like_the_portal(self):
        db = flt.vendor_db()
        labels = [g["label"] for g in db]
        self.assertTrue(any(l.startswith("FLOCK SAFETY") for l in labels))
        self.assertIn("ble", {g["kind"] for g in db})
        for g in db:
            if g["kind"] == "extra":          # non-OUI signatures (names, ...)
                self.assertTrue(g["rows"])
                continue
            self.assertTrue(g["ouis"])
            self.assertTrue(all(len(o) == 6 for o in g["ouis"]))

    def test_filter_rules(self):
        ble = {"label": "X", "kind": "ble", "ouis": ["AABBCC"], "meta": {}}
        self.assertEqual([r["t"] for r in flt.vendor_filters(ble)], ["OUI", "PROBE"])
        self.assertEqual([r["t"] for r in flt.vendor_filters(ble, wifi=False)], ["OUI"])
        wifi = {"label": "Y", "kind": "wifi", "ouis": ["AABBCC"], "meta": {"Frame Types": "Probe Request, Beacon"}}
        self.assertEqual([r["t"] for r in flt.vendor_filters(wifi)], ["PROBE", "BEACON"])
        # every database entry encodes as a valid list on its own
        for g in flt.vendor_db():
            flt.encode(flt.vendor_filters(g))


class PlanTest(unittest.TestCase):
    def test_local_vs_remote(self):
        lines, expect, _ = commands.plan("audio", "A1B2C3", {"buzzer": 0, "led": 1}, "A1B2C3")
        self.assertEqual((lines, expect), ([("audio 0 1", "OK")], "local"))
        lines, expect, _ = commands.plan("audio", "D4E5F6", {"buzzer": 1, "led": 0}, "A1B2C3")
        self.assertEqual((lines, expect), ([("raudio D4E5F6 1 0", "CMD")], "ack"))
        lines, expect, _ = commands.plan("status", "all", {}, "A1B2C3")
        self.assertEqual((lines, expect), ([("rstatus all", "CMD")], "rst"))

    def test_filters_set_lines(self):
        lines, expect, label = commands.plan(
            "filters_set", "D4E5F6", {"filters": [{"t": "OUI", "id": "0025DF", "d": "Axon"}]}, "A1B2C3")
        self.assertEqual(lines[0], ("mfbegin D4E5F6", "OK"))
        self.assertTrue(lines[1][0].startswith("mfdata "))
        self.assertEqual(lines[-1], ("mfend", "CMD"))
        self.assertEqual(expect, "ack")
        lines, expect, _ = commands.plan(
            "filters_set", "A1B2C3", {"filters": [{"t": "OUI", "id": "0025DF"}]}, "A1B2C3")
        self.assertEqual((lines[0][0], lines[-1][1], expect), ("mfbegin self", "OK", "local"))

    def test_library_one_entry_per_vendor(self):
        lib = flt.library()
        keys = [g["key"] for g in lib]
        self.assertEqual(len(keys), len(set(keys)))
        by = {g["key"]: g for g in lib}
        # Firmware presets and database entries for the same vendor merge.
        self.assertIn("axon", by)
        self.assertEqual({r["t"] for r in by["axon"]["rows"]}, {"OUI", "PROBE", "BEACON", "CID", "SVC"})
        for k in ("ipro", "meta", "verkada", "flipper zero", "flock safety"):
            self.assertGreater(len(by[k]["sources"]), 1, k)
        for g in lib:
            rows = [(r["t"], r["id"].upper()) for r in g["rows"]]
            self.assertEqual(len(rows), len(set(rows)), g["label"])
            flt.encode(g["rows"])      # every entry is valid on the wire

    def test_filters_add_del_use_their_own_verbs(self):
        # An older home would read "mfbegin <t> add" as a full replace; the
        # dedicated verbs make it answer "unknown command" instead.
        one = {"filters": [{"t": "MAC", "id": "aa:bb:cc:dd:ee:ff", "d": "watched"}]}
        lines, expect, label = commands.plan("filters_add", "D4E5F6", one, "A1B2C3")
        self.assertEqual((lines[0], lines[-1], expect), (("mfadd D4E5F6", "OK"), ("mfend", "CMD"), "ack"))
        self.assertEqual(label, "Add MAC AABBCCDDEEFF")
        lines, expect, _ = commands.plan("filters_del", "all", one, "A1B2C3")
        self.assertEqual(lines[0], ("mfdel all", "OK"))
        lines, expect, _ = commands.plan("filters_add", "A1B2C3", one, "A1B2C3")
        self.assertEqual((lines[0][0], lines[-1][1], expect), ("mfadd self", "OK", "local"))

    def test_firmware_image_checks(self):
        import firmware as fwm
        path = os.path.join(os.path.dirname(__file__), "..", "..", ".pio", "build", "seeed_xiao_esp32s3", "firmware.bin")
        if os.path.exists(path):
            meta = fwm.inspect(open(path, "rb").read())
            self.assertEqual(len(meta["sha256"]), 64)
            self.assertTrue(meta["date"] and meta["time"])
        with self.assertRaises(fwm.FirmwareError):
            fwm.inspect(b"\x00" * 100000)               # not an app image
        bad = bytearray(open(path, "rb").read()) if os.path.exists(path) else None
        if bad:
            bad[12] = 5                                  # another chip id
            with self.assertRaises(fwm.FirmwareError):
                fwm.inspect(bytes(bad))

    def test_firmware_plans(self):
        lines, expect, _ = commands.plan("fw_send", "D4E5F6", {}, "A1B2C3")
        self.assertEqual((lines, expect), ([("fwsend D4E5F6", "CMD")], "ota"))
        with self.assertRaises(commands.PlanError):
            commands.plan("fw_send", "all", {}, "A1B2C3")
        with self.assertRaises(commands.PlanError):
            commands.plan("fw_stage", "D4E5F6", {}, "A1B2C3")   # staging is home-only
        lines, expect, _ = commands.plan("fw_self", "A1B2C3", {}, "A1B2C3")
        self.assertEqual((lines, expect), ([("fwself", "OK")], "local"))

    def test_key_never_over_the_mesh(self):
        with self.assertRaises(commands.PlanError):
            commands.plan("setkey", "D4E5F6", {"key": "k" * 32}, "A1B2C3")
        with self.assertRaises(commands.PlanError):
            commands.plan("setkey", "all", {"key": "k" * 32}, "A1B2C3")
        lines, expect, _ = commands.plan("setkey", "A1B2C3", {"key": "k" * 32}, "A1B2C3")
        self.assertEqual(expect, "local")
        with self.assertRaises(commands.PlanError):
            commands.plan("setkey", "A1B2C3", {"key": "short"}, "A1B2C3")

    def test_guards(self):
        with self.assertRaises(commands.PlanError):
            commands.plan("rechan", "all", {"ch": 13}, "A1B2C3")
        with self.assertRaises(commands.PlanError):
            commands.plan("preset", "D4E5F6", {"name": "nope"}, "A1B2C3")
        with self.assertRaises(commands.PlanError):
            commands.plan("audio", "zz", {}, "A1B2C3")


class CommanderDemoTest(unittest.TestCase):
    """Commander driving the demo console simulator end to end."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.store = Store(os.path.join(cls.tmp.name, "t.db"))
        cls.events = []
        cls.fleet = Fleet(cls.store, lambda e, p: cls.events.append((e, p)))
        cls.demo = DemoSource(cls.fleet, lambda: None)
        cls.fleet.feed_line(f"[MESH] ESP-NOW LR up: HOME node, channel 6, id {DEMO_HOME}", "demo")
        cls.demo._tlm(time.time())
        cls.cmd = commands.Commander(cls.fleet, cls.store, lambda e, p: None,
                                     lambda: cls.demo, lambda: {"status_poll": 0})
        cls.fleet.on_line = cls.cmd.feed_line
        cls.cmd.start()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def wait(self, job, timeout=25):
        end = time.time() + timeout
        while time.time() < end:
            j = self.cmd.jobs[job["id"]]
            if j["state"] in ("done", "failed", "partial"):
                return j
            time.sleep(0.1)
        self.fail(f"job {job['label']} stuck in {self.cmd.jobs[job['id']]['state']}")

    def node(self, nid):
        return next(n for n in self.fleet.snapshot()["nodes"] if n["id"] == nid)

    def test_status_and_audio(self):
        j = self.wait(self.cmd.submit("status", "DE0B02"))
        self.assertEqual(j["state"], "done")
        self.assertEqual(self.node("DE0B02")["cfg"]["buzzer"], 1)
        j = self.wait(self.cmd.submit("audio", "DE0B02", {"buzzer": 0, "led": 1}))
        self.assertEqual((j["state"], j["replies"]["DE0B02"]["text"]), ("done", "applied"))
        self.wait(self.cmd.submit("status", "DE0B02"))
        self.assertEqual(self.node("DE0B02")["cfg"]["buzzer"], 0)

    def test_old_firmware_refuses(self):
        j = self.wait(self.cmd.submit("identify", "DE0E05", {"secs": 5}))
        self.assertEqual((j["state"], j["error"]), ("failed", "rejected"))

    def test_filter_push_and_readback(self):
        lst = [{"t": "OUI", "id": "D42DC5", "d": "I-PRO"}, {"t": "PROBE", "id": "D42DC5", "d": "I-PRO wifi"}]
        j = self.wait(self.cmd.submit("filters_set", "DE0C03", {"filters": lst}))
        self.assertEqual(j["state"], "done")
        self.demo.down_until.clear()                    # skip the simulated reboot
        j = self.wait(self.cmd.submit("filters_get", "DE0C03"))
        self.assertEqual(j["replies"]["DE0C03"]["text"], "2 filters")
        self.assertEqual([f["id"] for f in self.node("DE0C03")["filters"]], ["D42DC5", "D42DC5"])

    def test_live_add_and_remove(self):
        base = [{"t": "OUI", "id": "D42DC5", "d": "I-PRO"}]
        self.wait(self.cmd.submit("filters_set", "DE0C03", {"filters": base}))
        add = [{"t": "MAC", "id": "AABBCCDDEEFF", "d": "watched"}]
        j = self.wait(self.cmd.submit("filters_add", "DE0C03", {"filters": add}))
        self.assertEqual((j["state"], j["replies"]["DE0C03"]["n"]), ("done", 2))
        self.assertNotIn("DE0C03", self.demo.down_until)        # live: no reboot
        self.wait(self.cmd.submit("filters_get", "DE0C03"))
        self.assertEqual([f["id"] for f in self.node("DE0C03")["filters"]], ["D42DC5", "AABBCCDDEEFF"])
        j = self.wait(self.cmd.submit("filters_del", "DE0C03", {"filters": add}))
        self.assertEqual((j["state"], j["replies"]["DE0C03"]["n"]), ("done", 1))
        # Removing the last filter is refused: the node would scan for nothing.
        j = self.wait(self.cmd.submit("filters_del", "DE0C03", {"filters": base}))
        self.assertEqual(j["state"], "failed")
        self.wait(self.cmd.submit("filters_get", "DE0C03"))
        self.assertEqual([f["id"] for f in self.node("DE0C03")["filters"]], ["D42DC5"])

    def test_local_home_status(self):
        j = self.wait(self.cmd.submit("status", DEMO_HOME))
        self.assertEqual(j["state"], "done")
        self.assertEqual(self.node(DEMO_HOME)["cfg"]["role"], "home")


if __name__ == "__main__":
    unittest.main()
