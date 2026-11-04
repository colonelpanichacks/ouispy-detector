import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/model.dart';
import 'package:ouispy_mesh/data/tones.dart';

Map<String, dynamic> _fleet(Map<String, bool> online, {double ts = 0}) => {
  'ts': ts,
  'nodes': [
    for (final e in online.entries) {'id': e.key, 'online': e.value, 'age': 1.0},
  ],
};

void main() {
  group('tones', () {
    test('a WAV with a sane header and length', () {
      final w = toneWav(tunes[Sfx.detect]!, 80);
      final b = ByteData.sublistView(w);
      expect(String.fromCharCodes(w.sublist(0, 4)), 'RIFF');
      expect(String.fromCharCodes(w.sublist(8, 12)), 'WAVE');
      expect(b.getUint16(22, Endian.little), 1);
      expect(b.getUint32(24, Endian.little), toneRate);
      expect(b.getUint32(40, Endian.little), w.length - 44);
      final ms = tunes[Sfx.detect]!.fold<int>(0, (a, n) => a + n.$2) + 30;
      expect((w.length - 44) / 2 / toneRate * 1000, closeTo(ms, 2));
    });

    test('louder is louder, and never clips', () {
      int peak(Uint8List w) {
        final b = ByteData.sublistView(w);
        var p = 0;
        for (var i = 44; i < w.length; i += 2) {
          p = b.getInt16(i, Endian.little).abs() > p ? b.getInt16(i, Endian.little).abs() : p;
        }
        return p;
      }

      final p20 = peak(toneWav(tunes[Sfx.drone]!, 20)), p100 = peak(toneWav(tunes[Sfx.drone]!, 100));
      expect(p100, greaterThan(p20 * 4));
      expect(p100, lessThan(32767));
    });

    test('volumes snap to the shipped ones', () {
      expect(snapVolume(0), 20);
      expect(snapVolume(55), 60);
      expect(snapVolume(100), 100);
      expect(toneName(Sfx.nodeDown, 40), 'sfx_node_down_40');
    });

    test('the shipped res/raw files match the generator (dart run tool/gen_tones.dart)', () {
      for (final s in Sfx.values) {
        for (final v in volumes) {
          final f = File('android/app/src/main/res/raw/${toneName(s, v)}.wav');
          expect(f.existsSync(), isTrue, reason: f.path);
          expect(f.readAsBytesSync(), toneWav(tunes[s]!, v), reason: f.path);
        }
      }
      expect(File('android/app/src/main/res/raw/keep.xml').readAsStringSync(), contains('@raw/sfx_*'));
    });
  });

  group('mesh events', () {
    test('a node flipping is one event per 30 s, both ways', () async {
      final m = Model();
      final ev = <MeshEvent>[];
      m.meshEvents.listen(ev.add);
      m.ingestFleet(_fleet({'AAAAAA': true, 'BBBBBB': true}));
      expect(ev, isEmpty); // first sight of the roster is not news
      m.ingestFleet(_fleet({'AAAAAA': false, 'BBBBBB': true}));
      m.ingestFleet(_fleet({'AAAAAA': true, 'BBBBBB': true})); // flapping: inside 30 s
      m.ingestFleet(_fleet({'AAAAAA': true, 'BBBBBB': false}));
      await Future<void>.delayed(Duration.zero);
      expect(ev.map((e) => (e.kind, e.id)).toList(), [(MeshEventKind.nodeDown, 'AAAAAA'), (MeshEventKind.nodeDown, 'BBBBBB')]);
    });

    test('a node toast is not repeated when another source said it', () async {
      final m = Model();
      final toasts = <String>[];
      m.toasts.listen(toasts.add);
      m.ingestFleet(_fleet({'AAAAAA': true}));
      m.toast('AAAAAA went offline'); // the USB node's own toast, or the dashboard's node_event
      m.ingestFleet(_fleet({'AAAAAA': false}));
      await Future<void>.delayed(const Duration(milliseconds: 500));
      expect(toasts, ['AAAAAA went offline']);
    });

    test('a drone is news once a session, and not from a bulk load', () async {
      final m = Model();
      final ev = <MeshEvent>[];
      m.meshEvents.listen(ev.add);
      final t = DateTime.now().millisecondsSinceEpoch / 1000;
      m.ingestDrone({'mac': 'aa:00:00:00:00:01', 'last': t}, notify: false); // in the state at connect
      m.ingestDrone({'mac': 'AA:00:00:00:00:01', 'last': t + 1});
      m.ingestDrone({'mac': 'AA:00:00:00:00:02', 'last': t - 3600}); // an old report: not flying
      m.ingestDrone({'mac': 'AA:00:00:00:00:03', 'last': t, 'alias': 'x', 'meta': true}); // an edit
      m.ingestDrone({'mac': 'AA:00:00:00:00:04', 'last': t, 'id': 'SN123'});
      m.ingestDrone({'mac': 'AA:00:00:00:00:04', 'last': t + 2});
      await Future<void>.delayed(Duration.zero);
      expect(ev.map((e) => e.id).toList(), ['AA:00:00:00:00:04']);
      expect(ev.single.kind, MeshEventKind.drone);
      expect(ev.single.title, contains('SN123'));
    });

    test('lost_node becomes a toast and an event', () async {
      final m = Model();
      final ev = <MeshEvent>[];
      final toasts = <String>[];
      m.meshEvents.listen(ev.add);
      m.toasts.listen(toasts.add);
      m.lostNode({'node': 'CCCCCC', 'how': 'heard by AD5484'});
      await Future<void>.delayed(Duration.zero);
      expect(ev.single.kind, MeshEventKind.lostNode);
      expect(toasts.single, contains('CCCCCC'));
    });
  });
}
