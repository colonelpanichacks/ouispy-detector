// Firmware images and putting them on nodes (home-dashboard/firmware.py, commands.py).
//
// An image is an ESP-IDF application: header magic 0xE9, chip id 9 (ESP32-S3: both the
// XIAO and the AtomS3 boards), the app description (magic 0xABCD5432) at byte 32, the
// OUI-SPY build stamp (the newest `__DATE__ __TIME__` in it, what nodes report as `fw`)
// and `OUISPY-KIND=<kind>` naming which firmware it is. Two boards: the XIAO ESP32-S3
// runs detector / skyspy / wardrive, the AtomS3 + GPS base runs gps / gpswd; one board's
// build never goes on the other.
//
// Two ways to update: the dashboard (it stages its own library build or an uploaded .bin on
// the home over its USB, then sends it over the mesh: fw_install / fw_stage / fw_send /
// fw_self jobs), or the phone on the home's USB (local_mesh.dart: fwbegin / fwdata / fwend,
// then fwsend <id> or fwself).
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';

import '../main.dart';
import 'dashboard.dart';
import 'local_mesh.dart';
import 'model.dart';

// ------------------------------------------------------------ SHA-256 (FIPS 180-4)

const _k = <int>[
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, //
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// The SHA-256 of [data] as 64 lowercase hex digits (what `fwbegin` wants).
String sha256Hex(List<int> data) {
  const m = 0xffffffff;
  int r(int x, int n) => ((x >> n) | (x << (32 - n))) & m;
  final h = Uint32List.fromList([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  final len = data.length;
  final padded = ((len + 9 + 63) ~/ 64) * 64;
  final b = Uint8List(padded)..setRange(0, len, data);
  b[len] = 0x80;
  final bits = len * 8;
  for (var i = 0; i < 8; i++) {
    b[padded - 1 - i] = (bits ~/ (1 << (8 * i))) & 0xff;
  }
  final w = Uint32List(64);
  for (var o = 0; o < padded; o += 64) {
    for (var i = 0; i < 16; i++) {
      final j = o + i * 4;
      w[i] = (b[j] << 24) | (b[j + 1] << 16) | (b[j + 2] << 8) | b[j + 3];
    }
    for (var i = 16; i < 64; i++) {
      final x = w[i - 15], y = w[i - 2];
      final s0 = r(x, 7) ^ r(x, 18) ^ (x >> 3);
      final s1 = r(y, 17) ^ r(y, 19) ^ (y >> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & m;
    }
    var a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (var i = 0; i < 64; i++) {
      final s1 = r(e, 6) ^ r(e, 11) ^ r(e, 25);
      final ch = (e & f) ^ ((~e & m) & g);
      final t1 = (hh + s1 + ch + _k[i] + w[i]) & m;
      final s0 = r(a, 2) ^ r(a, 13) ^ r(a, 22);
      final mj = (a & bb) ^ (a & c) ^ (bb & c);
      final t2 = (s0 + mj) & m;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) & m;
      d = c;
      c = bb;
      bb = a;
      a = (t1 + t2) & m;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  return h.map((x) => x.toRadixString(16).padLeft(8, '0')).join();
}

// ------------------------------------------------------------ images (firmware.py inspect)

const chipEsp32s3 = 9;
const appDescMagic = 0xABCD5432;
const fwMaxSize = 0x330000; // one app slot of default_8MB.csv
const fwChunk = 512; // bytes per fwdata line

/// The firmwares, as the dashboard's library names them.
const fwKinds = ['detector', 'skyspy', 'wardrive', 'gps', 'gpswd'];
const fwNames = {'detector': 'OUI-SPY detector', 'skyspy': 'Sky-Spy', 'wardrive': 'Wardriver', 'gps': 'GPS node', 'gpswd': 'GPS wardriver'};
const _atom = {'gps', 'gpswd'};

bool isAtomKind(String kind) => _atom.contains(kind);
String boardOf(String kind) => isAtomKind(kind) ? 'AtomS3 + GPS base' : 'XIAO ESP32-S3';

/// A node's firmware from the build stamp it reports ("Oct28 20:37:51S" = Sky-Spy).
String nodeKindOf(String fw) {
  final f = fw.trimRight();
  if (f.isEmpty) return 'detector';
  return switch (f[f.length - 1]) {
    'S' => 'skyspy',
    'W' => 'wardrive',
    'G' => 'gps',
    'Y' => 'gpswd',
    _ => 'detector',
  };
}

class FwError implements Exception {
  FwError(this.msg);
  final String msg;
  @override
  String toString() => msg;
}

class FwImage {
  FwImage._(this.bytes, this.name, this.chip, this.version, this.project, this.date, this.time, this.build, this.kind, this.sha256);
  final Uint8List bytes;
  final String name, version, project, date, time, build, kind, sha256;
  final int chip;
  int get size => bytes.length;

  /// Checks the image as the dashboard does; throws [FwError] when it is not a node app.
  factory FwImage.parse(List<int> data, {String name = 'firmware.bin', bool hash = true}) {
    final b = data is Uint8List ? data : Uint8List.fromList(data);
    if (b.length < 64 * 1024 || b.length > fwMaxSize) {
      throw FwError('size ${b.length} bytes is not a plausible app image (64 KB to ${fwMaxSize ~/ 1024} KB)');
    }
    if (b[0] != 0xE9) throw FwError('not an ESP32 application image (use firmware.bin, not a merged or bootloader file)');
    final bd = ByteData.sublistView(b);
    final chip = bd.getUint16(12, Endian.little);
    if (chip != chipEsp32s3) throw FwError('built for chip id $chip, not the ESP32-S3 these nodes use');
    if (bd.getUint32(32, Endian.little) != appDescMagic) throw FwError('no ESP-IDF app description in the image');
    String cs(int a, int z) {
      final s = b.sublist(a, z);
      final n = s.indexOf(0);
      return latin1.decode(n < 0 ? s : s.sublist(0, n)).trim();
    }

    final text = latin1.decode(b);
    return FwImage._(b, name, chip, cs(48, 80), cs(80, 112), cs(128, 144), cs(112, 128), buildStamp(text), kindIn(text), hash ? sha256Hex(b) : '');
  }

  String get label => '${fwNames[kind] ?? kind} ${build.isNotEmpty ? build : '$date $time'}';
}

const _months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/// The newest `Mmm dd yyyy hh:mm:ss` in the image: the firmware's FW_BUILD.
String buildStamp(String text) {
  DateTime? best;
  var out = '';
  for (final m in RegExp(r'([A-Z][a-z]{2}) ([ \d]\d) (\d{4}) (\d\d):(\d\d):(\d\d)').allMatches(text)) {
    final mo = _months.indexOf(m.group(1)!);
    if (mo < 0) continue;
    final t = DateTime(int.parse(m.group(3)!), mo + 1, int.parse(m.group(2)!.trim()), int.parse(m.group(4)!), int.parse(m.group(5)!), int.parse(m.group(6)!));
    if (best == null || t.isAfter(best)) {
      best = t;
      out = m.group(0)!;
    }
  }
  return out;
}

/// `OUISPY-KIND=<lowercase>` in the image; images from before the marker are detectors.
String kindIn(String text) {
  final i = text.indexOf('OUISPY-KIND=');
  if (i < 0) return 'detector';
  final m = RegExp(r'[a-z]{1,16}').matchAsPrefix(text, i + 12);
  return m?.group(0) ?? 'detector';
}

/// "Oct 27 2026 21:21:25" (an image) -> "Oct27 21:21:25" (what nodes report, plus a kind letter).
String fwCompact(String build) {
  final x = RegExp(r'^([A-Z][a-z]{2}) +(\d{1,2}) \d{4} (\d\d:\d\d:\d\d)$').firstMatch(build);
  return x == null ? '' : '${x.group(1)}${x.group(2)!.padLeft(2, '0')} ${x.group(3)}';
}

/// Does a node reporting [nodeFw] already run [build]?
bool fwIsCurrent(String nodeFw, String build) {
  if (nodeFw.isEmpty || build.isEmpty) return false;
  final f = nodeFw.trim().replaceFirst(RegExp(r'(?<=\d\d:\d\d:\d\d)[A-Z]$'), '');
  if (RegExp(r'^[A-Z][a-z]{2}\d{2} \d\d:\d\d:\d\d$').hasMatch(f)) return f == fwCompact(build);
  return f.length >= 15 && build.startsWith(f); // older firmware cut its stamp to 15 characters
}

/// Why [kind] must not go on this node (the wrong board), or null when it fits.
String? boardError(String kind, String nodeFw, String id) {
  if (nodeFw.trim().isEmpty) return null; // not reported: the dashboard lets it through too
  final have = nodeKindOf(nodeFw);
  if (isAtomKind(have) == isAtomKind(kind)) return null;
  return '$id is ${isAtomKind(have) ? 'an AtomS3 + GPS' : 'a XIAO'} board: the ${fwNames[kind] ?? kind} build is for ${isAtomKind(kind) ? 'an AtomS3 + GPS base' : 'a XIAO ESP32-S3'}';
}

/// An image checked against a node: the chip, then the board. Null when it may go on.
String? imageError(FwImage img, FleetNode n) {
  if (img.chip != chipEsp32s3) return 'built for chip id ${img.chip}, not the ESP32-S3 these nodes use';
  return boardError(img.kind, n.fw, n.id);
}

// ------------------------------------------------------------ the dashboard's REST, raw

/// One request to the dashboard that keeps its answer even on an error (Dashboard.post
/// throws the body of a 400 away): (status, decoded JSON). Status 0: no answer.
Future<(int, dynamic)> dashReq(
  Dashboard d,
  String method,
  String path, {
  Object? json,
  List<int>? bytes,
  Map<String, String> headers = const {},
  Duration timeout = const Duration(seconds: 15),
}) async {
  if (d.base.isEmpty) return (0, null);
  try {
    final rq = http.Request(method, d.uri(path))..headers.addAll({...d.authHeaders, ...headers});
    if (json != null) {
      rq.headers['Content-Type'] = 'application/json';
      rq.body = jsonEncode(json);
    } else if (bytes != null) {
      rq.headers['Content-Type'] = 'application/octet-stream';
      rq.bodyBytes = bytes;
    }
    final r = await http.Response.fromStream(await rq.send().timeout(timeout));
    dynamic body;
    try {
      body = jsonDecode(utf8.decode(r.bodyBytes));
    } catch (_) {}
    return (r.statusCode, body);
  } catch (_) {
    return (0, null);
  }
}

String dashError((int, dynamic) r, String what) {
  final (st, b) = r;
  if (b is Map && b['error'] != null) return '${b['error']}';
  return st == 0 ? 'no answer from the dashboard' : '$what: HTTP $st';
}

// ------------------------------------------------------------ where images come from

/// The dashboard's library: {kind: {size, sha256, build, kind}} of what this checkout built.
Future<Map<String, Map<String, dynamic>>> fwLibrary(Dashboard d) async {
  final j = await d.getJson('/api/firmware/library');
  if (j is! Map) return {};
  return {
    for (final e in j.entries)
      if (e.value is Map) '${e.key}': (e.value as Map).cast<String, dynamic>(),
  };
}

/// Folders the app can read a .bin from without a file picker.
Future<List<Directory>> fwFolders() async {
  final out = <Directory>[];
  try {
    final ext = await getExternalStorageDirectory();
    if (ext != null) out.add(Directory('${ext.path}/firmware'));
  } catch (_) {}
  try {
    out.add(Directory('${(await getApplicationDocumentsDirectory()).path}/firmware'));
  } catch (_) {}
  out.add(Directory('/sdcard/Download'));
  return out;
}

/// The .bin files in [fwFolders], newest first.
Future<List<File>> fwFiles() async {
  final out = <File>[];
  for (final d in await fwFolders()) {
    try {
      if (!await d.exists()) continue;
      await for (final f in d.list()) {
        if (f is File && f.path.toLowerCase().endsWith('.bin')) out.add(f);
      }
    } catch (_) {}
  }
  out.sort((a, b) => b.statSync().modified.compareTo(a.statSync().modified));
  return out;
}

/// An image from a path on the phone or an http(s) URL. Throws [FwError].
Future<FwImage> fwLoad(String where) async {
  where = where.trim();
  final name = where.split('/').last.split('?').first;
  final List<int> data;
  if (where.startsWith('http://') || where.startsWith('https://')) {
    try {
      final r = await http.get(Uri.parse(where)).timeout(const Duration(seconds: 60));
      if (r.statusCode != 200) throw FwError('HTTP ${r.statusCode} from $where');
      data = r.bodyBytes;
    } on FwError {
      rethrow;
    } catch (e) {
      throw FwError('could not download it ($e)');
    }
  } else {
    final f = File(where);
    if (!await f.exists()) throw FwError('no file at $where');
    data = await f.readAsBytes();
  }
  return compute(_parse, (data, name));
}

FwImage _parse((List<int>, String) a) => FwImage.parse(a.$1, name: a.$2);

// ------------------------------------------------------------ an update run

/// One node's part in a run.
class FwStep {
  FwStep(this.id);
  final String id;
  String state = 'waiting'; // waiting, staging, sending, installing, done, failed, skipped
  int pct = 0;
  String detail = '';
  int? kbps;
  bool get over => state == 'done' || state == 'failed' || state == 'skipped';
}

/// A firmware run: stage on the home once, each remote in turn, the home last (app.js fwGo).
class FwRun extends ChangeNotifier {
  FwRun._();
  static final I = FwRun._();

  bool running = false, _stop = false;
  String what = '', error = '';
  int stagePct = 0;
  final steps = <String, FwStep>{};
  Timer? _ticker;

  void _set(String id, String state, {int? pct, String? detail, int? kbps}) {
    final s = steps.putIfAbsent(id, () => FwStep(id));
    s.state = state;
    if (pct != null) s.pct = pct;
    if (detail != null) s.detail = detail;
    if (kbps != null) s.kbps = kbps;
    notifyListeners();
  }

  void _begin(String label, List<String> ids) {
    running = true;
    _stop = false;
    what = label;
    error = '';
    stagePct = 0;
    steps
      ..clear()
      ..addEntries(ids.map((i) => MapEntry(i, FwStep(i))));
    notifyListeners();
  }

  void _end([String err = '']) {
    running = false;
    error = err;
    _ticker?.cancel();
    for (final s in steps.values) {
      if (!s.over) s.state = 'skipped';
    }
    notifyListeners();
    final bad = steps.values.where((s) => s.state == 'failed').length;
    App.I.m.toast(err.isNotEmpty ? 'firmware: $err' : (bad > 0 ? 'firmware: $bad node(s) need attention' : 'firmware: all selected nodes updated'));
  }

  /// Stops the run after the node in flight; on USB also aborts the transfer itself.
  Future<void> abort() async {
    _stop = true;
    final l = App.I.link;
    if (l is LocalMesh) {
      await l.fwAbort();
    } else if (l is Dashboard) {
      // the dashboard has no abort route: its fwabort goes to the home's console
      await l.sendLine('fwabort');
    }
    notifyListeners();
  }

  /// Updates [ids] (the home among them is done last) with a library build of [kind] or an
  /// uploaded [img], through whichever backend the app is on.
  Future<void> run(List<String> ids, {String? kind, FwImage? img}) async {
    if (running) return;
    final m = App.I.m;
    final home = m.nodes.values.where((n) => n.isHome).firstOrNull;
    if (home == null) {
      m.toast('no home node: firmware goes through the home');
      return;
    }
    final k = kind ?? img?.kind ?? 'detector';
    // never one board's firmware on the other
    for (final id in ids) {
      final n = m.nodes[id];
      if (n == null) continue;
      final e = img != null ? imageError(img, n) : boardError(k, n.fw, n.id);
      if (e != null) {
        m.toast(e);
        return;
      }
    }
    final remotes = [
      for (final id in ids)
        if (id != home.id) id,
    ];
    final self = ids.contains(home.id);
    _begin(img != null ? img.label : '${fwNames[k] ?? k} (library)', [...remotes, if (self) home.id]);
    final l = App.I.link;
    try {
      if (l is LocalMesh) {
        if (img == null) return _end('over USB the image comes from a .bin on the phone, not the library');
        await _usb(l, home.id, remotes, self, img);
      } else if (l is Dashboard) {
        await _dash(l, home.id, remotes, self, kind: kind, img: img);
      } else {
        _end('no backend');
      }
    } catch (e) {
      _end('$e');
    }
  }

  // ---- the phone on the home's USB

  Future<void> _usb(LocalMesh mesh, String home, List<String> remotes, bool self, FwImage img) async {
    if (mesh.homeId.isEmpty || mesh.homeId != home) return _end('the node on USB is not the home');
    _set(home, steps.containsKey(home) ? 'staging' : 'waiting');
    final st = await mesh.nodeRun(home, 'fw_stage', {
      'image': img.bytes,
      'sha256': img.sha256,
      'progress': (int done, int size) {
        final p = size == 0 ? 0 : done * 100 ~/ size;
        if (p != stagePct) {
          stagePct = p;
          notifyListeners();
        }
      },
    });
    if (st.r == null) return _end('staging failed: ${st.err ?? 'no answer'}');
    stagePct = 100;
    for (final id in remotes) {
      if (_stop) return _end('stopped');
      _set(id, 'sending', pct: 0, detail: 'offering');
      final sub = mesh.otaLines.where((o) => o['node'] == id);
      final got = Completer<Map<String, dynamic>?>();
      var last = DateTime.now();
      final s = sub.listen((o) {
        last = DateTime.now();
        final state = '${o['state']}';
        _set(
          id,
          state == 'done' ? 'done' : (state == 'failed' ? 'failed' : 'sending'),
          pct: (o['pct'] as num?)?.toInt(),
          kbps: (o['kbps'] as num?)?.toInt(),
          detail: '${o['err'] ?? ''}'.isNotEmpty ? '${o['err']}' : state,
        );
        if ((state == 'done' || state == 'failed') && !got.isCompleted) got.complete(o);
      });
      final sent = await mesh.nodeRun(id, 'fw_send');
      if (sent.r == null) {
        await s.cancel();
        _set(id, 'failed', detail: sent.err ?? 'no answer');
        continue;
      }
      // the home reports every 2 s while it sends; 2 min of silence is a dead transfer
      _ticker?.cancel();
      _ticker = Timer.periodic(const Duration(seconds: 5), (_) {
        if (_stop || DateTime.now().difference(last).inSeconds > 120) {
          if (!got.isCompleted) got.complete(null);
        }
      });
      final o = await got.future;
      _ticker?.cancel();
      await s.cancel();
      if (o == null) _set(id, 'failed', detail: _stop ? 'stopped' : 'no progress from the home for 2 minutes');
      if (o?['state'] == 'done') _set(id, 'done', pct: 100, detail: 'updated, rebooting into it');
    }
    if (self && !_stop) {
      _set(home, 'installing', pct: 100);
      final r = await mesh.nodeRun(home, 'fw_self');
      _set(home, r.r != null ? 'done' : 'failed', detail: r.r != null ? 'installing, rebooting' : (r.err ?? 'no answer'));
    }
    _end(_stop ? 'stopped' : '');
  }

  // ---- the dashboard

  Future<Map<String, dynamic>?> _cmd(Dashboard d, String id, String op, Map<String, dynamic> args) async {
    final r = await dashReq(d, 'POST', '/api/nodes/$id/cmd', json: {'op': op, 'args': args});
    final b = r.$2;
    if (r.$1 != 200 || b is! Map || b['ok'] != true || b['job'] is! Map) {
      _set(id, 'failed', detail: dashError(r, op));
      return null;
    }
    return (b['job'] as Map).cast<String, dynamic>();
  }

  /// Polls /api/jobs until the job settles; [onJob] sees every update.
  Future<Map<String, dynamic>?> _wait(Dashboard d, Map<String, dynamic> job, String node, Duration limit, void Function(Map<String, dynamic>) onJob) async {
    final until = DateTime.now().add(limit);
    while (DateTime.now().isBefore(until)) {
      if (_stop) return null;
      final j = await d.getJson('/api/jobs?node=$node');
      if (j is List) {
        for (final x in j) {
          if (x is Map && x['id'] == job['id']) {
            final jm = x.cast<String, dynamic>();
            onJob(jm);
            if (const ['done', 'failed', 'partial'].contains(jm['state'])) return jm;
          }
        }
      }
      await Future.delayed(const Duration(seconds: 1));
    }
    return null;
  }

  String _jobErr(Map<String, dynamic>? j) {
    if (j == null) return _stop ? 'stopped' : 'no result in time';
    final e = '${j['error'] ?? ''}';
    if (e.isNotEmpty) return e;
    final rs = j['replies'];
    return rs is Map ? rs.values.map((x) => x is Map ? '${x['text']}' : '$x').join(', ') : '${j['state']}';
  }

  Future<void> _dash(Dashboard d, String home, List<String> remotes, bool self, {String? kind, FwImage? img}) async {
    if (img != null) {
      final up = await dashReq(d, 'POST', '/api/firmware', bytes: img.bytes, headers: {'X-Filename': img.name}, timeout: const Duration(seconds: 120));
      if (up.$1 != 200 || up.$2 is! Map || (up.$2 as Map)['ok'] != true) return _end('upload refused: ${dashError(up, 'upload')}');
    }
    // one node, a library build: fw_install (the dashboard stages, then sends or self-installs)
    final one = [...remotes, if (self) home];
    if (kind != null && img == null && one.length == 1) {
      final id = one.single;
      _set(id, 'staging', pct: 0, detail: 'staging on home');
      final job = await _cmd(d, id, 'fw_install', {'kind': kind});
      if (job == null) return _end();
      final j = await _wait(d, job, id, const Duration(minutes: 45), (x) => _dashProgress(id, x));
      _set(id, j?['state'] == 'done' ? 'done' : 'failed', pct: j?['state'] == 'done' ? 100 : null, detail: j?['state'] == 'done' ? 'updated, rebooting' : _jobErr(j));
      return _end();
    }
    final a = <String, dynamic>{'kind': ?kind, 'convert': true};
    final sj = await _cmd(d, home, 'fw_stage', {'kind': ?kind});
    if (sj == null) return _end('staging refused');
    final st = await _wait(d, sj, home, const Duration(minutes: 10), (x) {
      stagePct = (x['progress'] as num?)?.toInt() ?? stagePct;
      notifyListeners();
    });
    if (st?['state'] != 'done') return _end('staging failed: ${_jobErr(st)}');
    stagePct = 100;
    for (final id in remotes) {
      if (_stop) return _end('stopped');
      _set(id, 'sending', pct: 0, detail: 'offering');
      final job = await _cmd(d, id, 'fw_send', a);
      if (job == null) continue;
      final j = await _wait(d, job, id, const Duration(minutes: 40), (x) => _dashProgress(id, x));
      _set(id, j?['state'] == 'done' ? 'done' : 'failed', pct: j?['state'] == 'done' ? 100 : null, detail: j?['state'] == 'done' ? 'updated, rebooting' : _jobErr(j));
    }
    if (self && !_stop) {
      _set(home, 'installing', pct: 100);
      final job = await _cmd(d, home, 'fw_self', a);
      if (job != null) {
        final j = await _wait(d, job, home, const Duration(minutes: 2), (_) {});
        _set(home, j?['state'] == 'done' ? 'done' : 'failed', detail: j?['state'] == 'done' ? 'installing, rebooting' : _jobErr(j));
      }
    }
    _end(_stop ? 'stopped' : '');
  }

  void _dashProgress(String id, Map<String, dynamic> j) {
    final ota = j['ota'] is Map ? (j['ota'] as Map) : const {};
    final stage = j['op'] == 'fw_stage';
    _set(
      id,
      stage || j['state'] == 'queued' ? 'staging' : 'sending',
      pct: (j['progress'] as num?)?.toInt(),
      kbps: (ota['kbps'] as num?)?.toInt(),
      detail: stage || j['state'] == 'queued' ? 'staging on home' : '${ota['state'] ?? j['state'] ?? ''}',
    );
  }
}
