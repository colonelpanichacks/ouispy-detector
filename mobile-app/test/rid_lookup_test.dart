// Remote ID maker lookup: the offline serial decode, ID type labels, and the FAA cache
// (one request per serial, retry after an hour, persisted), against a mocked FAA.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:ouispy_mesh/data/model.dart';
import 'package:ouispy_mesh/data/rid_lookup.dart';
import 'package:shared_preferences/shared_preferences.dart';

const serial = '1581F6W842591002QD17';

Map<String, dynamic> faaReply(List<Map<String, dynamic>> items) => {
  'apiVersion': '1.0.0',
  'data': {'currentItemCount': items.length, 'items': items},
};

final avata = {
  'trackingNumber': 'RID000001376',
  'makeName': 'DJI',
  'modelName': 'Avata 2',
  'status': 'accepted',
  'certificationType': {'id': 1},
};

Future<void> settle() => Future.delayed(const Duration(milliseconds: 20));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('offline decode', () {
    test('CTA-2063-A serial parts', () {
      final p = RidSerial.parse(serial)!;
      expect(p.code, '1581');
      expect(p.maker, 'DJI');
      expect(p.lengthChar, 'F');
      expect(p.declaredLen, 15);
      expect(p.body, '6W842591002QD17');
      expect(p.lengthOk, isTrue);
    });
    test('length characters', () {
      expect(RidSerial.lengthOf('1'), 1);
      expect(RidSerial.lengthOf('9'), 9);
      expect(RidSerial.lengthOf('A'), 10);
      expect(RidSerial.lengthOf('F'), 15);
      expect(RidSerial.lengthOf('0'), isNull);
      expect(RidSerial.lengthOf('G'), isNull);
    });
    test('unknown code, bad length, junk', () {
      final p = RidSerial.parse('ZZZZ3ABCDE')!;
      expect(p.maker, isNull);
      expect(p.lengthOk, isFalse);
      expect(RidSerial.parse('1581'), isNull);
      expect(RidSerial.parse('1581-F6W8'), isNull);
      expect(RidSerial.parse(' 1581f6w842591002qd17 ')!.maker, 'DJI');
    });
    test('ID type labels', () {
      expect(ridIdTypeLabel(1), 'Serial number (ANSI/CTA-2063-A)');
      expect(ridIdTypeLabel(2), 'CAA registration');
      expect(ridIdTypeLabel(3), 'UTM (USS) UUID');
      expect(ridIdTypeLabel(4), 'Specific session ID');
      expect(ridIdTypeLabel('1'), 'Serial number (ANSI/CTA-2063-A)');
      expect(ridIdTypeLabel(9), '');
      expect(ridIdTypeLabel(null), '');
    });
    test('cookie header from Set-Cookie', () {
      expect(RidLookup.cookieHeader('a=1; Path=/; HttpOnly,b=2; Expires=Wed, 21 Oct 2026 07:28:00 GMT'), 'a=1; b=2');
      expect(RidLookup.cookieHeader(null), '');
    });
  });

  group('describe and the FAA cache', () {
    late Model m;
    late int calls, listdocs;
    late bool fail;
    late double clock;
    late List<Map<String, dynamic>> items;

    RidLookup make() => RidLookup(
      model: () => m,
      viaDashboard: (_, _) async => null,
      now: () => clock,
      client: MockClient((r) async {
        if (r.url.path == '/listdocs') {
          listdocs++;
          return http.Response('<html></html>', 200, headers: {'set-cookie': 'sess=abc; Path=/'});
        }
        calls++;
        expect(r.url.path, '/api/v1/serialNumbers');
        expect(r.url.queryParameters['findBy'], 'serialNumber');
        expect(r.url.queryParameters['serialNumber'], serial);
        expect(r.headers['Cookie'], 'sess=abc');
        await Future.delayed(const Duration(milliseconds: 5));
        if (fail) return http.Response('oops', 503);
        return http.Response(jsonEncode(faaReply(items)), 200);
      }),
    );

    Drone drone({int? idt = 1, String id = serial, String vendor = ''}) {
      m.ingestDrone({'mac': 'AA:BB:CC:00:11:22', 'id': id, 'idt': idt, 'vendor': vendor, 'first': 1.0, 'last': 1.0});
      return m.drones['AA:BB:CC:00:11:22']!;
    }

    setUp(() {
      SharedPreferences.setMockInitialValues({});
      m = Model();
      calls = 0;
      listdocs = 0;
      fail = false;
      clock = 1000000;
      items = [avata];
    });

    test('instant: serial code, then radio vendor, then nothing', () {
      final r = make();
      expect(r.describe(drone()).maker, 'DJI');
      expect(r.describe(drone()).source, RidSource.serial);
      expect(r.describe(drone()).code, '1581');
      expect(r.describe(drone()).idt, 'Serial number (ANSI/CTA-2063-A)');
      // a CAA registration is not read as a serial
      final caa = r.describe(drone(idt: 2, id: '1581XYZ123', vendor: 'Espressif'));
      expect(caa.source, RidSource.vendor);
      expect(caa.maker, 'Espressif');
      expect(r.describe(drone(idt: 4, id: 'session', vendor: '')).known, isFalse);
    });

    test('lookup lands on the drone, one request per serial, persisted', () async {
      final r = make();
      final d = drone();
      var heard = 0;
      r.addListener(() => heard++);
      r.ensure(d);
      r.ensure(d); // in flight: no second request
      expect(r.pending(serial), isTrue);
      await settle();
      expect(calls, 1);
      expect(listdocs, 1);
      expect(heard, greaterThan(0));
      expect(r.state(d), FaaState.found);
      final w = r.describe(m.drones[d.mac]!);
      expect((w.maker, w.model, w.source, w.faaStatus), ('DJI', 'Avata 2', RidSource.faa, 'accepted'));
      expect(m.drones[d.mac]!.maker, 'DJI Avata 2'); // the Drone.maker getter reads the merge
      // a later sighting of the same serial: answered from the cache, no request
      r.ensure(d);
      await settle();
      expect(calls, 1);
      final p = await SharedPreferences.getInstance();
      final blob = jsonDecode(p.getString(RidLookup.prefsKey)!) as Map;
      expect(blob[serial]['ok'], isTrue);
      expect(blob[serial]['ts'], clock);
      // a new instance (an app restart) reads the cache: instant FAA answer, no request
      m = Model();
      final r2 = make();
      await r2.load();
      final d2 = drone();
      expect(r2.describe(d2).source, RidSource.faa);
      r2.ensure(d2);
      await settle();
      expect(calls, 1);
      expect(m.drones[d2.mac]!.maker, 'DJI Avata 2');
    });

    test('a failure retries only after an hour', () async {
      final r = make();
      fail = true;
      final d = drone();
      r.ensure(d);
      await settle();
      expect(calls, 1);
      expect(r.state(d), FaaState.failed);
      expect(r.describe(d).source, RidSource.serial); // still instant
      clock += 1800;
      r.ensure(d);
      await settle();
      expect(calls, 1);
      fail = false;
      clock += 1801;
      r.ensure(d);
      await settle();
      expect(calls, 2);
      expect(r.state(m.drones[d.mac]!), FaaState.found);
    });

    test('no record: answered, not retried by ensure', () async {
      items = [];
      final r = make();
      final d = drone();
      r.ensure(d);
      await settle();
      expect(r.state(m.drones[d.mac]!), FaaState.missing);
      r.ensure(m.drones[d.mac]!);
      await settle();
      expect(calls, 1);
      // the button forces a fresh ask
      items = [avata];
      final e = await r.lookup(m.drones[d.mac]!, force: true);
      expect(e!.items.first['modelName'], 'Avata 2');
      expect(calls, 2);
    });

    test('only ID types 1 and 2 are looked up on their own, and only with the setting on', () async {
      final r = make();
      r.ensure(drone(idt: 3, id: 'c0ffee00-uuid'));
      await settle();
      expect(calls, 0);
      await r.setFaaLookup(false);
      r.ensure(drone());
      await settle();
      expect(calls, 0);
      final p = await SharedPreferences.getInstance();
      expect(p.getBool(RidLookup.settingKey), isFalse);
      await r.setFaaLookup(true);
      r.ensure(drone());
      await settle();
      expect(calls, 1);
    });

    test('the dashboard answer is preferred', () async {
      final r = RidLookup(
        model: () => m,
        now: () => clock,
        viaDashboard: (mac, s) async => faaReply([avata]),
        client: MockClient((_) async {
          calls++;
          return http.Response('{}', 500);
        }),
      );
      final d = drone();
      await r.lookup(d);
      expect(calls, 0);
      expect(m.drones[d.mac]!.maker, 'DJI Avata 2');
    });
  });
}
