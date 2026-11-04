// The app as the dashboard, with no hardware: node lines in the firmware's formats
// (src/main.cpp, mesh.cpp, rid.cpp) through a fake link into the model.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
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

    link.feed('TLM {"v":1,"id":"AD5484","role":"home","mode":"scan","mesh":1,"ch":6,"up":120,"rx":10,"tx":4,"txf":0,"dup":1,"bad":0,'
        '"n":2,"rate":"lr250k","enc":1,"nodes":[{"id":"4B4D04","role":"remote","last":1,"pkts":40,"rssi":-61},{"id":"51A09C","role":"remote","last":2,"pkts":12,"rssi":-77}]}');
    await settle();
    expect(m.link, Link.up);
    expect(m.nodes.keys.toSet(), {'AD5484', '4B4D04', '51A09C'});
    expect(m.nodes['AD5484']!.isHome, true);
    expect(m.nodes['4B4D04']!.online, true);
    expect(m.home['ch'], 6);

    link.feed('RST {"node":"4B4D04","role":"remote","mode":"scan","ch":6,"buzzer":1,"led":0,"key":1,"locked":1,"peers":2,"lrssi":-60,'
        '"filters":12,"fcrc":"1A2B3C4D","up":300,"heap":120000,"fw":"2026-11-01 1200","alias":"Back porch","nbr":[["AD5484",-61,40]]}');
    await settle();
    expect(m.nodes['4B4D04']!.filters, 12);
    expect(m.nodes['4B4D04']!.buzzer, 1);
    expect(m.nodes['4B4D04']!.label, 'Back porch');

    // a detection relayed over the mesh: the node comes from the [mesh] tag
    link.feed('{"mac":"44:c2:7e:28:06:92","rssi":-71,"type":"OUI","match":"44C27E","desc":"Axon Body [mesh 4B4D04]","dom":"ble",'
        '"info":{"name":"Axon","cid":"0x0B34"}}');
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
    link.feed('RID {"mac":"8c:1e:d9:27:a5:93","node":"51A09C","rssi":-80,"src":"wifi","id":"1581F895C26250170B","idt":1,"uat":2,'
        '"lat":35.6,"lon":-82.5,"alt":700,"hagl":42,"spd":5.5,"vspd":63,"hdg":90,"st":2,"plat":35.601,"plon":-82.501}');
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
}
