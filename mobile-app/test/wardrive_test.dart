// Wardrive with no hardware: WD lines in the firmware's format (src/wardrive.cpp printObs)
// into devices and sessions, and the WiGLE CSV as home-dashboard/wardrive.py writes it.
import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/backend.dart';
import 'package:ouispy_mesh/data/wardrive.dart';

const t0 = 1762250000.0; // 2025-11-04 09:53:20 UTC

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('WD lines become devices, best RSSI keeps its place, sessions split on a gap', () {
    final w = Wardrive(persist: false);
    final c = Console();
    w.attachConsole(c);

    expect(
      w.ingestLine(
        'WD {"t":"wifi","mac":"aa:bb:cc:00:11:22","ssid":"Cafe, \\"Free\\"","ch":6,"rssi":-80,"sec":"open","hidden":0,"wps":1,"std":"n","rate":54,"cc":"US","vend":["0050F2"],"lat":40.1,"lon":-74.2,"acc":5.0,"new":1}',
        ts: t0,
      ),
      isTrue,
    );
    // louder, somewhere else: the device moves to where it was loudest
    w.ingestLine(
      'WD {"t":"wifi","mac":"AA:BB:CC:00:11:22","ssid":"Cafe, \\"Free\\"","ch":6,"rssi":-60,"sec":"open","hidden":0,"wps":1,"lat":40.2,"lon":-74.3,"acc":4.0,"new":0}',
      ts: t0 + 10,
    );
    // quieter: the place stays
    w.ingestLine('WD {"t":"wifi","mac":"AA:BB:CC:00:11:22","ssid":"","ch":6,"rssi":-90,"sec":"open","hidden":0,"wps":1,"lat":41,"lon":-75,"new":0}', ts: t0 + 20);
    w.ingestLine('WD {"t":"wifi","mac":"11:22:33:44:55:66","ssid":"Lab5","ch":149,"rssi":-70,"sec":"WPA2/WPA3","hidden":1,"wps":0,"new":1}', ts: t0 + 30);
    w.ingestLine(
      'WD {"t":"ble","mac":"DE:AD:BE:EF:00:01","name":"Tag","rssi":-55,"rnd":1,"mfr":76,"app":512,"tx":-4,"fl":6,"svc":["FD6F"],"lat":40.3,"lon":-74.4,"acc":3.0,"new":1}',
      ts: t0 + 40,
    );
    w.ingestLine('WD {"t":"zigbee","mac":"00:12:4B:00:01:02:03:04","rssi":-77,"ch":15,"new":1}', ts: t0 + 45);
    expect(w.ingestLine('DET {"mac":"AA:BB:CC:00:11:22"}'), isFalse);
    expect(w.ingestLine('WD {"t":"wifi","mac":"bad"}'), isFalse);
    expect(w.ingestLine('WD {torn'), isFalse);

    expect(w.devices.length, 4);
    final cafe = w.devices['wifi|AA:BB:CC:00:11:22']!;
    expect(cafe.name, 'Cafe, "Free"'); // an empty SSID never wipes the name
    expect(cafe.rssi, -60);
    expect(cafe.lat, 40.2);
    expect(cafe.lon, -74.3);
    expect(cafe.acc, 4.0);
    expect(cafe.seen, 3);
    expect(cafe.first, t0);
    expect(cafe.last, t0 + 20);
    expect(cafe.band, '2.4');
    expect(cafe.wps, isTrue);
    expect(cafe.std, 'n');
    expect(cafe.cc, 'US');
    expect(cafe.vend, ['0050F2']);
    expect(cafe.open, isTrue);

    final lab = w.devices['wifi|11:22:33:44:55:66']!;
    expect(lab.band, '5');
    expect(lab.hidden, isTrue);
    expect(lab.placed, isFalse);

    final tag = w.devices['ble|DE:AD:BE:EF:00:01']!;
    expect(tag.band, 'ble');
    expect(tag.rnd, isTrue);
    expect(tag.mfr, 76);
    expect(tag.svc, ['FD6F']);

    final zb = w.devices['zigbee|00:12:4B:00:01:02:03:04']!;
    expect(zb.band, 'zigbee');

    expect(w.sessions.length, 1);
    final s1 = w.currentId!;
    expect(w.inScope(session: s1).length, 4);
    expect(w.obs.length, 4); // the positioned sightings

    final st = w.stats(session: s1, now: t0 + 60);
    expect(st.total, 4);
    expect(st.open, 1);
    expect(st.placed, 2);
    expect(st.bySec, {'open': 1, 'WPA2/WPA3': 1});
    expect(st.byBand, {'2.4': 1, '5': 1, 'ble': 1, 'zigbee': 1});
    expect(st.byCh, {6: 1, 149: 1});
    expect(st.newPerHour.reduce((a, b) => a + b), 4);

    // 31 min of silence: a new session
    w.ingestLine('WD {"t":"wifi","mac":"AA:BB:CC:00:11:22","ssid":"Cafe","ch":6,"rssi":-65,"sec":"open","hidden":0,"wps":0,"new":0}', ts: t0 + 20 + wdSessionGapS + 60);
    expect(w.sessions.length, 2);
    expect(w.currentId, isNot(s1));
    expect(w.inScope(session: w.currentId).length, 1);
    expect(cafe.sessions.length, 2);

    // paused: nothing kept
    w.setPaused(true);
    expect(w.ingestLine('WD {"t":"ble","mac":"DE:AD:BE:EF:00:09","name":"","rssi":-50,"rnd":1,"new":1}'), isFalse);

    // a phone fix fills in a line with none of its own
    w.setPaused(false);
    w.fix = const WdFix(51.5, -0.1, 8);
    w.fixTs = t0 + 5000;
    w.ingestLine('WD {"t":"ble","mac":"DE:AD:BE:EF:00:09","name":"","rssi":-50,"rnd":1,"new":1}', ts: t0 + 5001);
    expect(w.devices['ble|DE:AD:BE:EF:00:09']!.lat, 51.5);

    // round trip through the saved form
    final w2 = Wardrive(persist: false)..loadJson(w.toJson());
    expect(w2.devices.length, w.devices.length);
    expect(w2.obs.length, w.obs.length);
    expect(w2.devices['wifi|AA:BB:CC:00:11:22']!.lat, 40.2);
    expect(w2.currentId, w.currentId);
  });

  test('WiGLE CSV: the 1.4 pre-header, the column header, one row per positioned sighting', () {
    final w = Wardrive(persist: false);
    w.ingestLine(
      'WD {"t":"wifi","mac":"AA:BB:CC:00:11:22","ssid":"Cafe, \\"Free\\"","ch":6,"rssi":-80,"sec":"WPA2","hidden":0,"wps":0,"lat":40.1,"lon":-74.2,"acc":5,"new":1}',
      ts: t0,
    );
    w.ingestLine('WD {"t":"ble","mac":"DE:AD:BE:EF:00:01","name":"Tag","rssi":-55,"rnd":1,"lat":40.3,"lon":-74.4,"new":1}', ts: t0 + 1);
    w.ingestLine('WD {"t":"wifi","mac":"11:22:33:44:55:66","ssid":"NoFix","ch":11,"rssi":-70,"sec":"open","hidden":0,"wps":0,"new":1}', ts: t0 + 2);
    w.ingestLine('WD {"t":"zigbee","mac":"00:12:4B:00:01:02:03:04","rssi":-77,"lat":40.3,"lon":-74.4,"new":1}', ts: t0 + 3);
    final csv = w.wigleCsv(session: w.currentId);
    final lines = csv.split('\n');
    expect(lines[0], 'WigleWifi-1.4,appRelease=1.0,model=OUI-SPY Mesh,release=1.0,device=OUI-SPY wardriver,display=,board=,brand=colonelpanic.tech');
    expect(lines[1], 'MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type');
    expect(lines[2], 'AA:BB:CC:00:11:22,"Cafe, ""Free""",[WPA2-PSK-CCMP][ESS],2025-11-04 09:53:20,6,-80,40.1000000,-74.2000000,0,5.0,WIFI');
    expect(lines[3], 'DE:AD:BE:EF:00:01,Tag,Misc [LE],2025-11-04 09:53:21,0,-55,40.3000000,-74.4000000,0,0.0,BLE');
    expect(lines.length, 5); // no row for the unplaced network or the Zigbee find; trailing newline
    expect(lines[4], '');
    expect(Wardrive.csvRows(csv), 2);
    expect(w.wigleCsv(), csv); // all sessions = the one

    // open and WPA3 auth strings
    final d = w.devices['wifi|11:22:33:44:55:66']!;
    expect(Wardrive.wigleRow(d, t0, null, 1, 2, null), '11:22:33:44:55:66,NoFix,[ESS],2025-11-04 09:53:20,11,,1.0000000,2.0000000,0,0.0,WIFI\n');
    d.sec = 'WPA2/WPA3';
    expect(Wardrive.wigleRow(d, t0, -1, 1, 2, 3)!.split(',')[2], '[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]');
    expect(Wardrive.wigleFileName(DateTime(2026, 1, 2, 3, 4, 5)), 'WigleWifi_20260102_030405.csv');
  });

  test('dashboard /api/wardrive merges into the same devices', () {
    final w = Wardrive(persist: false);
    w.ingestLine('WD {"t":"wifi","mac":"AA:BB:CC:00:11:22","ssid":"Cafe","ch":6,"rssi":-80,"sec":"open","hidden":0,"wps":0,"new":1}', ts: t0);
    w.mergeDashboard({
      'session': 7,
      'session_start': t0 - 100,
      'devices': [
        {
          'mac': 'AA:BB:CC:00:11:22',
          'kind': 'wifi',
          'name': 'Cafe',
          'sec': 'open',
          'ch': 6,
          'rssi': -50,
          'lat': 1.0,
          'lon': 2.0,
          'acc': 9.0,
          'first': t0 - 50,
          'last': t0 + 5,
          'seen': 12,
          'now': true,
          'vendor': 'Acme',
        },
        {'mac': 'DE:AD:BE:EF:00:01', 'kind': 'ble', 'name': '', 'rssi': -60, 'lat': null, 'lon': null, 'first': t0, 'last': t0, 'seen': 1, 'rnd': true, 'now': false},
      ],
    });
    expect(w.devices.length, 2);
    final d = w.devices['wifi|AA:BB:CC:00:11:22']!;
    expect(d.rssi, -50);
    expect(d.lat, 1.0);
    expect(d.seen, 12);
    expect(d.first, t0 - 50);
    expect(d.maker, 'Acme');
    expect(d.sessions, contains('D7'));
    expect(w.currentId, 'D7');
    expect(w.sessions['D7']!.remote, isTrue);
    // a merged device with a place and no sighting here still exports
    expect(Wardrive.csvRows(w.wigleCsv(session: 'D7')), 1);

    final m = w.metrics(t0 - 3600, t0 + 3600, bucket: 3600, start: t0 - 3600, nb: 3);
    expect(m['total'], 2);
    expect(m['open'], 1);
    expect(m['placed'], 1);
    expect((m['by_sec'] as List).first, {'name': 'open', 'count': 1});
  });
}
