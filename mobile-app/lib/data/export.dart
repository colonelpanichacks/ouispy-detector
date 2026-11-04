// History tools: the dashboard's exports (detections CSV / JSON, sightings CSV) written to
// the app's documents folder, and clearing detections (all, one node's, one device's).
//
// On the dashboard they are its own endpoints (/api/export/..., POST /api/detections/clear),
// so the files are the same ones its web UI downloads. On USB the phone is the dashboard:
// detections come from its store (MeshStore.dets, the same columns as store.py DET_COLS);
// the dashboard's sightings table (node join / leave events) has no phone twin, so the USB
// "sightings" file is one row per device heard (MeshStore.devices).
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';

import 'backend.dart';
import 'dashboard.dart';
import 'model.dart';
import 'store.dart';

/// store.py DET_COLS, with the dashboard's leading local `time` and trailing `mac_alias`.
const detCols = ['time', 'id', 'ts', 'node', 'mac', 'alias', 'rssi', 'type', 'match', 'descr', 'dom', 'source', 'is_new', 'mac_alias'];
const deviceCols = ['mac', 'alias', 'dom', 'type', 'name', 'descr', 'first', 'last', 'first_time', 'last_time', 'hits', 'best', 'nodes'];

String _two(int v) => v.toString().padLeft(2, '0');
String _time(num ts) {
  final t = DateTime.fromMillisecondsSinceEpoch((ts * 1000).round());
  return '${t.year}-${_two(t.month)}-${_two(t.day)} ${_two(t.hour)}:${_two(t.minute)}:${_two(t.second)}';
}

String _stamp() {
  final t = DateTime.now();
  return '${t.year}${_two(t.month)}${_two(t.day)}-${_two(t.hour)}${_two(t.minute)}${_two(t.second)}';
}

/// One CSV field (RFC 4180: quoted when it holds a comma, quote or line break).
String csvField(Object? v) {
  if (v == null) return '';
  final s = v is bool ? (v ? '1' : '0') : '$v';
  return s.contains(RegExp('[",\r\n]')) ? '"${s.replaceAll('"', '""')}"' : s;
}

String toCsv(List<String> cols, Iterable<Map<String, dynamic>> rows) {
  final b = StringBuffer()..write('${cols.join(',')}\r\n');
  for (final r in rows) {
    b.write('${cols.map((c) => csvField(r[c])).join(',')}\r\n');
  }
  return b.toString();
}

/// The detection filters the dashboard's exports take (`q`, `node`, `dom`, `since`).
class ExportFilter {
  const ExportFilter({this.q = '', this.node = '', this.dom = '', this.since});
  final String q, node, dom;
  final double? since;
  String get query => [
    if (q.isNotEmpty) 'q=${Uri.encodeQueryComponent(q)}',
    // the export endpoint takes one node id (a `kind:` pick is the device list's alone)
    if (node.isNotEmpty && !node.startsWith('kind:')) 'node=${Uri.encodeQueryComponent(node)}',
    if (dom.isNotEmpty) 'dom=$dom',
    if (since != null) 'since=${since!.round()}',
  ].join('&');
}

/// The phone's detections that pass [f], oldest first, as export rows. [nodeOk] decides a
/// node filter the store can't (a `kind:` one); by default it is the node id.
List<Map<String, dynamic>> localDetRows(ExportFilter f, {bool Function(String node)? nodeOk}) {
  final s = MeshStore.I;
  final ql = f.q.toLowerCase();
  final rows = <Map<String, dynamic>>[];
  for (final d in s.dets) {
    final mac = '${d['mac'] ?? ''}';
    final node = '${d['node'] ?? ''}';
    if (f.dom.isNotEmpty && d['dom'] != f.dom) continue;
    if (f.node.isNotEmpty && !(nodeOk?.call(node) ?? node == f.node)) continue;
    final ts = (d['ts'] as num?) ?? 0;
    if (f.since != null && ts < f.since!) continue;
    if (ql.isNotEmpty && !'$mac ${d['descr'] ?? ''} ${d['name'] ?? ''} ${d['alias'] ?? ''} ${s.aliases[mac] ?? ''}'.toLowerCase().contains(ql)) continue;
    rows.add({for (final c in detCols) c: d[c], 'time': _time(ts), 'is_new': d['is_new'] == true ? 1 : 0, 'mac_alias': s.aliases[mac]});
  }
  return rows;
}

/// Every device the phone heard, as export rows.
List<Map<String, dynamic>> localDeviceRows() {
  final s = MeshStore.I;
  return [
    for (final e in s.devices.entries)
      {
        'mac': e.key,
        'alias': s.aliases[e.key],
        'dom': e.value['dom'],
        'type': e.value['type'],
        'name': e.value['name'],
        'descr': e.value['descr'],
        'first': e.value['first'],
        'last': e.value['last'],
        'first_time': e.value['first'] is num ? _time(e.value['first'] as num) : '',
        'last_time': e.value['last'] is num ? _time(e.value['last'] as num) : '',
        'hits': e.value['hits'],
        'best': e.value['best'],
        'nodes': ((e.value['nodes'] as Map?) ?? const {}).keys.join(' '),
      },
  ]..sort((a, b) => ((a['first'] as num?) ?? 0).compareTo((b['first'] as num?) ?? 0));
}

Future<String> _write(String name, List<int> bytes) async {
  final dir = await getApplicationDocumentsDirectory();
  final f = File('${dir.path}/$name');
  await f.writeAsBytes(bytes, flush: true);
  return f.path;
}

Future<String> _download(Dashboard d, String path, String name) async {
  final r = await http.get(d.uri(path), headers: d.authHeaders).timeout(const Duration(seconds: 60));
  if (r.statusCode != 200) throw HttpException('HTTP ${r.statusCode} on $path');
  return _write(name, r.bodyBytes);
}

/// Detections as CSV or JSON ([fmt] `csv` | `json`); returns the file's path.
Future<String> exportDetections(Backend b, {String fmt = 'csv', ExportFilter filter = const ExportFilter(), bool Function(String node)? nodeOk}) async {
  final name = 'ouispy-detections-${_stamp()}.$fmt';
  if (b is Dashboard) {
    final q = filter.query;
    return _download(b, '/api/export/detections.$fmt${q.isEmpty ? '' : '?$q'}', name);
  }
  await MeshStore.I.load();
  final rows = localDetRows(filter, nodeOk: nodeOk);
  return _write(name, utf8.encode(fmt == 'json' ? const JsonEncoder.withIndent(' ').convert(rows) : toCsv(detCols, rows)));
}

/// The sightings CSV: the dashboard's node events; on USB, one row per device heard.
Future<String> exportSightings(Backend b) async {
  if (b is Dashboard) return _download(b, '/api/export/sightings.csv', 'ouispy-sightings-${_stamp()}.csv');
  await MeshStore.I.load();
  return _write('ouispy-devices-${_stamp()}.csv', utf8.encode(toCsv(deviceCols, localDeviceRows())));
}

/// Clears detections: all of them, one node's, or one device's (the dashboard's
/// POST /api/detections/clear). Returns how many went, or null when it failed.
Future<int?> clearDetections(Backend b, Model m, {String? node, String? mac}) async {
  mac = mac?.toUpperCase();
  if (b is Dashboard) {
    final r = await b.post('/api/detections/clear', {'node': ?node, 'mac': ?mac});
    if (r == null) return null;
    m.ingestCleared({'node': node ?? '', 'mac': mac ?? ''}); // the socket echo does it too
    return (r['n'] as num?)?.toInt() ?? 0;
  }
  final s = MeshStore.I;
  await s.load();
  final before = s.dets.length;
  s.dets.removeWhere((d) => (node == null || d['node'] == node) && (mac == null || d['mac'] == mac));
  s.dirty();
  m.ingestCleared({'node': node ?? '', 'mac': mac ?? ''});
  return before - s.dets.length;
}
