// The app as the dashboard, with no hardware: node lines in the firmware's formats
// (src/main.cpp, mesh.cpp, rid.cpp) through a fake link into the model.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/firmware.dart';
import 'package:ouispy_mesh/data/local_mesh.dart';
import 'package:ouispy_mesh/data/lookup.dart';
import 'package:ouispy_mesh/data/model.dart';
import 'package:ouispy_mesh/data/usb_link.dart';

class FakeLink implements LineLink {
  final _l = StreamController<String>.broadcast();
  final _s = StreamController<bool>.broadcast();
  final sent = <String>[];
  @override
  String get name => 'fake';
  @override
  bool get open => true;
  @override
  Stream<String> get lines => _l.stream;
  @override
  Stream<bool> get state => _s.stream;
  @override
  Future<bool> start() async => true;
  @override
  Future<void> stop() async {}
  @override
  Future<void> send(String line) async => sent.add(line);
  void feed(String l) => _l.add(l);
}

Future<void> settle() => Future.delayed(const Duration(milliseconds: 20));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('TLM roster, RST, detections, sightings, RID, commands', () async {
    await Lookup.I.load();
    final m = Model();
    final link = FakeLink();
    final mesh = LocalMesh(m, link);
    await mesh.start();

    link.feed(
      'TLM {"v":1,"id":"AD5484","role":"home","mode":"scan","mesh":1,"ch":6,"up":120,"rx":10,"tx":4,"txf":0,"dup":1,"bad":0,'
      '"n":2,"rate":"lr250k","enc":1,"nodes":[{"id":"4B4D04","role":"remote","last":1,"pkts":40,"rssi":-61},{"id":"51A09C","role":"remote","last":2,"pkts":12,"rssi":-77}]}',
    );
    await settle();
    expect(m.link, Link.up);
    expect(m.nodes.keys.toSet(), {'AD5484', '4B4D04', '51A09C'});
    expect(m.nodes['AD5484']!.isHome, true);
    expect(m.nodes['4B4D04']!.online, true);
    expect(m.home['ch'], 6);

    link.feed(
      'RST {"node":"4B4D04","role":"remote","mode":"scan","ch":6,"buzzer":1,"led":0,"key":1,"locked":1,"peers":2,"lrssi":-60,'
      '"filters":12,"fcrc":"1A2B3C4D","up":300,"heap":120000,"fw":"2026-11-01 1200","alias":"Back porch","nbr":[["AD5484",-61,40]]}',
    );
    await settle();
    expect(m.nodes['4B4D04']!.filters, 12);
    expect(m.nodes['4B4D04']!.buzzer, 1);
    expect(m.nodes['4B4D04']!.label, 'Back porch');

    // a detection relayed over the mesh: the node comes from the [mesh] tag
    link.feed(
      '{"mac":"44:c2:7e:28:06:92","rssi":-71,"type":"OUI","match":"44C27E","desc":"Axon Body [mesh 4B4D04]","dom":"ble",'
      '"info":{"name":"Axon","cid":"0x0B34"}}',
    );
    await settle();
    expect(m.dets.length, 1);
    expect(m.dets.first.node, '4B4D04');
    expect(m.dets.first.descr, 'Axon Body');
    expect(m.dets.first.isNew, true);
    expect(m.alertsUnacked, 1);
    expect(m.targets['44:C2:7E:28:06:92']!.name, 'Axon');

    // the same device from the home within 6 s: a new row (different node), no new alert
    link.feed('{"mac":"44:C2:7E:28:06:92","rssi":-58,"type":"OUI","desc":"Axon Body","node":"AD5484","dom":"ble"}');
    await settle();
    expect(m.dets.length, 2);
    expect(m.alertsUnacked, 1);
    expect(m.dets.first.seen.first.node, 'AD5484'); // strongest first
    expect(m.dets.first.closest, 'AD5484');
    // and again from the home: deduped
    link.feed('{"mac":"44:C2:7E:28:06:92","rssi":-59,"type":"OUI","desc":"Axon Body","node":"AD5484","dom":"ble"}');
    await settle();
    expect(m.dets.length, 2);
    // a signal update is a sighting, never a row
    link.feed('{"mac":"44:C2:7E:28:06:92","rssi":-55,"type":"OUI","desc":"Axon Body","node":"AD5484","dom":"ble","upd":1}');
    await settle();
    expect(m.dets.length, 2);

    // Remote ID
    link.feed(
      'RID {"mac":"8c:1e:d9:27:a5:93","node":"51A09C","rssi":-80,"src":"wifi","id":"1581F895C26250170B","idt":1,"uat":2,'
      '"lat":35.6,"lon":-82.5,"alt":700,"hagl":42,"spd":5.5,"vspd":63,"hdg":90,"st":2,"plat":35.601,"plon":-82.501}',
    );
    await settle();
    final d = m.drones['8C:1E:D9:27:A5:93']!;
    expect(d.ua, 'Multirotor / helicopter');
    expect(d.status, 'Airborne');
    expect(d.vspd, null); // 63 = unknown
    expect(d.hasPilot, true);
    expect(d.pilotDist, isNotNull);
    expect(m.dets.first.type, 'RID');

    // a command: queued, sent as the console line, settled by the reply
    final f = mesh.nodeCmd('4B4D04', 'identify', {'secs': 10});
    await settle();
    expect(link.sent.last, 'rident 4B4D04 10 3');
    link.feed('CMD {"op":"ident","id":7,"target":"4B4D04","cmd":7}');
    expect(await f, isNotNull);
    final g = mesh.nodeCmd('AD5484', 'audio', {'buzzer': 0, 'led': 1});
    await settle();
    expect(link.sent.last, 'audio 0 1');
    link.feed('OK audio buzzer=0 led=1');
    expect(await g, isNotNull);

    // the roster drops a node: it goes offline
    link.feed('TLM {"v":1,"id":"AD5484","role":"home","ch":6,"n":1,"nodes":[{"id":"4B4D04","role":"remote","last":1,"pkts":41,"rssi":-60}]}');
    await settle();
    expect(m.nodes['51A09C']!.flagOnline, false);

    final h = await mesh.history();
    expect(h!['total'], 2);
    final rec = await mesh.device('44:C2:7E:28:06:92');
    expect((rec!['rows'] as List).any((r) => r[0] == 'Broadcast name'), true);
    await mesh.stop();
  });

  group('firmware and security over the console (commands.py plan)', () {
    late Model m;
    late FakeLink link;
    late LocalMesh mesh;
    setUp(() async {
      await Lookup.I.load();
      m = Model();
      link = FakeLink();
      mesh = LocalMesh(m, link);
      await mesh.start();
      link.feed('TLM {"v":1,"id":"AD5484","role":"home","ch":6,"n":1,"nodes":[{"id":"4B4D04","role":"remote","last":1,"pkts":40,"rssi":-61}]}');
      await settle();
    });
    tearDown(() => mesh.stop());

    test('fw_stage: fwbegin, 512-byte fwdata with resume, fwend', () async {
      final img = List<int>.generate(1300, (i) => (i * 7 + 3) & 0xff);
      final prog = <int>[];
      final f = mesh.nodeRun('AD5484', 'fw_stage', {'image': img, 'progress': (int done, int size) => prog.add(done)});
      await settle();
      expect(link.sent.last, 'fwbegin 1300 ${sha256Hex(img)}');
      expect(sha256Hex(img), '5af531edf226c8c97629e4bcf8d3daa4b46599d9fcc3e0131afc3e0e150baef8'); // hashlib
      expect(mesh.staging, true);
      link.feed('OK fwbegin');
      await settle();
      expect(link.sent.last, 'fwdata 0 ${hexOf(img.sublist(0, 512))}');
      link.feed('OK fwdata 512');
      await settle();
      expect(link.sent.last, startsWith('fwdata 512 '));
      final n = link.sent.length;
      link.feed('OK fwdata 512'); // a stale reply to a resent line: ignored
      link.feed('OK rstatus'); // someone else's answer: ignored
      await settle();
      expect(link.sent.length, n);
      // the node lost a line: it names the offset it wants and the upload picks up there
      link.feed('ERR fwdata: expected offset 0');
      await settle();
      expect(link.sent.last, 'fwdata 0 ${hexOf(img.sublist(0, 512))}');
      link.feed('OK fwdata 512');
      await settle();
      link.feed('OK fwdata 1024');
      await settle();
      expect(link.sent.last, 'fwdata 1024 ${hexOf(img.sublist(1024))}');
      expect(link.sent.last.length, 'fwdata 1024 '.length + 276 * 2);
      link.feed('OK fwdata 1300');
      await settle();
      expect(link.sent.last, 'fwend');
      link.feed('OK fwend staged 1300 bytes');
      final r = await f;
      expect(r.err, isNull);
      expect(r.r!['size'], 1300);
      expect(prog.last, 1300);
      expect(mesh.staging, false);
    });

    test('fw_stage fails on ERR fwbegin; only on the home', () async {
      final f = mesh.nodeRun('AD5484', 'fw_stage', {'image': List<int>.filled(600, 1)});
      await settle();
      link.feed('ERR fwbegin: no PSRAM for the image');
      expect((await f).err, 'fwbegin: no PSRAM for the image');
      final g = await mesh.nodeRun('4B4D04', 'fw_stage', {'image': List<int>.filled(600, 1)});
      expect(g.err, contains('staged on the home'));
    });

    test('abort stops the upload and sends fwabort', () async {
      final f = mesh.nodeRun('AD5484', 'fw_stage', {'image': List<int>.filled(2000, 9)});
      await settle();
      link.feed('OK fwbegin');
      await settle();
      final a = mesh.fwAbort();
      expect((await f).err, 'aborted');
      await settle();
      expect(link.sent.last, 'fwabort');
      link.feed('OK fwabort');
      expect(await a, isNotNull);
    });

    test('fw_send / fw_self, OTA progress lines', () async {
      final ota = <Map<String, dynamic>>[];
      final sub = mesh.otaLines.listen(ota.add);
      final s = mesh.nodeRun('4B4D04', 'fw_send');
      await settle();
      expect(link.sent.last, 'fwsend 4B4D04');
      link.feed('CMD {"op":"fwsend","id":9,"target":"4B4D04","cmd":11}');
      expect((await s).r, isNotNull);
      link.feed('OTA {"node":"4B4D04","state":"sending","pct":42,"bytes":500000,"size":1200000,"kbps":96,"direct":1,"proto":2,"err":""}');
      await settle();
      expect(ota.single['pct'], 42);
      expect(m.nodes['4B4D04']!.ota['state'], 'sending');
      await sub.cancel();
      expect((await mesh.nodeRun('AD5484', 'fw_send')).err, contains('pick one remote'));
      expect((await mesh.nodeRun('4B4D04', 'fw_self')).err, contains('home node'));
      final h = mesh.nodeRun('AD5484', 'fw_self');
      await settle();
      expect(link.sent.last, 'fwself');
      link.feed('OK installing the staged firmware on this node — rebooting');
      expect((await h).r, isNotNull);
      link.feed('ERR fwsend: an update is already running');
      final busy = mesh.nodeRun('4B4D04', 'fw_send');
      await settle();
      link.feed('ERR fwsend: an update is already running');
      expect((await busy).err, 'fwsend: an update is already running');
    });

    test('wipe with a typed confirm, lost list as rdeny', () async {
      expect((await mesh.nodeRun('4B4D04', 'wipe', {'confirm': 'wipe'})).err, contains('WIPE 4B4D04'));
      final w = mesh.nodeRun('4B4D04', 'wipe', {'confirm': 'wipe 4b4d04'});
      await settle();
      expect(link.sent.last, 'rwipe 4B4D04 WIPE');
      link.feed('CMD {"op":"rwipe","id":10,"target":"4B4D04","cmd":12}');
      expect((await w).r, isNotNull);
      final h = mesh.nodeRun('AD5484', 'wipe', {'confirm': 'WIPE AD5484'});
      await settle();
      expect(link.sent.last, 'wipe NOW');
      link.feed('WIPE {"why":"asked over USB"}');
      expect((await h).r, isNotNull);
      final all = mesh.nodeRun('all', 'wipe', {'confirm': 'WIPE ALL'});
      await settle();
      expect(link.sent.last, 'rwipe all WIPE');
      link.feed('CMD {"op":"rwipe","id":11,"target":"all","cmd":12}');
      await settle();
      expect(link.sent.last, 'wipe NOW');
      link.feed('WIPE {"why":"asked over USB"}');
      expect((await all).r, isNotNull);

      final l = mesh.setLost('4B4D04', true, note: 'left at the cafe');
      await settle();
      expect(link.sent.last, 'rdeny all 4B4D04');
      expect(m.nodes['4B4D04']!.raw['lost'], true);
      link.feed('CMD {"op":"rdeny","id":12,"target":"all","cmd":13}');
      expect(await l, isNotNull);
      final c = mesh.setLost('4B4D04', false);
      await settle();
      expect(link.sent.last, 'rdeny all -');
      link.feed('CMD {"op":"rdeny","id":13,"target":"all","cmd":13}');
      await c;
      expect(m.nodes['4B4D04']!.raw['lost'], false);
      expect(mesh.forget('4B4D04'), false); // online
    });
  });
}
