// The app's alert tones, made here as PCM WAV: the Tab5's jingles (tab5-dashboard/src/sfx.cpp)
// at five volumes. Flutter has no audio player of its own and the app carries no audio
// package, so the tones ship as Android raw resources and play as notification-channel
// sounds (sounds.dart). Pure Dart: tool/gen_tones.dart writes the files from this, and a test
// checks the shipped files still match.
import 'dart:math' as math;
import 'dart:typed_data';

/// The events that make a sound.
enum Sfx { detect, watch, drone, nodeDown, nodeUp, lost }

/// (Hz, ms); 0 Hz is a rest.
const tunes = <Sfx, List<(int, int)>>{
  Sfx.detect: [(1319, 55), (1760, 55), (2637, 110)], // up-chirp
  Sfx.watch: [(1760, 80), (0, 40), (1760, 80), (0, 40), (2637, 170)], // double knock, then high
  Sfx.drone: [(880, 90), (0, 25), (1175, 90), (0, 25), (880, 90), (0, 25), (1568, 160)], // warble, then high
  Sfx.nodeDown: [(784, 90), (587, 90), (392, 200)], // falling
  Sfx.nodeUp: [(392, 70), (587, 70), (784, 130)], // rising
  Sfx.lost: [(1568, 140), (1047, 140), (1568, 140), (1047, 140), (1568, 220)], // siren
};

const volumes = [20, 40, 60, 80, 100];
const toneRate = 22050;

/// The resource name, `sfx_<event>_<volume>` by Android's rules (lower case, digits, underscores).
String toneName(Sfx s, int vol) =>
    'sfx_${switch (s) {
      Sfx.detect => 'detect',
      Sfx.watch => 'watch',
      Sfx.drone => 'drone',
      Sfx.nodeDown => 'node_down',
      Sfx.nodeUp => 'node_up',
      Sfx.lost => 'lost',
    }}_$vol';

/// Snap any volume to the nearest one shipped.
int snapVolume(int v) => volumes.reduce((a, b) => (v - a).abs() <= (v - b).abs() ? a : b);

/// A 16-bit mono WAV of [notes] at [vol] percent. Each note is a soft square (fundamental
/// plus odd harmonics, so it stays clean at 22 kHz) with 4 ms edges so it does not click.
Uint8List toneWav(List<(int, int)> notes, int vol, {int rate = toneRate}) {
  final amp = 0.9 * (vol.clamp(0, 100) / 100) * 32767;
  final samples = <int>[];
  final edge = (rate * 0.004).round();
  for (final (hz, ms) in notes) {
    final n = (rate * ms / 1000).round();
    for (var i = 0; i < n; i++) {
      if (hz == 0) {
        samples.add(0);
        continue;
      }
      final t = i / rate;
      final w = 2 * math.pi * hz * t;
      var v = math.sin(w) + math.sin(3 * w) / 3 + math.sin(5 * w) / 5;
      v *= 0.8; // the harmonic sum peaks near 1.2
      final env = math.min(1.0, math.min(i / edge, (n - 1 - i) / edge));
      samples.add((v * env * amp).round().clamp(-32768, 32767));
    }
  }
  samples.addAll(List.filled((rate * 0.03).round(), 0)); // a little tail so players don't clip the end
  final data = samples.length * 2;
  final b = ByteData(44 + data);
  void str(int o, String s) {
    for (var i = 0; i < s.length; i++) {
      b.setUint8(o + i, s.codeUnitAt(i));
    }
  }

  str(0, 'RIFF');
  b.setUint32(4, 36 + data, Endian.little);
  str(8, 'WAVE');
  str(12, 'fmt ');
  b.setUint32(16, 16, Endian.little);
  b.setUint16(20, 1, Endian.little); // PCM
  b.setUint16(22, 1, Endian.little); // mono
  b.setUint32(24, rate, Endian.little);
  b.setUint32(28, rate * 2, Endian.little);
  b.setUint16(32, 2, Endian.little);
  b.setUint16(34, 16, Endian.little);
  str(36, 'data');
  b.setUint32(40, data, Endian.little);
  for (var i = 0; i < samples.length; i++) {
    b.setInt16(44 + i * 2, samples[i], Endian.little);
  }
  return b.buffer.asUint8List();
}
