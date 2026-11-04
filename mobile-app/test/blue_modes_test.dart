// Board recognition and each Unified Blue mode's parser, with lines in the firmware's own
// formats (oui-spy-recon-blue lib/blue_modes/src/raw/*.cpp, lib/oui_mesh/src/rb_mesh.cpp,
// sky-spy-c5, the legacy oui-spy-unified-blue wrapper) through a fake Backend.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/backend.dart';
import 'package:ouispy_mesh/data/blue_modes.dart';
import 'package:ouispy_mesh/theme.dart';
import 'package:ouispy_mesh/views/board_view.dart';

class FakeBackend implements Backend {
  @override
  final console = Console();
  final sent = <String>[];
  @override
  String get label => 'fake';
  @override
  Future<bool> sendLine(String line) async {
    sent.add(line);
    console.add(line, src: 'phone');
    return true;
  }

  void feed(String l) => console.add(l);

  @override
  dynamic noSuchMethod(Invocation i) => Future.value();
}

Future<void> pump() => Future<void>.delayed(Duration.zero);

void main() {
  late Board b;
  late FakeBackend fake;
  var now = 1000.0;
  setUp(() {
    now = 1000.0;
    b = Board(clock: () => now);
    fake = FakeBackend();
  });

  Future<void> lines(List<String> ls) async {
    for (final l in ls) {
      fake.feed(l);
    }
    await pump();
  }

  test('attach replays the ring, then follows the stream, ignoring our own lines', () async {
    fake.feed('[axeoff] armed — scanning for Axon signatures');
    b.attach(fake);
    expect(b.modeId, 'axeoff');
    await fake.sendLine('CMD:STATUS');
    await lines(['[state] IDLE -> TRACKING']);
    expect(b.axe.state, 'TRACKING');
    expect(b.lines.items.any((l) => l.line == 'CMD:STATUS'), isFalse);
    b.detach();
    await lines(['[state] TRACKING -> IDLE (lost)']);
    expect(b.axe.state, 'TRACKING');
  });

  test('probe sends status, CMD:VERSION, get_config 400 ms apart', () async {
    b.attach(fake);
    final sw = Stopwatch()..start();
    await b.probe();
    expect(fake.sent, ['status', 'CMD:VERSION', '{"cmd":"get_config"}']);
    expect(sw.elapsedMilliseconds, greaterThanOrEqualTo(800));
  });

  test('Recon: TLM kind reconblue, app, mstate; RST fw R', () async {
    b.attach(fake);
    await lines([
      'TLM {"v":1,"id":"A1B2C3","role":"home","mode":"scan","mesh":1,"ch":6,"up":120,"heap":180000,"dev":0,"filters":0,"n":2,"rx":10,"q":0,"dup":0,"self":0,"bad":0,'
          '"tx":5,"txf":0,"txcb":5,"pend":0,"lrssi":-60,"heal":0,"mqtt":0,"dsent":0,"dretry":0,"dack":0,"dlost":0,"dpend":0,"park":0,"pble":0,"rate":"LR 250k","enc":0,'
          '"eplain":0,"eauth":0,"ereplay":0,"nvs":100,"kind":"reconblue","app":"pcap","mstate":"paused","rid":1,"drones":0,"wd":[0,0],"wdch":"","wdon":0,"strays":[],"nodes":[]}',
      'RST {"node":"A1B2C3","role":"home","mode":"scan","ch":6,"buzzer":1,"led":1,"key":0,"locked":0,"mqtt":0,"heal":0,"peers":2,"lrssi":-60,"filters":0,"fcrc":"00000000",'
          '"up":120,"heap":180000,"dev":0,"dsent":0,"dacked":0,"dexp":0,"fw":"Nov04 21:21:25R"}',
    ]);
    expect(b.family, BoardFamily.recon);
    expect(b.modeId, 'pcap');
    expect(b.modeLabel, 'PCAP');
    expect(b.meshOn, isTrue);
    expect(b.meshRole, 'home');
    expect(b.meshCh, 6);
    expect(b.meshRate, 'LR 250k');
    expect(b.meshState, 'paused');
    expect(b.firmware, 'Nov04 21:21:25R');
    expect(b.nodeId, 'A1B2C3');
    expect(b.chip, contains('Recon'));
    // another node's status is not this board's
    await lines(['RST {"node":"FFFFFF","role":"remote","ch":6,"fw":"Oct28 20:37:51W"}']);
    expect(b.family, BoardFamily.recon);
    await lines(['OK blue detector — rebooting']);
    expect(b.pendingMode, 'detector');
  });

  test('Recon solo: mesh off', () async {
    b.attach(fake);
    await lines(['TLM {"v":1,"id":"A1B2C3","role":"solo","mode":"scan","mesh":0,"ch":1,"rate":"off","kind":"reconblue","app":"foxhunter","mstate":"off","nodes":[]}']);
    expect(b.meshOn, isFalse);
    expect(b.modeId, 'foxhunter');
  });

  test('OUI-SPY mesh detector: TLM kind and fw suffix', () async {
    b.attach(fake);
    await lines(['RST {"node":"AD5484","role":"home","mode":"scan","ch":1,"fw":"Oct28 20:37:51W"}']);
    expect(b.family, BoardFamily.ouispy);
    expect(b.fwKind, 'wardrive');
    expect(kindFromFw('Oct28 20:37:51S'), 'skyspy');
    expect(kindFromFw('Oct28 20:37:51Y'), 'gpswd');
    expect(kindFromFw('Oct28 20:37:51G'), 'gps');
    expect(kindFromFw('Oct28 20:37:51'), 'detector');
    await lines(['TLM {"v":1,"id":"AD5484","role":"home","mesh":1,"ch":1,"rate":"LR","kind":"detector","nodes":[]}']);
    expect(b.fwKind, 'detector');
    expect(b.summary, 'OUI-SPY mesh detector · Mesh detector');
    await lines(['{"mac":"AA:BB:CC:DD:EE:FF","alias":"","rssi":-70,"type":"OUI","match":"AA:BB:CC","desc":"Axon","node":"AD5484","dom":"ble"}']);
    expect(b.meshDets.length, 1);
    expect(b.meshDets.last!.desc, 'Axon');
    expect(b.modeId, isEmpty);
  });

  test('legacy wrapper banner and FINAL BOOT MODE; passwords redacted', () async {
    b.attach(fake);
    await lines([
      'OUI SPY UNIFIED FIRMWARE v2.0',
      "[OUI-SPY] Loaded AP config: SSID='snoopuntothem' PASS='hunter22'",
      '[OUI-SPY] FINAL BOOT MODE: 6',
      'Password: secret123',
    ]);
    expect(b.family, BoardFamily.legacyUnified);
    expect(b.firmware, 'Unified Blue v2.0');
    expect(b.modeId, 'blesniff');
    final all = b.lines.items.map((l) => l.line).join('\n');
    expect(all, isNot(contains('hunter22')));
    expect(all, isNot(contains('secret123')));
    expect(redact('WiFi Password: (Open Network)'), 'WiFi Password: ***');
    expect(redact('Buzzer: ON'), 'Buzzer: ON');
    await lines(['[OUI-SPY] >>> STARTING FOXHUNTER (mode 2) <<<']);
    expect(b.modeId, 'foxhunter');
  });

  test('Foxhunter: RSSI history, acquired / lost', () async {
    b.attach(fake);
    await lines([
      'Target MAC: aa:bb:cc:11:22:33',
      'TARGET ACQUIRED!',
      'RSSI: -67 dBm',
      'RSSI: -61 dBm',
      'TARGET LOST - Searching...',
      'TARGET REACQUIRED!',
      'RSSI: -80 dBm',
    ]);
    expect(b.modeId, 'foxhunter');
    expect(b.fox.target, 'AA:BB:CC:11:22:33');
    expect(b.fox.history.oldestFirst.map((p) => p.rssi), [-67, -61, -80]);
    expect(b.fox.events.oldestFirst.map((e) => e.what), ['acquired', 'lost', 'reacquired']);
    expect(b.fox.rssi, -80);
    expect(b.fox.state, 'tracking');
  });

  test('PCAP: frames, counts, STATUS, version', () async {
    b.attach(fake);
    await lines([
      'OUI-SPY PCAP 1.0.0 built Nov  4 2026 10:00:00',
      '[Ch6 RSSI-54dBm] MGMT/BEACON src=aa:bb:cc:dd:ee:01 dst=ff:ff:ff:ff:ff:ff bssid=aa:bb:cc:dd:ee:01 len=212 ssid=Home Net',
      '[Ch6 RSSI-71dBm] DATA/QOS-DATA src=11:22:33:44:55:66 dst=aa:bb:cc:dd:ee:01 bssid=aa:bb:cc:dd:ee:01 len=1500',
      '[Ch11 RSSI-88dBm] SHORT len=14',
      '{"mode":"HOP","chan":11,"hopmask":"0x0421","dwell":250,"total":3,"pps":1,"drop_pcap":0,"drop_dash":0,"fw":"1.0.0","wifi":"APSTA","ap_ssid":"ouispy-pcap","ap_ip":"192.168.4.1","ap_mac":"aa:bb:cc:dd:ee:02","ap_stations":0}',
    ]);
    expect(b.modeId, 'pcap');
    expect(b.version, '1.0.0');
    expect(b.pcap.total, 3);
    final f = b.pcap.frames.oldestFirst.first;
    expect(
      [f.ch, f.rssi, f.type, f.sub, f.src, f.dst, f.bssid, f.len, f.ssid],
      [6, -54, 'MGMT', 'BEACON', 'aa:bb:cc:dd:ee:01', 'ff:ff:ff:ff:ff:ff', 'aa:bb:cc:dd:ee:01', 212, 'Home Net'],
    );
    expect(b.pcap.byType, {'MGMT': 1, 'DATA': 1, 'SHORT': 1});
    expect(b.pcap.byChan, {6: 2, 11: 1});
    expect(b.pcap.hopping, isTrue);
    expect(b.pcap.hopMask, 0x0421);
    expect(BlueCmd.channelsOf(0x0421), {1, 6, 11});
    expect(b.pcap.dwell, 250);
  });

  test('BLE Sniff: adverts, per-MAC aggregate, STATUS', () async {
    b.attach(fake);
    await lines([
      'OUI-SPY BLESNIFF 1.0.0 built Nov  4 2026 10:00:00',
      '[Ch37 RSSI-60dBm] ADV_IND rnd-s:c0:ff:ee:00:11:22 name="Axon Body 3" svc=0xFE6C mfr=0059(Nordic)',
      '[Ch38 RSSI-52dBm] ADV_NONCONN pub:c0:ff:ee:00:11:22',
      '[Ch? RSSI-90dBm] SCAN_RSP rnd-rpa:4a:11:22:33:44:55 mfr=004C(Apple)',
      '{"scan_win":30,"scan_int":100,"ftmask":"0x7f","total":3,"pps":1,"drop_pcap":0,"drop_dash":0,"fw":"1.0.0","ap_ssid":"ouispy-blesniff","ap_ip":"192.168.4.1",'
          '"ap_mac":"aa:bb","ap_stations":0,"session_bytes":0,"session_cap":0,"session_drop":0,"state":"idle","psram_free":0,"heap_free":100000}',
    ]);
    expect(b.modeId, 'blesniff');
    final a = b.ble.adverts.oldestFirst.first;
    expect(
      [a.ch, a.rssi, a.pdu, a.addrType, a.mac, a.name, a.svc, a.mfrId, a.mfrName],
      [37, -60, 'ADV_IND', 'rnd-s', 'C0:FF:EE:00:11:22', 'Axon Body 3', '0xFE6C', 0x59, 'Nordic'],
    );
    expect(b.ble.adverts.oldestFirst[2].ch, isNull);
    final d = b.ble.devices['C0:FF:EE:00:11:22']!;
    expect([d.count, d.best, d.rssi, d.name], [2, -52, -52, 'Axon Body 3']);
    expect(b.ble.devices.length, 2);
    expect(b.ble.window, 30);
    expect(b.ble.interval, 100);
  });

  test('Flock-You: detections and config', () async {
    b.attach(fake);
    await lines([
      '{"event":"detection","detection_method":"wifi_oui_addr2","detection_tier":2,"protocol":"wifi_2_4ghz","mac_address":"b4:1e:52:aa:bb:cc","oui":"B41E52",'
          '"device_name":"","rssi":-66,"channel":6,"frequency":2437,"ssid":"Flock-1234"}',
      '{"event":"config","beep_mask":29,"oui_count":42,"tiers":[{"tier":0,"method":"wifi_ssid","beep":1},{"tier":1,"method":"wifi_oui_addr1_addr3","beep":0},'
          '{"tier":2,"method":"wifi_oui_addr2","beep":1},{"tier":3,"method":"wifi_wildcard_probe","beep":1},{"tier":4,"method":"wifi_wildcard_probe_ie_sig","beep":1}]}',
    ]);
    expect(b.modeId, 'flock-wifi');
    final d = b.flock.dets.last!;
    expect(
      [d.method, d.tier, d.protocol, d.mac, d.oui, d.rssi, d.channel, d.freq, d.ssid],
      ['wifi_oui_addr2', 2, 'wifi_2_4ghz', 'B4:1E:52:AA:BB:CC', 'B41E52', -66, 6, 2437, 'Flock-1234'],
    );
    expect(b.flock.beepMask, 29);
    expect(b.flock.tiers.length, 5);
    expect(b.flock.tierBeeps(1), isFalse);
    expect(b.flock.tierBeeps(0), isTrue);
    expect(BlueCmd.setBeep(1, true), '{"cmd":"set_beep","tier":1,"on":1}');
    expect(BlueCmd.setBeepMask(31), '{"cmd":"set_beep_mask","mask":31}');
  });

  test('Detector: live events, plain hits deduped, dumps, STATUS', () async {
    b.attach(fake);
    const ev =
        '{"event":"detection","protocol":"ble","detection_method":"ble_oui","mac_address":"00:25:df:11:22:33","addr_type":"public","rssi":-58,"rssi_min":-70,"rssi_max":-58,'
        '"company_id":null,"service_uuid":null,"local_name":"","device_name":"","match_method":"oui","matched_signature":"00:25:DF","first_seen_ms":1000,"last_seen_ms":1000,"hit_count":1';
    await lines([
      'OUI-SPY BLE DETECTOR 1.1.0 built Nov  4 2026 10:00:00',
      '$ev,"replay_source":"live"}',
      '{"mac":"00:25:df:11:22:33","alias":"","rssi":-58}',
      '{"mac":"00:25:DF:11:22:33","alias":"","rssi":-58,"type":"OUI","match":"00:25:DF","desc":"Axon","node":"A1B2C3","dom":"ble"}',
    ]);
    expect(b.modeId, 'detector');
    expect(b.version, '1.1.0');
    expect(b.det.live.length, 1);
    final d = b.det.live.last!;
    expect([d.mac, d.rssi, d.method, d.signature, d.hits, d.companyId, d.replay], ['00:25:DF:11:22:33', -58, 'oui', '00:25:DF', 1, null, 'live']);
    await lines(['BEGIN_DUMP live bytes=0 count=1', '$ev,"replay_source":"ram"}', 'END_DUMP live count=1']);
    expect(b.det.dumpLive.length, 1);
    expect(b.det.dumpLive.first.replay, 'ram');
    expect(b.det.live.length, 1);
    await lines(['BEGIN_DUMP prev bytes=900 count=2', '$ev,"replay_source":"flash"}', '$ev,"replay_source":"flash"}', 'END_DUMP prev count=2']);
    expect(b.det.dumpPrev.length, 2);
    expect(b.det.dumping, isEmpty);
    await lines(['{"mode":"ble_detector","fw":"1.1.0","uptime_s":60,"live_count":1,"prev_exists":true,"prev_bytes":900,"heap":150000}']);
    expect(b.det.status['live_count'], 1);
    await b.detClear(prev: true);
    expect(fake.sent.last, 'CMD:CLEAR_PREV');
    expect(b.det.dumpPrev, isEmpty);
  });

  test('AXEOFF: trailing note stripped', () async {
    b.attach(fake);
    await lines(['{"mac":"00:25:df:aa:bb:cc","rssi":-72,"type":"BLE_SVC","match":"FE6C"}  Axon signature (svc UUID FE6C)', '[state] IDLE -> TRACKING']);
    expect(b.modeId, 'axeoff');
    final d = b.axe.dets.last!;
    expect([d.mac, d.rssi, d.type, d.match, d.note], ['00:25:DF:AA:BB:CC', -72, 'BLE_SVC', 'FE6C', 'Axon signature (svc UUID FE6C)']);
    expect(b.axe.state, 'TRACKING');
  });

  test('Sky Spy: 2.4 GHz and C5 5 GHz drones, status', () async {
    b.attach(fake);
    await lines([
      '{"status":"scanning"}',
      '{"mac":"60:60:1f:aa:bb:cc","rssi":-70,"drone_lat":40.123456,"drone_long":-74.654321,"drone_altitude":120,"pilot_lat":40.1,"pilot_long":-74.6,"basic_id":"1581F5FJD239C00DW22E"}',
      '{"mac":"60:60:1f:11:22:33","rssi":-80,"band":"5GHz","channel":149,"drone_lat":40.2,"drone_long":-74.5,"drone_altitude":90,"pilot_lat":0.0,"pilot_long":0.0,"basic_id":"C5DRONE","src":"c5"}',
    ]);
    expect(b.modeId, 'skyspy');
    expect(b.sky.status['status'], 'scanning');
    final a = b.sky.drones['60:60:1F:AA:BB:CC']!;
    expect([a.rssi, a.lat, a.lon, a.alt, a.basicId], [-70, 40.123456, -74.654321, 120, '1581F5FJD239C00DW22E']);
    final c = b.sky.drones['60:60:1F:11:22:33']!;
    expect([c.band, c.channel, c.src], ['5GHz', 149, 'c5']);
    expect(b.chip, 'ESP32-C5');
  });

  test('command lines', () {
    expect(BlueCmd.pcapChan(6), 'CMD:CHAN 6');
    expect(BlueCmd.pcapHop(BlueCmd.hopMaskOf([1, 6, 11])), 'CMD:HOP 0x0421');
    expect(BlueCmd.pcapDwell(250), 'CMD:DWELL 250');
    expect(BlueCmd.bleWindow(30), 'CMD:WINDOW 30');
    expect(BlueCmd.bleInterval(100), 'CMD:INTERVAL 100');
    expect(BlueCmd.blue('flock-wifi'), 'blue flock-wifi');
    expect(() => BlueCmd.blue(''), throwsArgumentError);
    expect(() => BlueCmd.blue('nope'), throwsArgumentError);
    expect(BlueCmd.mesh(true, 'remote', 6), 'mesh 1 remote 6');
  });

  // Every mode's panel renders at phone width without overflowing.
  final samples = <String, List<String>>{
    'unknown': ['hello', 'Password: secret'],
    'recon': [
      'TLM {"v":1,"id":"A1B2C3","role":"solo","mesh":0,"ch":1,"rate":"off","kind":"reconblue","app":"detector","mstate":"off","nodes":[]}',
      '{"event":"detection","protocol":"ble","detection_method":"ble_oui","mac_address":"00:25:df:11:22:33","rssi":-58,"match_method":"oui","matched_signature":"00:25:DF","hit_count":1,"replay_source":"live"}',
      '{"mode":"ble_detector","fw":"1.1.0","uptime_s":60,"live_count":1,"prev_exists":true,"prev_bytes":900,"heap":150000}',
    ],
    'foxhunter': ['Target MAC: aa:bb:cc:11:22:33', 'TARGET ACQUIRED!', for (var i = 0; i < 200; i++) 'RSSI: -${40 + i % 50} dBm', 'TARGET LOST - Searching...'],
    'pcap': [
      '[Ch6 RSSI-54dBm] MGMT/BEACON src=aa:bb:cc:dd:ee:01 dst=ff:ff:ff:ff:ff:ff bssid=aa:bb:cc:dd:ee:01 len=212 ssid=A very long network name here',
      '{"mode":"HOP","chan":11,"hopmask":"0x0421","dwell":250,"total":3,"pps":1,"drop_pcap":0,"drop_dash":0,"fw":"1.0.0"}',
    ],
    'blesniff': [
      '[Ch37 RSSI-60dBm] ADV_IND rnd-s:c0:ff:ee:00:11:22 name="Axon Body 3 with a long name" svc=0xFE6C,0x180F mfr=0059(Nordic)',
      '{"scan_win":30,"scan_int":100,"total":3,"pps":1,"drop_pcap":0,"state":"idle","session_bytes":0}',
    ],
    'flock': [
      '{"event":"config","beep_mask":29,"oui_count":42,"tiers":[{"tier":0,"method":"wifi_ssid","beep":1},{"tier":1,"method":"wifi_oui_addr1_addr3","beep":0}]}',
      '{"event":"detection","detection_method":"wifi_oui_addr2","detection_tier":2,"protocol":"wifi_2_4ghz","mac_address":"b4:1e:52:aa:bb:cc","oui":"B41E52","device_name":"","rssi":-66,"channel":6,"frequency":2437,"ssid":"Flock-1234"}',
    ],
    'axeoff': ['{"mac":"00:25:df:aa:bb:cc","rssi":-72,"type":"BLE_SVC","match":"FE6C"}  Axon signature (svc UUID FE6C)', '[state] IDLE -> TRACKING'],
    'skyspy': [
      '{"status":"scanning"}',
      '{"mac":"60:60:1f:11:22:33","rssi":-80,"band":"5GHz","channel":149,"drone_lat":40.2,"drone_long":-74.5,"drone_altitude":90,"basic_id":"C5DRONE","src":"c5"}',
    ],
    'ouispy': [
      'RST {"node":"AD5484","role":"home","mode":"scan","ch":1,"fw":"Oct28 20:37:51W"}',
      '{"mac":"AA:BB:CC:DD:EE:FF","alias":"","rssi":-70,"type":"OUI","match":"AA:BB:CC","desc":"Axon","node":"AD5484","dom":"ble"}',
    ],
    'legacy': ['OUI SPY UNIFIED FIRMWARE v2.0', 'OUI SPY - Firmware Selector'],
  };
  for (final e in samples.entries) {
    testWidgets('BoardView renders ${e.key} at 412 dp', (tester) async {
      tester.view.physicalSize = const Size(412 * 2.0, 1800 * 2.0);
      tester.view.devicePixelRatio = 2.0;
      addTearDown(tester.view.reset);
      Board.I.reset();
      for (final l in e.value) {
        Board.I.feed(l);
      }
      await tester.pumpWidget(MaterialApp(theme: buildTheme(), home: const Scaffold(body: BoardBody())));
      await tester.pump(const Duration(milliseconds: 300));
      expect(tester.takeException(), isNull);
      expect(find.byType(BoardBody), findsOneWidget); // the page title is the shared header's, not the body's
      // scroll to the bottom too
      await tester.drag(find.byType(ListView), const Offset(0, -3000));
      await tester.pump(const Duration(milliseconds: 300));
      expect(tester.takeException(), isNull);
    });
  }
}
