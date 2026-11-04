// The FPV link protocol port against the receiver's own encoder rules (link_proto.h):
// packets with their CRC, RAW rows, NL rows round-tripped through a port of
// link_row_encode, and INFO.
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/fpv.dart';

/// link_row_encode, ported for the test: what the receiver sends.
Uint8List nlEncode(Uint8List px, int delta, int k) {
  final q = 2 * delta + 1;
  final bits = <int>[];
  void put(int v, int n) {
    for (var i = n - 1; i >= 0; i--) {
      bits.add((v >> i) & 1);
    }
  }

  var r = px[0];
  for (var x = 1; x < fpvW; x++) {
    final d = px[x] - r;
    final qe = d >= 0 ? (d + delta) ~/ q : -((-d + delta) ~/ q);
    r = (r + qe * q).clamp(0, 255);
    final u = qe >= 0 ? 2 * qe : -2 * qe - 1;
    final h = u >> k;
    if (h < 12) {
      put(0, h);
      put(1, 1);
      put(u & ((1 << k) - 1), k);
    } else {
      put(0, 12);
      put(u, 9);
    }
  }
  while (bits.length % 8 != 0) {
    bits.add(0);
  }
  final out = [1, delta | (k << 4), px[0]];
  for (var i = 0; i < bits.length; i += 8) {
    var b = 0;
    for (var j = 0; j < 8; j++) {
      b = (b << 1) | bits[i + j];
    }
    out.add(b);
  }
  return Uint8List.fromList(out);
}

void main() {
  test('channels', () {
    expect(fpvChannelName(5), 'R6');
    expect(fpvMhz[fpvChannelIndex('R6')], 5843);
    expect(fpvChannelIndex('5865'), 8); // A1
  });

  test('packets: CRC, parse, resync after junk and a bad packet', () {
    final got = <List<int>>[];
    final p = LinkParser((t, a, b, pl) => got.add([t, a, b, pl.length]));
    final good = linkPacket(linkCmd, cmdChannelSet, 5);
    final bad = linkPacket(linkCmd, 1, 2)..[6] ^= 0xFF;
    p.feed([0x00, 0x13, ...bad, ...good.sublist(0, 3)]);
    p.feed(good.sublist(3));
    expect(got, [
      [linkCmd, cmdChannelSet, 5, 0],
    ]);
    expect(p.crcErrors, 1);
  });

  test('RAW and NL rows decode to the reconstruction', () {
    final px = Uint8List.fromList([for (var x = 0; x < fpvW; x++) (128 + 90 * ((x * 37) % 17 - 8) / 8).round().clamp(0, 255)]);
    final out = Uint8List(fpvW);
    expect(linkRowDecode(Uint8List.fromList([0, ...px]), out), true);
    expect(out, px);
    for (final (delta, k) in [(0, 3), (1, 2), (3, 1), (15, 0)]) {
      expect(linkRowDecode(nlEncode(px, delta, k), out), true, reason: 'delta $delta k $k');
      for (var x = 0; x < fpvW; x++) {
        expect((out[x] - px[x]).abs() <= delta, true, reason: 'delta $delta pixel $x');
      }
    }
  });

  test('INFO', () {
    final b = Uint8List(20);
    b[1] = 168;
    b[2] = 0x02;
    b[3] = 5843 & 0xFF;
    b[4] = 5843 >> 8;
    b[5] = 30;
    b[12] = 5;
    b[14] = 80;
    final i = FpvInfo(b);
    expect(i.video, true);
    expect(i.mhz, 5843);
    expect(i.channelName, 'R6');
    expect(i.level > 0.5, true);
  });
}
