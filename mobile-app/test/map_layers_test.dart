import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/model.dart';
import 'package:ouispy_mesh/views/map_layers.dart';

void main() {
  group('placeLabels', () {
    const dot = Rect.fromLTRB(-8, -8, 8, 8);
    const size = Size(60, 16);
    const view = Size(400, 400);

    test('a lone label goes below its dot', () {
      final r = placeLabels([const Offset(200, 200)], [dot], [size], view);
      expect(r.single, isNotNull);
      expect(r.single!.top, greaterThan(208));
      expect(r.single!.center.dx, closeTo(200, 0.01));
    });

    test('labels of dots close together never overlap, and keep off the dots', () {
      final anchors = [const Offset(200, 200), const Offset(204, 202), const Offset(198, 206), const Offset(210, 198)];
      final r = placeLabels(anchors, List.filled(4, dot), List.filled(4, size), view);
      final placed = r.whereType<Rect>().toList();
      expect(placed.length, greaterThanOrEqualTo(2));
      for (var i = 0; i < placed.length; i++) {
        for (var j = i + 1; j < placed.length; j++) {
          expect(placed[i].overlaps(placed[j]), isFalse);
        }
        for (final a in anchors) {
          expect(placed[i].overlaps(dot.shift(a)), isFalse);
        }
      }
    });

    test('a label that fits nowhere is skipped, the first one wins', () {
      final anchors = List.filled(6, const Offset(200, 200));
      final r = placeLabels(anchors, List.filled(6, dot), List.filled(6, size), view);
      expect(r.first, isNotNull);
      expect(r.whereType<Rect>().length, 4); // below, above, right, left, then nothing
      expect(r.last, isNull);
    });

    test('labels stay inside the view', () {
      // in the corner only the left spot is inside the view
      final r = placeLabels([const Offset(380, 380)], [dot], [size], view);
      expect(r.single, isNotNull);
      expect(r.single!.right, lessThanOrEqualTo(380 - 8));
      expect((Offset.zero & view).contains(r.single!.bottomRight - const Offset(0.01, 0.01)), isTrue);
      // right in the corner nothing fits: the dot stays, its label doesn't
      expect(placeLabels([const Offset(395, 395)], [dot], [size], view).single, isNull);
    });

    test('centred labels sit on their point; skipped ones are not placed', () {
      final r = placeLabels([const Offset(100, 100), const Offset(300, 300)], [Rect.zero, Rect.zero], [size, size], view, centred: [true, true], skip: [false, true]);
      expect(r[0]!.center, const Offset(100, 100));
      expect(r[1], isNull);
    });
  });

  group('range', () {
    FleetNode node(String id, double lat, double lon) => FleetNode({'id': id, 'lat': lat, 'lon': lon});
    final placed = {'A': node('A', 40, -75), 'B': node('B', 40.001, -75)};

    test('distance from RSSI follows the log-distance model', () {
      expect(rssiDistance(-59, 'ble'), closeTo(1, 1e-9));
      expect(rssiDistance(-86, 'ble'), closeTo(10, 1e-6));
      expect(rssiDistance(-40, 'wifi'), closeTo(1, 1e-9));
    });

    test('a fix that carries per-node distances wins over RSSI', () {
      final t = Target('AA')
        ..dom = 'ble'
        ..seen = [Seen('A', -59, 0), Seen('B', -86, 0), Seen('Z', -50, 0)];
      expect(rangeFor(t, placed), {'A': closeTo(1, 1e-9), 'B': closeTo(10, 1e-6)});
      expect(
        rangeFor(
          t,
          placed,
          fixNodes: [
            {'id': 'A', 'd': 4.5},
            {'id': 'Z', 'd': 3},
          ],
        ),
        {'A': 4.5},
      );
    });

    test('signal centre leans to the loudest node', () {
      final t = Target('AA')..seen = [Seen('A', -50, 0), Seen('B', -80, 0)];
      final p = signalCentre(t, placed)!;
      expect(p.latitude, lessThan(40.0001));
      expect(signalCentre(Target('BB'), placed), isNull);
    });
  });

  test('links cycle home, mesh, off', () {
    expect(nextLinkMode(LinkMode.home), LinkMode.mesh);
    expect(nextLinkMode(LinkMode.mesh), LinkMode.off);
    expect(nextLinkMode(LinkMode.off), LinkMode.home);
  });

  test('node trails: dashboard tracks, then newer moves the phone saw', () {
    final tr = NodeTrails();
    tr.ingest({
      'tracks': {
        'N1': [
          [100, 40.0, -75.0, 5],
          [110, 40.0001, -75.0, 5],
        ],
      },
    });
    expect(tr.of('N1').length, 2);
    tr.record([
      FleetNode({
        'id': 'N1',
        'lat': 40.001,
        'lon': -75.0,
        'cfg': {'fw': '1.0Y'},
      }),
      FleetNode({'id': 'N2', 'lat': 40.0, 'lon': -75.0}), // not a GPS node: no trail
    ], 120);
    expect(tr.of('N1').length, 3);
    expect(tr.of('N2'), isEmpty);
  });

  test('distances read in metres or km', () {
    expect(fmtDist(42.4), '42 m');
    expect(fmtDist(1530), '1.5 km');
    expect(fmtDist(12500), '13 km');
  });
}
