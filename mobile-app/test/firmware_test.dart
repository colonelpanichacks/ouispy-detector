// Firmware images and SHA-256 (firmware.dart), against firmware.py's checks and the
// FIPS 180-4 test vectors.
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/data/firmware.dart';
import 'package:ouispy_mesh/data/model.dart';

Uint8List fakeImage({int chip = 9, int magic = appDescMagic, int size = 80 * 1024, String kind = 'skyspy'}) {
  final b = Uint8List(size);
  final d = ByteData.sublistView(b);
  b[0] = 0xE9;
  d.setUint16(12, chip, Endian.little);
  d.setUint32(32, magic, Endian.little);
  void put(int at, String s) => b.setRange(at, at + s.length, latin1.encode(s));
  put(48, 'v1.0');
  put(80, 'ouispy');
  put(112, '10:00:00');
  put(128, 'Jan  2 2025');
  put(4000, 'Jan  2 2025 10:00:00'); // the core's build date: older
  put(9000, 'Oct 27 2026 21:21:25'); // FW_BUILD: the newest
  if (kind.isNotEmpty) put(12000, 'OUISPY-KIND=$kind\u0000');
  return b;
}

void main() {
  test('SHA-256 test vectors', () {
    expect(sha256Hex([]), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
    expect(sha256Hex(utf8.encode('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    expect(sha256Hex(utf8.encode('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')), '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
    expect(sha256Hex(List<int>.filled(1000000, 0x61)), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
    // 55 / 56 / 64 bytes: the padding edges
    expect(sha256Hex(List<int>.filled(55, 0x61)), '9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318');
    expect(sha256Hex(List<int>.filled(56, 0x61)), 'b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a');
    expect(sha256Hex(List<int>.filled(64, 0x61)), 'ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb');
  });

  test('image header: chip, app description, build stamp, kind', () {
    final i = FwImage.parse(fakeImage(), name: 'sky.bin');
    expect(i.chip, 9);
    expect(i.version, 'v1.0');
    expect(i.project, 'ouispy');
    expect(i.build, 'Oct 27 2026 21:21:25');
    expect(i.kind, 'skyspy');
    expect(i.sha256, sha256Hex(fakeImage()));
    expect(FwImage.parse(fakeImage(kind: '')).kind, 'detector');
    expect(() => FwImage.parse(fakeImage(chip: 0)), throwsA(isA<FwError>().having((e) => e.msg, 'msg', contains('chip id 0'))));
    expect(() => FwImage.parse(fakeImage(magic: 0)), throwsA(isA<FwError>()));
    expect(() => FwImage.parse(Uint8List(1000)..[0] = 0xE9), throwsA(isA<FwError>().having((e) => e.msg, 'msg', contains('plausible'))));
    final merged = fakeImage()..[0] = 0;
    expect(() => FwImage.parse(merged), throwsA(isA<FwError>().having((e) => e.msg, 'msg', contains('merged'))));
  });

  test('kinds, boards and the current build', () {
    expect(nodeKindOf('Oct28 20:37:51S'), 'skyspy');
    expect(nodeKindOf('Oct28 20:37:51W'), 'wardrive');
    expect(nodeKindOf('Oct28 20:37:51Y'), 'gpswd');
    expect(nodeKindOf('Oct28 20:37:51'), 'detector');
    expect(nodeKindOf(''), 'detector');
    expect(boardError('gps', 'Oct28 20:37:51', 'AD5484'), contains('XIAO'));
    expect(boardError('detector', 'Oct28 20:37:51G', 'AD5484'), contains('AtomS3'));
    expect(boardError('skyspy', 'Oct28 20:37:51', 'AD5484'), isNull); // same board: a conversion
    expect(boardError('gps', '', 'AD5484'), isNull);
    expect(fwCompact('Oct  7 2026 21:21:25'), 'Oct07 21:21:25');
    expect(fwIsCurrent('Oct27 21:21:25', 'Oct 27 2026 21:21:25'), true);
    expect(fwIsCurrent('Oct27 21:21:25S', 'Oct 27 2026 21:21:25'), true);
    expect(fwIsCurrent('Oct27 21:21:26', 'Oct 27 2026 21:21:25'), false);
    expect(fwIsCurrent('Oct 27 2026 21:', 'Oct 27 2026 21:21:25'), true); // old 15-char stamps
    final atom = FleetNode({
      'id': '4B4D04',
      'cfg': {'fw': 'Oct28 20:37:51G'},
    });
    expect(imageError(FwImage.parse(fakeImage(kind: 'detector'), hash: false), atom), contains('AtomS3'));
    expect(imageError(FwImage.parse(fakeImage(kind: 'gpswd'), hash: false), atom), isNull);
  });
}
