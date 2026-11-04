// The triangulation model (the dashboard's settings.tri), calibrate-at-a-spot's fit, the
// model hook LocalMesh's Tri reads, and the History exports' CSV.
import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/export.dart';
import 'package:ouispy_mesh/data/store.dart';
import 'package:ouispy_mesh/data/tri.dart';
import 'package:shared_preferences/shared_preferences.dart';

double rssiAt(double p0, double n, double d) => p0 - 10 * n * math.log(d) / math.ln10;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  SharedPreferences.setMockInitialValues({});

  test('settings.tri round trip, clamps', () {
    final t = TriModel.fromJson({
      'p0_ble': -61,
      'p0_wifi': -45,
      'n': 3.1,
      'sigma': 5,
      'max_err': 10,
      'smooth': 0.5,
      'offsets': {'ad5484': -3},
      'devices': {
        'aa:bb:cc:dd:ee:ff': {'p0': -58.2, 'n': 2.4, 'rms': 1.2, 'nodes': 3, 'ts': 100},
      },
    });
    expect(t.p0Ble, -61);
    expect(t.offsets['AD5484'], -3);
    expect(t.devices['AA:BB:CC:DD:EE:FF']!.p0, -58.2);
    final j = t.toJson();
    expect(j['max_err'], 10);
    expect((j['devices'] as Map)['AA:BB:CC:DD:EE:FF']['nodes'], 3);
    final c = t.copyWith(n: 9, p0Ble: -5, offsets: {'X': 99}).clamped();
    expect(c.n, 5);
    expect(c.p0Ble, -20);
    expect(c.offsets['X'], 40);
    expect(t.forDevice('aa:bb:cc:dd:ee:ff', 'ble').cal, true);
    expect(t.forDevice('11:22:33:44:55:66', 'wifi').p0, -45);
  });

  test('calibration fit: both P0 and n when the distances differ, else P0 only', () {
    final pts = [
      for (final d in [2.0, 5.0, 12.0]) (d: d, rssi: rssiAt(-55, 2.2, d)),
    ];
    final f = fitCalibration(pts, 2.7, ts: 1)!;
    expect(f.fitted, true);
    expect(f.cal.p0, closeTo(-55, 0.11));
    expect(f.cal.n, closeTo(2.2, 0.011));
    expect(f.cal.rms, lessThan(0.1));
    expect(f.cal.nodes, 3);

    final same = [
      for (final d in [4.0, 4.5]) (d: d, rssi: rssiAt(-60, 2.0, d)),
    ];
    final g = fitCalibration(same, 2.7)!;
    expect(g.fitted, false);
    expect(g.cal.n, 2.7);
    expect(fitCalibration(const [], 2.7), isNull);
  });

  test('a Tri without a model reads the phone model; calibrations and offsets apply', () async {
    final nodes = {'A': (0.0, 0.0), 'B': (0.0, 0.0002), 'C': (0.0002, 0.0)};
    final tri = Tri((id) => nodes[id]);
    triModel = const TriModel(p0Ble: -59, n: 2.7);
    expect(tri.model.p0Ble, -59);
    for (final id in ['A', 'B', 'C']) {
      tri.add('AA:BB:CC:DD:EE:FF', id, -70, 'ble', 10);
    }
    final generic = tri.fix('AA:BB:CC:DD:EE:FF', 10);
    expect(generic['ok'], true);
    expect(generic['cal'], false);
    final dGeneric = (generic['nodes'] as List).first['d'] as double;

    triModel = triModel.copyWith(devices: {'AA:BB:CC:DD:EE:FF': const TriCal(p0: -50, n: 2.0, rms: 1)});
    tri.reset('AA:BB:CC:DD:EE:FF');
    final cal = tri.fix('AA:BB:CC:DD:EE:FF', 10);
    expect(cal['cal'], true);
    // louder at 1 m: the same reading is further away
    expect((cal['nodes'] as List).first['d'] as double, greaterThan(dGeneric));

    triModel = triModel.copyWith(offsets: {'A': -10});
    final off = tri.fix('AA:BB:CC:DD:EE:FF', 10);
    final a = (off['nodes'] as List).firstWhere((n) => n['id'] == 'A')['d'] as double;
    final b = (off['nodes'] as List).firstWhere((n) => n['id'] == 'B')['d'] as double;
    expect(a, lessThan(b)); // A reads 10 dB deaf: corrected it is closer
    triModel = const TriModel();
  });

  test('trails keep new fixes only', () {
    final t = TriTrails.I;
    t.add('11:22:33:44:55:66', 1, 10, 10, 3);
    t.add('11:22:33:44:55:66', 1, 10.1, 10, 3); // not newer
    t.add('11:22:33:44:55:66', 2, 10, 10, 3); // the same spot
    t.add('11:22:33:44:55:66', 3, 10.0001, 10, 3);
    expect(t.of('11:22:33:44:55:66').length, 2);
    t.clear('11:22:33:44:55:66');
    expect(t.of('11:22:33:44:55:66'), isEmpty);
  });

  test('CSV quoting and the phone detections export', () {
    expect(csvField('a,b'), '"a,b"');
    expect(csvField('say "hi"'), '"say ""hi"""');
    expect(csvField(true), '1');
    expect(csvField(null), '');
    final s = MeshStore.I;
    s.dets = [
      {'id': 1, 'ts': 1000, 'node': 'AD5484', 'mac': 'AA:BB:CC:DD:EE:FF', 'rssi': -60, 'type': 'LE', 'descr': 'Tag, small', 'dom': 'ble', 'is_new': true},
      {'id': 2, 'ts': 2000, 'node': '4B4D04', 'mac': '11:22:33:44:55:66', 'rssi': -70, 'type': 'BEACON', 'descr': 'AP', 'dom': 'wifi', 'is_new': false},
    ];
    s.aliases = {'AA:BB:CC:DD:EE:FF': 'Keys'};
    expect(localDetRows(const ExportFilter()).length, 2);
    expect(localDetRows(const ExportFilter(dom: 'wifi')).single['mac'], '11:22:33:44:55:66');
    expect(localDetRows(const ExportFilter(node: 'AD5484')).single['mac_alias'], 'Keys');
    expect(localDetRows(const ExportFilter(q: 'keys')).length, 1);
    expect(localDetRows(const ExportFilter(since: 1500)).length, 1);
    final csv = toCsv(detCols, localDetRows(const ExportFilter()));
    final lines = csv.trim().split('\r\n');
    expect(lines.first, detCols.join(','));
    expect(lines[1], contains('"Tag, small"'));
    expect(lines.length, 3);
    expect(const ExportFilter(node: 'kind:detector', dom: 'ble').query, 'dom=ble');
  });
}
