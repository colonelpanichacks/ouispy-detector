// What the app keeps when it is the dashboard: one JSON file in the app's private
// storage, written a few seconds after a change. The same records home-dashboard keeps in
// SQLite (store.py), trimmed to what a phone needs: names, notes, groups, drone tags,
// node names / colours / places, marks, every device heard (with its broadcast intel),
// the latest detections, hourly counts and drone reports for flights.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:path_provider/path_provider.dart';

const _keepDets = 3000, _keepReports = 4000, _keepDevices = 20000;

class MeshStore {
  MeshStore._();
  static final I = MeshStore._();

  Map<String, String> aliases = {}, notes = {};
  List<Map<String, dynamic>> groups = []; // {id, name, color, members: [mac]}
  Map<String, Map<String, dynamic>> droneMeta = {}; // mac -> {alias, tag, notes}
  Map<String, Map<String, dynamic>> droneLast = {}; // mac -> last public state (no tracks)
  Map<String, List<Map<String, dynamic>>> droneReports = {}; // mac -> [{ts, node, lat, lon, alt, hagl, spd, plat, plon}]
  Map<String, Map<String, dynamic>> nodes = {}; // id -> {alias, color, lat, lon}
  Map<String, List<dynamic>> filters = {}; // id -> the node's filter list (RFL)
  List<Map<String, dynamic>> marks = [];
  Map<String, Map<String, dynamic>> devices = {}; // mac -> {first, last, hits, best, dom, type, descr, name, nodes: {id: n}, intel: {}}
  List<Map<String, dynamic>> dets = [];
  Map<String, int> hours = {}; // "hour since epoch" -> detections
  int total = 0;
  // who turns up with whom: pairs heard within the same minute (store.py companions)
  Map<String, Map<String, int>> together = {};
  final _recent = <String, double>{};

  bool _loaded = false;
  Timer? _save;
  File? _file;

  Future<File> _f() async => _file ??= File('${(await getApplicationSupportDirectory()).path}/mesh.json');

  Future<void> load() async {
    if (_loaded) return;
    _loaded = true;
    try {
      final f = await _f();
      if (!await f.exists()) return;
      final j = jsonDecode(await f.readAsString()) as Map<String, dynamic>;
      Map<String, Map<String, dynamic>> mm(Object? o) => {for (final e in ((o as Map?) ?? const {}).entries) '${e.key}': (e.value as Map).cast<String, dynamic>()};
      aliases = ((j['aliases'] as Map?) ?? const {}).map((k, v) => MapEntry('$k', '$v'));
      notes = ((j['notes'] as Map?) ?? const {}).map((k, v) => MapEntry('$k', '$v'));
      groups = [for (final g in (j['groups'] as List? ?? const [])) (g as Map).cast<String, dynamic>()];
      droneMeta = mm(j['droneMeta']);
      droneLast = mm(j['droneLast']);
      droneReports = {
        for (final e in ((j['droneReports'] as Map?) ?? const {}).entries) '${e.key}': [for (final r in e.value as List) (r as Map).cast<String, dynamic>()],
      };
      nodes = mm(j['nodes']);
      filters = {for (final e in ((j['filters'] as Map?) ?? const {}).entries) '${e.key}': e.value as List};
      marks = [for (final x in (j['marks'] as List? ?? const [])) (x as Map).cast<String, dynamic>()];
      devices = mm(j['devices']);
      dets = [for (final d in (j['dets'] as List? ?? const [])) (d as Map).cast<String, dynamic>()];
      hours = ((j['hours'] as Map?) ?? const {}).map((k, v) => MapEntry('$k', (v as num).toInt()));
      total = (j['total'] as num?)?.toInt() ?? dets.length;
      together = {for (final e in ((j['together'] as Map?) ?? const {}).entries) '${e.key}': (e.value as Map).map((k, v) => MapEntry('$k', (v as num).toInt()))};
    } catch (_) {
      // a torn file: start clean rather than not at all
    }
  }

  void dirty() {
    _save ??= Timer(const Duration(seconds: 5), flush);
  }

  Future<void> flush() async {
    _save?.cancel();
    _save = null;
    try {
      final f = await _f();
      final tmp = File('${f.path}.tmp');
      await tmp.writeAsString(
        jsonEncode({
          'v': 1,
          'aliases': aliases,
          'notes': notes,
          'groups': groups,
          'droneMeta': droneMeta,
          'droneLast': droneLast,
          'droneReports': droneReports,
          'nodes': nodes,
          'filters': filters,
          'marks': marks,
          'devices': devices,
          'dets': dets,
          'hours': hours,
          'total': total,
          'together': together,
        }),
      );
      await tmp.rename(f.path);
    } catch (_) {}
  }

  /// Folds one sighting into the device's record; returns the record.
  Map<String, dynamic> intel(
    String mac,
    double ts, {
    required String dom,
    required String type,
    required String descr,
    required String name,
    required String node,
    int? rssi,
    Map<String, dynamic>? info,
  }) {
    final d = devices.putIfAbsent(mac, () => {'first': ts, 'hits': 0, 'nodes': <String, dynamic>{}, 'intel': <String, dynamic>{}});
    d['last'] = ts;
    d['hits'] = ((d['hits'] as num?) ?? 0).toInt() + 1;
    if (rssi != null && rssi > ((d['best'] as num?) ?? -999)) d['best'] = rssi;
    d['dom'] = dom;
    if (type.isNotEmpty) d['type'] = type;
    if (descr.isNotEmpty) d['descr'] = descr;
    if (name.isNotEmpty) d['name'] = name;
    final ns = (d['nodes'] as Map).cast<String, dynamic>();
    ns[node] = ((ns[node] as num?) ?? 0) + 1;
    d['nodes'] = ns;
    if (info != null) {
      final it = (d['intel'] as Map).cast<String, dynamic>();
      void addTo(String k, Object? v) {
        if (v == null || '$v'.isEmpty) return;
        final l = (it[k] as List?)?.toList() ?? [];
        if (!l.contains(v)) l.add(v);
        if (l.length > 64) l.removeAt(0);
        it[k] = l;
      }

      addTo('names', info['name']);
      final frame = '${info['frame'] ?? ''}';
      addTo(frame == 'probe' ? 'probes' : 'ssids', info['ssid']);
      addTo('aps', info['bssid']);
      addTo('cids', info['cid']);
      for (final u in (info['uuids'] as List? ?? const [])) {
        addTo('uuids', u);
      }
      for (final k in const ['sec', 'ch', 'phy', 'tx', 'app', 'rnd', 'conn', 'wps', 'hidden', 'cc', 'rate']) {
        if (info[k] != null) it[k] = info[k];
      }
      d['intel'] = it;
    }
    _companions(mac, ts);
    if (devices.length > _keepDevices) {
      final oldest = devices.entries.reduce((a, b) => ((a.value['last'] as num) < (b.value['last'] as num)) ? a : b).key;
      devices.remove(oldest);
    }
    dirty();
    return d;
  }

  void _companions(String mac, double ts) {
    _recent[mac] = ts;
    _recent.removeWhere((_, t) => ts - t > 60);
    if ((_recent[mac] ?? 0) != ts) return;
    for (final o in _recent.keys) {
      if (o == mac) continue;
      final a = together.putIfAbsent(mac, () => {});
      a[o] = (a[o] ?? 0) + 1;
    }
    if (together.length > 4000) together.remove(together.keys.first);
  }

  List<Map<String, dynamic>> companions(String mac) {
    final a = together[mac] ?? const {};
    final occ = math.max(1, ((devices[mac]?['hits'] as num?) ?? 1).toInt());
    final l = [
      for (final e in a.entries)
        if (e.value >= 3) {'mac': e.key, 'label': aliases[e.key] ?? devices[e.key]?['name'] ?? devices[e.key]?['descr'] ?? e.key, 'together': e.value, 'occasions': occ},
    ]..sort((x, y) => (y['together'] as int).compareTo(x['together'] as int));
    return l.take(8).toList();
  }

  void addDet(Map<String, dynamic> row, double ts) {
    dets.add(row);
    if (dets.length > _keepDets) dets.removeRange(0, dets.length - _keepDets);
    final h = '${ts ~/ 3600}';
    hours[h] = (hours[h] ?? 0) + 1;
    if (hours.length > 24 * 40) hours.remove(hours.keys.reduce((a, b) => int.parse(a) < int.parse(b) ? a : b));
    total++;
    dirty();
  }

  void addDroneReport(String mac, double ts, Map<String, dynamic> n) {
    final l = droneReports.putIfAbsent(mac, () => []);
    if (l.isNotEmpty && ts - (l.last['ts'] as num) < 2) return; // drones.py STORE_EVERY_S
    l.add({
      'ts': ts,
      for (final k in const ['node', 'lat', 'lon', 'alt', 'hagl', 'spd', 'plat', 'plon'])
        if (n[k] != null) k: n[k],
    });
    if (l.length > _keepReports) l.removeRange(0, l.length - _keepReports);
    final last = droneLast.putIfAbsent(mac, () => {'first': ts, 'reports': 0});
    last['reports'] = ((last['reports'] as num?) ?? 0).toInt() + 1;
    last['last'] = ts;
    for (final k in const ['id', 'op', 'desc', 'lat', 'lon', 'alt', 'hagl', 'spd', 'hdg', 'plat', 'plon', 'src', 'rssi', 'node']) {
      if (n[k] != null && n[k] != '') last[k] = n[k];
    }
    dirty();
  }

  Map<String, dynamic> stats(double now) {
    final h = now ~/ 3600;
    var day = 0;
    for (var i = 0; i < 24; i++) {
      day += hours['${h - i}'] ?? 0;
    }
    return {'total': total, 'day': day, 'macs': devices.length};
  }

  List<Map<String, dynamic>> hourly(double now) {
    final h = now ~/ 3600;
    return [
      for (var i = 23; i >= 0; i--) {'h': (h - i) * 3600, 'c': hours['${h - i}'] ?? 0},
    ];
  }

  // ---- groups
  Map<String, dynamic>? groupFor(String mac) => groups.where((g) => (g['members'] as List).contains(mac)).firstOrNull;
  String groupName(String mac) => '${groupFor(mac)?['name'] ?? ''}';
  Map<String, dynamic> groupOf() => {
    for (final g in groups)
      for (final mac in g['members'] as List) '$mac': {'name': g['name'], 'color': g['color']},
  };

  int groupCreate(String name, int color) {
    final id = groups.fold<int>(0, (a, g) => math.max(a, (g['id'] as num).toInt())) + 1;
    groups.add({'id': id, 'name': name, 'color': color, 'members': <String>[]});
    dirty();
    return id;
  }

  void groupMove(String mac, int? id) {
    for (final g in groups) {
      (g['members'] as List).remove(mac);
    }
    if (id != null) {
      final g = groups.where((g) => g['id'] == id).firstOrNull;
      if (g != null) (g['members'] as List).add(mac);
    }
    dirty();
  }
}
