// FPV: the OUI SPY Recon's 5.8 GHz analog receiver (the C5) as the phone sees it. This is
// a straight port of the receiver's own link protocol (c5vrx/main/link_proto.h, protocol 2):
// the packet parser, the 224x168 row codec (RAW and the near-lossless DPCM + Rice "NL") and
// the INFO metrics. So once the display board passes its link packets through to USB
// (docs/FIRMWARE-NEEDS.md), the phone shows the live picture, the channel map and the lock
// events. FPV detections also arrive as `FPV {json}` console lines, so they reach the feed,
// the alerts and the Drones tab with Remote ID.
//
//   packet: A5 5A | type | a | b | len | payload | crc16 lo hi  (CRC-16/CCITT-FALSE over type..payload)
//   0x11 ROW  a = frame, b = row 0..167, payload[0] codec (0 RAW 224 B, 1 NL)
//   0x10 INFO 20 B, closes a frame (or alone while scanning)
//   0x20 CMD  a = command, b = argument (phone -> receiver, through the display)
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import 'backend.dart';

const fpvW = 224, fpvH = 168;

// ------------------------------------------------------------------ channels (c5vrx/main/rf.c)

const fpvBands = ['R', 'A', 'B', 'E', 'F', 'L'];
const fpvBandNames = ['RaceBand', 'Boscam A', 'Boscam B', 'Boscam E', 'FatShark', 'LowBand'];
const fpvMhz = [
  5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, // R
  5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, // A
  5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, // B
  5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, // E
  5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, // F
  5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621, // L
];

/// "R6" for a channel index (band * 8 + channel - 1); "--" when unknown.
String fpvChannelName(int i) => i < 0 || i >= 48 ? '--' : '${fpvBands[i ~/ 8]}${i % 8 + 1}';

/// The index of "R6" (or of a frequency), or -1.
int fpvChannelIndex(String s) {
  final m = RegExp(r'^([RABEFL])([1-8])$').firstMatch(s.toUpperCase().trim());
  if (m != null) return fpvBands.indexOf(m.group(1)!) * 8 + int.parse(m.group(2)!) - 1;
  final f = int.tryParse(s.trim());
  return f == null ? -1 : fpvMhz.indexOf(f);
}

// ------------------------------------------------------------------ packets

const linkInfo = 0x10, linkRow = 0x11, linkCmd = 0x20;
const cmdScanStart = 1, cmdScanStop = 2, cmdChannelStep = 3, cmdMode = 4, cmdSmoothing = 5, cmdChannelSet = 6, cmdSurvey = 7, cmdHop = 8;
const modeAuto = 0, modeFine = 1, modeRaw = 2;

final Uint16List _crcTable = () {
  final t = Uint16List(256);
  for (var i = 0; i < 256; i++) {
    var c = i << 8;
    for (var b = 0; b < 8; b++) {
      c = (c & 0x8000) != 0 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    t[i] = c;
  }
  return t;
}();

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), as link_crc16.
int linkCrc16(List<int> d, [int start = 0, int? end]) {
  var c = 0xFFFF;
  for (var i = start; i < (end ?? d.length); i++) {
    c = ((c << 8) & 0xFFFF) ^ _crcTable[((c >> 8) ^ d[i]) & 0xFF];
  }
  return c;
}

/// A packet, as link_packet builds it.
Uint8List linkPacket(int type, int a, int b, [List<int> payload = const []]) {
  final out = Uint8List(8 + payload.length);
  out.setAll(0, [0xA5, 0x5A, type, a & 0xFF, b & 0xFF, payload.length]);
  out.setAll(6, payload);
  final c = linkCrc16(out, 2, 6 + payload.length);
  out[6 + payload.length] = c & 0xFF;
  out[7 + payload.length] = c >> 8;
  return out;
}

/// The link parser (link_parser_feed): hunts for the magic, waits for the whole packet,
/// checks the CRC, and after a bad one looks again from the byte after it.
class LinkParser {
  LinkParser(this.onPacket);
  final void Function(int type, int a, int b, Uint8List payload) onPacket;
  final _q = <int>[];
  int packets = 0, crcErrors = 0, skipped = 0;

  void feed(List<int> data) {
    _q.addAll(data);
    var s = 0;
    for (;;) {
      while (s < _q.length && !(_q[s] == 0xA5 && (s + 1 == _q.length || _q[s + 1] == 0x5A))) {
        s++;
        skipped++;
      }
      if (s + 6 > _q.length) break;
      final len = _q[s + 5];
      if (s + 8 + len > _q.length) break;
      final c = linkCrc16(_q, s + 2, s + 6 + len);
      if ((c & 0xFF) == _q[s + 6 + len] && (c >> 8) == _q[s + 7 + len]) {
        packets++;
        onPacket(_q[s + 2], _q[s + 3], _q[s + 4], Uint8List.fromList(_q.sublist(s + 6, s + 6 + len)));
        s += 8 + len;
      } else {
        crcErrors++;
        s += 1;
      }
    }
    if (s > 0) _q.removeRange(0, s);
    if (_q.length > 4096) _q.removeRange(0, _q.length - 600); // junk with no magic: keep a tail
  }
}

// ------------------------------------------------------------------ rows

/// link_row_decode: one 224-pixel row (RAW or NL) into [px]. False for a malformed row.
bool linkRowDecode(Uint8List inp, Uint8List px) {
  final len = inp.length;
  if (len < 1) return false;
  if (inp[0] == 0) {
    if (len < 1 + fpvW) return false;
    px.setRange(0, fpvW, inp, 1);
    return true;
  }
  if (inp[0] != 1 || len < 3) return false;
  final delta = inp[1] & 15, k = (inp[1] >> 4) & 7, q = 2 * delta + 1;
  var p = 3;
  var acc = 0; // next bits, left aligned in 32
  var n = 0;
  var r = inp[2];
  px[0] = r;
  for (var x = 1; x < fpvW; x++) {
    while (n <= 24 && p < len) {
      acc |= inp[p++] << (24 - n);
      n += 8;
    }
    acc &= 0xFFFFFFFF;
    final z = acc == 0 ? 32 : 31 - acc.bitLength + 1;
    int v;
    if (z < 12) {
      final need = z + 1 + k;
      if (need > n) return false;
      v = (z << k) | (k > 0 ? (((acc << (z + 1)) & 0xFFFFFFFF) >> (32 - k)) : 0);
      acc = need < 32 ? (acc << need) & 0xFFFFFFFF : 0;
      n -= need;
    } else {
      const need = 12 + 9;
      if (need > n) return false;
      v = ((acc << 12) & 0xFFFFFFFF) >> (32 - 9);
      acc = (acc << need) & 0xFFFFFFFF;
      n -= need;
    }
    final qe = (v & 1) != 0 ? -((v + 1) >> 1) : (v >> 1);
    r += qe * q;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    px[x] = r;
  }
  return true;
}

// ------------------------------------------------------------------ INFO

class FpvInfo {
  FpvInfo(Uint8List b)
    : version = b.isNotEmpty ? b[0] : 0,
      rows = b.length > 1 ? b[1] : 0,
      flags = b.length > 2 ? b[2] : 0,
      mhz = b.length > 4 ? b[3] | (b[4] << 8) : 0,
      gain = b.length > 5 ? b[5] : 0,
      error = b.length > 6 ? b[6] : 0,
      channel = b.length > 12 ? b[12] : 0xFF,
      locks = b.length > 13 ? b[13] : 0,
      power = b.length > 14 ? b[14] : 0,
      clip = b.length > 15 ? b[15] : 0,
      jitterNs = b.length > 17 ? b[17] * 4 : 0,
      late = b.length > 18 ? b[18] : 0,
      noSync = b.length > 19 ? b[19] : 0;
  final int version, rows, flags, mhz, gain, error, channel, locks, power, clip, jitterNs, late, noSync;

  bool get pal => flags & 0x01 != 0;
  bool get video => flags & 0x02 != 0 && rows > 0 && !const [1, 2, 5].contains(error);
  int get mode => (flags >> 2) & 3;
  bool get scanning => flags & 0x10 != 0;
  bool get smoothing => flags & 0x20 != 0;
  bool get survey => flags & 0x40 != 0;
  bool get hop => flags & 0x80 != 0;
  String get channelName => fpvChannelName(channel == 0xFF ? -1 : channel);
  String get errorText => const ['', 'no hsync', 'no vsync', 'lost sync', 'timeout', 'error', 'lost vsync', 'off carrier'][error.clamp(0, 7)];

  /// The display's proximity level (shell.cpp): (62 - gain) + 10 log10(power / 2), over 45 dB.
  double get level {
    final p = power < 2 ? 2 : power;
    return ((62 - gain) + 10 * math.log(p / 2) / math.ln10).clamp(0, 45) / 45;
  }
}

/// An FPV transmitter heard: on a lock, and once a second while locked.
class FpvDet {
  FpvDet({required this.ts, required this.mhz, required this.channel, required this.level, this.node = '', this.video = true, this.snapshot});
  final double ts;
  final int mhz, channel;
  final double level;
  final String node;
  final bool video;
  String? snapshot; // a picture saved at the time, when snapshots are on
  String get name => fpvChannelName(channel);
}

// ------------------------------------------------------------------ the receiver

class Fpv extends ChangeNotifier {
  Fpv._() {
    parser = LinkParser(_packet);
  }
  static final I = Fpv._();

  late final LinkParser parser;
  FpvInfo? info;
  ui.Image? frame;
  double fps = 0;
  final dets = <FpvDet>[];
  final survey = <int, FpvInfo>{}; // channel -> the last probe of it during a sweep
  bool snapOnLock = true, record = false;
  int _lastLocks = -1;
  final _rows = Uint8List(fpvW * fpvH);
  final _row = Uint8List(fpvW);
  int _frameNo = -1, _frames = 0;
  DateTime _fpsT = DateTime.now();
  StreamSubscription<ConsoleLine>? _sub;
  Backend? _b;
  Directory? _rec;
  int _recN = 0;

  /// FPV lines from the console (`FPV {json}`, `[SCAN]`/`[LINK] tuned` text) and, once the
  /// display passes them through, link packets (fed to [feedBytes] by the transport).
  void attach(Backend b) {
    _sub?.cancel();
    _b = b;
    _sub = b.console.stream.listen((c) => _line(c.line));
  }

  void feedBytes(List<int> d) => parser.feed(d);

  void _line(String l) {
    if (!l.startsWith('FPV ')) return;
    try {
      final j = jsonDecode(l.substring(4)) as Map;
      final ch = j['ch'] is String ? fpvChannelIndex('${j['ch']}') : ((j['ch'] as num?)?.toInt() ?? fpvMhz.indexOf((j['mhz'] as num?)?.toInt() ?? 0));
      _det(
        FpvDet(
          ts: DateTime.now().millisecondsSinceEpoch / 1000,
          mhz: (j['mhz'] as num?)?.toInt() ?? (ch >= 0 ? fpvMhz[ch] : 0),
          channel: ch,
          level: ((j['level'] as num?)?.toDouble() ?? 0.5).clamp(0, 1),
          node: '${j['node'] ?? ''}',
          video: j['video'] != 0,
        ),
      );
    } catch (_) {}
  }

  void _det(FpvDet d) {
    // one entry per channel per 10 s: a lock and its once-a-second updates fold together
    final prev = dets.where((x) => x.channel == d.channel && d.ts - x.ts < 10).firstOrNull;
    if (prev != null) return;
    dets.insert(0, d);
    if (dets.length > 300) dets.removeLast();
    if (snapOnLock && frame != null) snapshot().then((p) => d.snapshot = p);
    notifyListeners();
  }

  void _packet(int type, int a, int b, Uint8List p) {
    if (type == linkRow && b < fpvH) {
      if (a != _frameNo) _frameNo = a;
      if (linkRowDecode(p, _row)) _rows.setRange(b * fpvW, (b + 1) * fpvW, _row);
    } else if (type == linkInfo) {
      final i = FpvInfo(p);
      info = i;
      if (i.survey && i.channel < 48) survey[i.channel] = i;
      // a scan that locks onto video bumps the counter by one
      if (_lastLocks >= 0 && i.locks == ((_lastLocks + 1) & 0xFF) && i.video) {
        _det(FpvDet(ts: DateTime.now().millisecondsSinceEpoch / 1000, mhz: i.mhz, channel: i.channel == 0xFF ? -1 : i.channel, level: i.level));
      }
      _lastLocks = i.locks;
      if (i.rows > 0) _publish();
      notifyListeners();
    }
  }

  void _publish() {
    _frames++;
    final now = DateTime.now();
    final dt = now.difference(_fpsT).inMilliseconds;
    if (dt >= 1000) {
      fps = _frames * 1000 / dt;
      _frames = 0;
      _fpsT = now;
    }
    // grey to RGBA with the display's auto-levels (stretch the 2..98 % range)
    final hist = List<int>.filled(256, 0);
    for (final v in _rows) {
      hist[v]++;
    }
    var lo = 0, hi = 255, acc = 0;
    final cut = _rows.length * 2 ~/ 100;
    for (var i = 0; i < 256; i++) {
      acc += hist[i];
      if (acc > cut) {
        lo = i;
        break;
      }
    }
    acc = 0;
    for (var i = 255; i >= 0; i--) {
      acc += hist[i];
      if (acc > cut) {
        hi = i;
        break;
      }
    }
    final span = (hi - lo) < 16 ? 16 : hi - lo;
    final rgba = Uint8List(fpvW * fpvH * 4);
    for (var i = 0; i < _rows.length; i++) {
      final v = ((_rows[i] - lo) * 255 ~/ span).clamp(0, 255);
      rgba[i * 4] = v;
      rgba[i * 4 + 1] = v;
      rgba[i * 4 + 2] = v;
      rgba[i * 4 + 3] = 255;
    }
    ui.decodeImageFromPixels(rgba, fpvW, fpvH, ui.PixelFormat.rgba8888, (img) {
      frame?.dispose();
      frame = img;
      if (record) _saveFrame(img);
      notifyListeners();
    });
  }

  Future<Directory> _dir(String sub) async {
    final d = Directory('${(await getApplicationDocumentsDirectory()).path}/fpv/$sub');
    await d.create(recursive: true);
    return d;
  }

  /// The current picture as a PNG; returns its path.
  Future<String?> snapshot() async {
    final f = frame;
    if (f == null) return null;
    final png = await f.toByteData(format: ui.ImageByteFormat.png);
    if (png == null) return null;
    final ts = DateTime.now().toIso8601String().replaceAll(':', '-').split('.').first;
    final file = File('${(await _dir('snapshots')).path}/fpv-${info?.channelName ?? 'ch'}-$ts.png');
    await file.writeAsBytes(png.buffer.asUint8List());
    return file.path;
  }

  /// Recording keeps every frame as a numbered PNG in a folder of its own.
  Future<void> setRecord(bool on) async {
    record = on;
    if (on) {
      final ts = DateTime.now().toIso8601String().replaceAll(':', '-').split('.').first;
      _rec = await _dir('recordings/$ts');
      _recN = 0;
    } else {
      _rec = null;
    }
    notifyListeners();
  }

  Future<void> _saveFrame(ui.Image img) async {
    final d = _rec;
    if (d == null) return;
    final png = await img.toByteData(format: ui.ImageByteFormat.png);
    if (png == null) return;
    await File('${d.path}/${(_recN++).toString().padLeft(6, '0')}.png').writeAsBytes(png.buffer.asUint8List());
  }

  void setSnapOnLock(bool on) {
    snapOnLock = on;
    notifyListeners();
  }

  // ---- control: the text verbs the display takes (docs/FIRMWARE-NEEDS.md)
  Future<bool> _say(String l) async => await _b?.sendLine(l) ?? false;
  Future<bool> tune(int ch) => _say('fpv chan ${fpvChannelName(ch)}');
  Future<bool> step(int n) => _say('fpv step $n');
  Future<bool> scan() => _say('fpv scan');
  Future<bool> sweep() => _say('fpv survey');
  Future<bool> hop() => _say('fpv hop');
  Future<bool> stop() => _say('fpv stop');
  Future<bool> mode(int m) => _say('fpv mode ${const ['auto', 'fine', 'raw'][m.clamp(0, 2)]}');
  Future<bool> smooth(bool on) => _say('fpv smooth ${on ? 1 : 0}');
  Future<bool> stream(bool on) => _say('fpv stream ${on ? 1 : 0}');
}
