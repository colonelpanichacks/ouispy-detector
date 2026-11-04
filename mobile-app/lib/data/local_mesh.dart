// The app as the dashboard: a node's console (USB now, BLE next) read straight into the
// model, doing what home-dashboard does with the same lines (fleet.py, drones.py, tri.py,
// commands.py), so every view works with no Mac anywhere. Names, groups, notes, tags,
// node placements, marks, detections and drone reports are kept on the phone (store.dart).
//
// Lines (src/main.cpp, mesh.cpp, rid.cpp): a detection JSON {mac, rssi, type, desc, node,
// dom, upd, info}, `TLM {..}` every 5 s with the roster, `RST {..}` a node's status, `RFL`
// its filters, `RID {..}` Remote ID, `POS`/`GPS` positions, `OTA`, `CMD`/`ACK`/`OK`/`ERR`
// command replies, and the legacy `[MESH] node X joined|offline` text.
import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'backend.dart';
import 'lookup.dart';
import 'model.dart';
import 'store.dart';
import 'tri.dart';
import 'usb_link.dart';
import 'watch.dart';

// fleet.py
const _homeTimeout = 15.0, _nodeStale = 12.0, _legacyStale = 600.0, _dedupS = 6.0, _seenWindow = 60.0;
// drones.py
const _uaTypes = [
  'None',
  'Aeroplane',
  'Multirotor / helicopter',
  'Gyroplane',
  'Hybrid lift (VTOL)',
  'Ornithopter',
  'Glider',
  'Kite',
  'Free balloon',
  'Captive balloon',
  'Airship',
  'Parachute',
  'Rocket',
  'Tethered powered aircraft',
  'Ground obstacle',
  'Other',
];
const _status = ['Undeclared', 'Ground', 'Airborne', 'Emergency', 'Remote ID system failure'];
const droneTags = ['unknown', 'friendly', 'neutral', 'suspicious', 'hostile'];
const _flightGap = 300.0, _flightAirGap = 1500.0, _flightAirM = 10.0;
final _meshTag = RegExp(r'\s*\[mesh ([0-9A-F]{6})\]\s*$');
final _joined = RegExp(r'\[MESH\] node ([0-9A-F]{6}) joined(?: \((home|remote)\))?');
final _offline = RegExp(r'\[MESH\] node ([0-9A-F]{6}) offline');

double _now() => DateTime.now().millisecondsSinceEpoch / 1000;
double? _num(Object? v, {List<num> bad = const [], double? lo, double? hi}) {
  final d = v is num ? v.toDouble() : (v is String ? double.tryParse(v) : null);
  if (d == null || bad.contains(d) || (lo != null && d < lo) || (hi != null && d > hi)) return null;
  return d;
}

String _s(Object? v) => v == null ? '' : '$v';

class _Node {
  _Node(this.id);
  final String id;
  String role = 'unknown';
  bool online = false;
  double lastSeen = 0, since = 0;
  int pkts = 0;
  int? rssi;
  Map<String, dynamic> cfg = {}, ota = {}, tlm = {};
}

class _Job {
  _Job(this.lines, this.want, this.label, {this.wants});
  final List<String> lines;
  final String want; // OK, CMD, RST, RFL
  final List<String>? wants; // per line, when they differ (a filter transfer)
  final String label;
  final done = Completer<Map<String, dynamic>?>();
}

class LocalMesh implements Backend {
  LocalMesh(this.m, this.link) : store = MeshStore.I {
    tri = Tri(_pos);
  }
  final Model m;
  final LineLink link;
  final MeshStore store;
  late final Tri tri;

  final _nodes = <String, _Node>{};
  Map<String, dynamic> _home = {};
  String homeId = '';
  double _homeSeen = 0, _lastTlm = 0;
  final _sightings = <String, Map<String, (int, double)>>{}; // mac -> node -> (rssi, ts)
  final _dedup = <String, double>{};
  final _drones = <String, Map<String, dynamic>>{};
  StreamSubscription<String>? _sub;
  StreamSubscription<bool>? _st;
  Timer? _tick;
  int _detId = 0;
  final _q = <_Job>[];
  _Job? _job;
  DateTime _jobSent = DateTime(0);
  int _jobLine = 0;
  DateTime _lastPoll = DateTime(0);

  @override
  String get label => 'usb';

  (double, double)? _pos(String id) {
    final n = store.nodes[id];
    final lat = n?['lat'], lon = n?['lon'];
    return lat is num && lon is num ? (lat.toDouble(), lon.toDouble()) : null;
  }

  @override
  Future<void> start() async {
    await store.load();
    _detId = store.dets.isEmpty ? 0 : (store.dets.last['id'] as num? ?? 0).toInt();
    m.mode = 'usb';
    m.link = Link.connecting;
    m.target = 'USB  ${link.name}';
    m.lastError = 'waiting for the node on USB';
    _sub = link.lines.listen(_line);
    _st = link.state.listen((up) {
      if (!up) {
        m.link = Link.connecting;
        m.lastError = 'node unplugged or silent';
        m.changed();
      }
    });
    await fetchState();
    if (!await link.start()) {
      m.lastError = 'could not open the node on USB (allow it when Android asks)';
      m.changed();
    }
    _tick = Timer.periodic(const Duration(seconds: 1), (_) => _second());
  }

  @override
  Future<void> stop() async {
    _tick?.cancel();
    await _sub?.cancel();
    await _st?.cancel();
    await link.stop();
    await store.flush();
    m.link = Link.off;
  }

  // ------------------------------------------------------------ lines

  void _line(String l) {
    if (l.startsWith('OK') || l.startsWith('ERR') || l.startsWith('CMD ') || l.startsWith('ACK ') || l.startsWith('RST ') || l.startsWith('RFL ')) {
      _reply(l);
    }
    try {
      if (l.startsWith('TLM ')) return _tlm(_json(l.substring(4)));
      if (l.startsWith('RST ')) return _rst(_json(l.substring(4)));
      if (l.startsWith('RFL ')) return _rfl(_json(l.substring(4)));
      if (l.startsWith('RID ')) return _rid(_json(l.substring(4)));
      if (l.startsWith('OTA ')) return _otaLine(_json(l.substring(4)));
      if (l.startsWith('POS ')) return _posLine(_json(l.substring(4)));
      if (l.startsWith('GPS ')) return _gpsLine(_json(l.substring(4)));
      final body = l.startsWith('DET ') ? l.substring(4) : l;
      if (body.startsWith('{')) {
        final j = _json(body);
        if (j.containsKey('drone_lat')) return _rid(j);
        if (j.containsKey('mac')) return _det(j);
        return;
      }
      final jn = _joined.firstMatch(l);
      if (jn != null) return _seen(jn.group(1)!, role: jn.group(2));
      final off = _offline.firstMatch(l);
      if (off != null) {
        _nodes[off.group(1)!]?.online = false;
        return _pushFleet();
      }
    } catch (_) {
      // a torn or foreign line: skip it
    }
  }

  Map<String, dynamic> _json(String s) => (jsonDecode(s) as Map).cast<String, dynamic>();

  _Node _n(String id) => _nodes.putIfAbsent(id, () => _Node(id));

  void _seen(String id, {String? role, double? last, int? pkts, int? rssi}) {
    if (id.isEmpty) return;
    final now = _now();
    final n = _n(id);
    if (role != null && role.isNotEmpty && role != 'unknown') n.role = role;
    final t = now - (last ?? 0);
    if (t > n.lastSeen) n.lastSeen = t;
    if (pkts != null) n.pkts = pkts;
    if (rssi != null && rssi != 0) n.rssi = rssi;
    if (!n.online) {
      n.online = true;
      n.since = now;
      m.toast('${m.nodeName(id)} joined');
      // read its status shortly after it shows up (commands.py)
      Future.delayed(const Duration(seconds: 4), () => nodeCmd(id, 'status'));
    }
  }

  void _tlm(Map<String, dynamic> t) {
    final now = _now();
    final id = _s(t['id']).toUpperCase();
    final role = _s(t['role']);
    _lastTlm = now;
    if (role == 'home' || role == 'solo') {
      if (homeId != id) {
        homeId = id;
        Future.delayed(const Duration(seconds: 1), () => nodeCmd(id, 'status'));
      }
      _homeSeen = now;
      _home = {...t, 'id': id, 'source': 'usb'};
      final n = _n(id);
      n.role = role;
      n.tlm = t;
      _seen(id, role: role);
      final roster = <String>{id};
      for (final r in (t['nodes'] as List? ?? const [])) {
        if (r is! Map) continue;
        final rid = _s(r['id']).toUpperCase();
        roster.add(rid);
        _seen(rid, role: _s(r['role']), last: _num(r['last']), pkts: (r['pkts'] as num?)?.toInt(), rssi: (r['rssi'] as num?)?.toInt());
      }
      // the roster is the truth: an online node it no longer lists has gone
      for (final n in _nodes.values) {
        if (n.online && !roster.contains(n.id)) _offlineNode(n);
      }
    } else {
      _seen(id, role: role);
      _n(id).tlm = t;
    }
    if (m.link != Link.up) {
      m.link = Link.up;
      m.lastError = '';
      m.target = 'USB  ${homeId.isEmpty ? link.name : homeId}';
    }
    _pushFleet();
  }

  void _offlineNode(_Node n) {
    if (!n.online) return;
    n.online = false;
    if (n.id != homeId) m.toast('${m.nodeName(n.id)} went offline');
  }

  void _rst(Map<String, dynamic> c) {
    final id = _s(c['node']).toUpperCase();
    if (id.isEmpty) return;
    final n = _n(id);
    n.cfg = c;
    if (_s(c['role']).isNotEmpty) n.role = _s(c['role']);
    if (homeId.isEmpty && c['role'] == 'home') homeId = id;
    final a = _s(c['alias']);
    if (a.isNotEmpty && _s(store.nodes[id]?['alias']).isEmpty) {
      store.nodes.putIfAbsent(id, () => {})['alias'] = a;
      store.dirty();
    }
    _seen(id);
    _pushFleet();
  }

  void _rfl(Map<String, dynamic> f) {
    final id = _s(f['node']).toUpperCase();
    if (id.isEmpty) return;
    store.filters[id] = f['filters'] is List ? f['filters'] as List : [];
    store.dirty();
    m.toast('${m.nodeName(id)}: ${store.filters[id]!.length} filters');
  }

  void _otaLine(Map<String, dynamic> o) {
    final id = _s(o['node']).toUpperCase();
    if (id.isEmpty) return;
    _n(id).ota = {...o, 'pct': o['pct']};
    _pushFleet();
  }

  void _posLine(Map<String, dynamic> p) {
    final id = _s(p['node']).toUpperCase();
    _place(id, _num(p['lat'], lo: -90, hi: 90), _num(p['lon'], lo: -180, hi: 180));
  }

  void _gpsLine(Map<String, dynamic> p) {
    if (homeId.isEmpty || p['fix'] == 0) return;
    _place(homeId, _num(p['lat'], lo: -90, hi: 90), _num(p['lon'], lo: -180, hi: 180));
  }

  void _place(String id, double? lat, double? lon) {
    if (id.isEmpty || lat == null || lon == null || (lat == 0 && lon == 0)) return;
    final n = store.nodes.putIfAbsent(id, () => {});
    n['lat'] = lat;
    n['lon'] = lon;
    store.dirty();
    _pushFleet();
  }

  // ------------------------------------------------------------ detections

  List<List<Object>> _seenBy(String mac, double now) {
    final per = _sightings[mac] ?? const {};
    final out = [
      for (final e in per.entries)
        if (now - e.value.$2 <= _seenWindow) [e.key, e.value.$1, ((now - e.value.$2) * 10).round() / 10],
    ];
    out.sort((a, b) => (b[1] as int).compareTo(a[1] as int));
    return out;
  }

  void _det(Map<String, dynamic> d) {
    final now = _now();
    final mac = _s(d['mac']).toUpperCase();
    if (mac.isEmpty) return;
    var desc = _s(d['desc']);
    final tag = _meshTag.firstMatch(desc);
    if (tag != null) desc = desc.substring(0, tag.start);
    var node = _s(d['node']).toUpperCase();
    if (node.isEmpty) node = tag?.group(1) ?? (homeId.isEmpty ? 'HOME' : homeId);
    final type = _s(d['type']).toUpperCase();
    final dom = _s(d['dom']).isNotEmpty ? _s(d['dom']) : (type == 'PROBE' || type == 'BEACON' ? 'wifi' : 'ble');
    final rssi = (d['rssi'] as num?)?.toInt();
    if (rssi != null) _sightings.putIfAbsent(mac, () => {})[node] = (rssi, now);
    _seen(node);
    tri.add(mac, node, rssi, dom, now);
    final info = d['info'] is Map ? (d['info'] as Map).cast<String, dynamic>() : null;
    final name = _s(info?['name']).isNotEmpty ? _s(info!['name']) : (_s(info?['frame']) == 'beacon' || _s(info?['frame']) == 'probe-resp' ? _s(info?['ssid']) : '');
    final dev = store.intel(mac, now, dom: dom, type: type, descr: desc, name: name, node: node, rssi: rssi, info: info);
    final seenBy = _seenBy(mac, now);
    if (d['upd'] == 1 || d['upd'] == true) {
      m.ingestSighting({'mac': mac, 'ts': now, 'descr': desc, 'type': type, 'dom': dom, 'name': name, 'seen_by': seenBy, 'info': ?info});
      return;
    }
    final k = '$node|$mac|$type';
    if (now - (_dedup[k] ?? 0) < _dedupS) return;
    _dedup[k] = now;
    final isNew = dev['hits'] == 1;
    final row = <String, dynamic>{
      'id': ++_detId,
      'ts': now,
      'node': node,
      'mac': mac,
      'alias': store.aliases[mac] ?? _s(d['alias']),
      'rssi': rssi,
      'type': type,
      'match': _s(d['match']),
      'descr': desc,
      'dom': dom,
      'source': 'usb',
      'name': name,
      'group': store.groupName(mac),
      'is_new': isNew,
      'seen_by': seenBy,
      'closest': seenBy.length >= 2 ? seenBy.first[0] : '',
    };
    store.addDet(row, now);
    m.ingestDet(row);
  }

  // ------------------------------------------------------------ drones (drones.py)

  void _rid(Map<String, dynamic> o) {
    final now = _now();
    if (o.containsKey('drone_lat') || o.containsKey('drone_long')) {
      o = {
        'mac': o['mac'],
        'rssi': o['rssi'],
        'id': o['basic_id'],
        'lat': o['drone_lat'],
        'lon': o['drone_long'],
        'alt': o['drone_altitude'],
        'plat': o['pilot_lat'],
        'plon': o['pilot_long'],
        'node': o['node'],
        'hagl': o['height_agl'],
        'spd': o['speed'],
        'hdg': o['heading'],
        'op': o['op_id'],
        'src': 'skyspy',
      };
    }
    final mac = _s(o['mac']).toUpperCase();
    if (mac.isEmpty) return;
    var lat = _num(o['lat'], lo: -90, hi: 90), lon = _num(o['lon'], lo: -180, hi: 180);
    if (lat == 0 && lon == 0) lat = lon = null;
    var plat = _num(o['plat'], lo: -90, hi: 90), plon = _num(o['plon'], lo: -180, hi: 180);
    if (plat == 0 && plon == 0) plat = plon = null;
    final n = {
      'node': _s(o['node']).toUpperCase(),
      'rssi': o['rssi'],
      'src': _s(o['src']).isEmpty ? 'rid' : o['src'],
      'id': _s(o['id']).trim(),
      'idt': o['idt'],
      'uat': o['uat'],
      'op': _s(o['op']).trim(),
      'desc': _s(o['desc']).trim(),
      'lat': lat,
      'lon': lon,
      'alt': _num(o['alt'], bad: [-1000]),
      'hagl': _num(o['hagl'], bad: [-1000]),
      'spd': _num(o['spd'], bad: [255, 254.25], lo: 0),
      'vspd': _num(o['vspd'], bad: [63, 62]),
      'hdg': _num(o['hdg'], lo: 0, hi: 360),
      'st': o['st'],
      'plat': plat,
      'plon': plon,
    };
    final s = _drones.putIfAbsent(
      mac,
      () => {'mac': mac, 'first': now, 'track': <List<double>>[], 'ptrack': <List<double>>[], 'nodes': <String, Map<String, dynamic>>{}, 'reports': 0},
    );
    n.forEach((k, v) {
      if (v != null && v != '') s[k] = v;
    });
    s['last'] = now;
    s['reports'] = (s['reports'] as int) + 1;
    final node = n['node'] as String;
    if (node.isNotEmpty) {
      _seen(node);
      final per = (s['nodes'] as Map<String, Map<String, dynamic>>).putIfAbsent(node, () => {'n': 0});
      per['n'] = (per['n'] as int) + 1;
      per['rssi'] = n['rssi'];
      per['last'] = now;
    }
    String closest = '';
    int best = -999;
    (s['nodes'] as Map<String, Map<String, dynamic>>).forEach((k, v) {
      final r = (v['rssi'] as num?)?.toInt() ?? -999;
      if (now - (v['last'] as double) < 30 && r > best) {
        best = r;
        closest = k;
      }
    });
    s['closest'] = closest;
    final tr = s['track'] as List<List<double>>;
    if (lat != null && lon != null && (tr.isEmpty || tr.last[1] != lat || tr.last[2] != lon)) {
      tr.add([now, lat, lon, (n['alt'] as double?) ?? 0, (n['hagl'] as double?) ?? 0]);
      if (tr.length > 600) tr.removeAt(0);
    }
    final pt = s['ptrack'] as List<List<double>>;
    if (plat != null && plon != null && (pt.isEmpty || pt.last[1] != plat || pt.last[2] != plon)) {
      pt.add([now, plat, plon]);
      if (pt.length > 600) pt.removeAt(0);
    }
    store.addDroneReport(mac, now, n);
    final pub = _dronePublic(s, now);
    final first = s['reports'] == 1;
    m.ingestDrone(pub);
    if (first) m.toast('Drone: ${pub['alias'] != '' ? pub['alias'] : (n['id'] != '' ? n['id'] : mac)}');
    // a drone is a device in the feed like any other (drones.py on_det)
    _det({
      'mac': mac,
      'rssi': n['rssi'],
      'type': 'RID',
      'desc': 'DRONE ${[pub['alias'], n['desc'], n['id'], pub['ua_type']].firstWhere((x) => _s(x).isNotEmpty, orElse: () => mac)}',
      'node': node,
      'dom': n['src'] == 'ble' ? 'ble' : 'wifi',
    });
  }

  Map<String, dynamic> _dronePublic(Map<String, dynamic> s, double now) {
    final meta = store.droneMeta[s['mac']] ?? const {};
    final lat = s['lat'], lon = s['lon'], plat = s['plat'], plon = s['plon'];
    int? pd;
    if (lat is double && lon is double && plat is double && plon is double) pd = _hav(lat, lon, plat, plon).round();
    String label(List<String> t, Object? i) {
      final k = i is num ? i.toInt() : int.tryParse('$i');
      return k != null && k >= 0 && k < t.length ? t[k] : '';
    }

    return {
      ...Map.of(s)..remove('nodes'),
      'ua_type': label(_uaTypes, s['uat']),
      'status': label(_status, s['st']),
      'vendor': Lookup.I.vendor(s['mac'] as String) ?? '',
      'pilot_dist_m': pd,
      'alias': meta['alias'] ?? '',
      'tag': meta['tag'] ?? 'unknown',
      'notes': meta['notes'] ?? '',
      'live': now - (s['last'] as double) < droneLiveS,
    };
  }

  static double _hav(double a1, double o1, double a2, double o2) {
    const r = 6371000.0;
    final p1 = a1 * math.pi / 180, p2 = a2 * math.pi / 180, dp = (a2 - a1) * math.pi / 180, dl = (o2 - o1) * math.pi / 180;
    final h = math.sin(dp / 2) * math.sin(dp / 2) + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) * math.sin(dl / 2);
    return 2 * r * math.asin(math.sqrt(h));
  }

  // ------------------------------------------------------------ the clock

  void _second() {
    final now = _now();
    // fleet.py tick: a node goes offline once it has been quiet past its timeout
    final homeQuiet = now - _homeSeen > _homeTimeout;
    for (final n in _nodes.values) {
      if (!n.online) continue;
      final legacy = n.cfg['fmt'] != null || _s(n.cfg['fw']).isEmpty;
      final limit = n.id == homeId ? _homeTimeout : ((legacy || homeQuiet) ? _legacyStale : _nodeStale);
      if (now - n.lastSeen > limit) _offlineNode(n);
    }
    if (m.link == Link.up && now - _lastTlm > 15) {
      m.link = Link.connecting;
      m.lastError = 'no telemetry from the node for ${(now - _lastTlm).round()} s';
    }
    // triangulate what was heard in the last second
    for (final mac in _sightings.keys.toList()) {
      final per = _sightings[mac]!;
      if (per.values.every((v) => now - v.$2 > 2)) continue;
      if (!tri.due(mac, now)) continue;
      final f = tri.fix(mac, now);
      if (f['ok'] == true || m.targets[mac]?.triOk == true) m.ingestTri(f, notify: false);
    }
    if (now.round() % 30 == 0) {
      _sightings.removeWhere((_, per) => per.values.every((v) => now - v.$2 > 600));
      _dedup.removeWhere((_, t) => now - t > 60);
      tri.forget(now);
      fetchStats();
    }
    // every 2 min, as the dashboard does: the home's status and everyone else's
    if (homeId.isNotEmpty && DateTime.now().difference(_lastPoll).inSeconds >= 120) {
      _lastPoll = DateTime.now();
      nodeCmd(homeId, 'status');
      nodeCmd('all', 'status');
    }
    _pump();
    _pushFleet();
    m.pruneTargets();
  }

  void _pushFleet() {
    final now = _now();
    m.ingestFleet({
      'ts': now,
      'home': {..._home, 'alive': now - _homeSeen < _homeTimeout},
      'nodes': [
        for (final n in _nodes.values)
          {
            'id': n.id,
            'alias': store.nodes[n.id]?['alias'] ?? '',
            'role': n.role,
            'online': n.online,
            'is_home': n.id == homeId,
            'rssi': n.rssi,
            'age': n.lastSeen == 0 ? 1e9 : now - n.lastSeen,
            'since': n.since,
            'color': store.nodes[n.id]?['color'],
            'filters_fresh': null,
            'cfg': n.cfg,
            'ota': n.ota,
            'tlm': n.tlm,
            'lat': store.nodes[n.id]?['lat'],
            'lon': store.nodes[n.id]?['lon'],
          },
      ],
    }, notify: false);
    m.changed();
  }

  // ------------------------------------------------------------ commands (commands.py)

  /// The console lines for an op, and the reply that settles it.
  _Job? _plan(String id, String op, Map<String, dynamic> a) {
    final local = id == homeId;
    final tgt = id;
    int b(Object? v) => (v == true || v == 1) ? 1 : 0;
    String one(String l, String r) => local ? l : r;
    switch (op) {
      case 'status':
        return _Job([one('status', 'rstatus $tgt')], local ? 'RST' : 'CMD', 'status');
      case 'audio':
        return _Job([one('audio ${b(a['buzzer'])} ${b(a['led'])}', 'raudio $tgt ${b(a['buzzer'])} ${b(a['led'])}')], local ? 'OK' : 'CMD', 'audio');
      case 'identify':
        final s = ((a['secs'] as num?)?.toInt() ?? 10).clamp(1, 120);
        return _Job([one('ident $s', 'rident $tgt $s 3')], local ? 'OK' : 'CMD', 'identify');
      case 'locate':
        final md = (a['mode'] as num?)?.toInt() ?? 0;
        return _Job([one('locate $md', 'rlocate $tgt $md')], local ? 'OK' : 'CMD', 'locate');
      case 'reboot':
        return _Job([one('reboot', 'rreboot $tgt')], local ? 'OK' : 'CMD', 'reboot');
      case 'preset':
        final n = _s(a['name']);
        if (!const ['axon', 'meta', 'ipro', 'verkada'].contains(n)) return null;
        return _Job([one('preset $n', 'rpreset $tgt $n')], local ? 'OK' : 'CMD', 'preset $n');
      case 'alias':
        final v = _s(a['alias']).replaceAll(RegExp(r'["\\]'), '');
        final w = v.isEmpty ? '-' : (v.length > 32 ? v.substring(0, 32) : v);
        return _Job([one('nodealias $w', 'ralias $tgt $w')], local ? 'OK' : 'CMD', 'name');
      case 'wardrive':
        final words = <String>[if (a['on'] != null) (a['on'] == true ? 'on' : 'off'), if (a['ch'] != null) 'ch ${a['ch']}'];
        return _Job([for (final w in words) one('wd $w', 'rwd $tgt $w')], local ? 'OK' : 'CMD', 'wardrive');
      case 'filters_get':
        return _Job([one('filters', 'rfilters $tgt')], local ? 'RFL' : 'CMD', 'filters');
      case 'rechan':
        final ch = (a['ch'] as num?)?.toInt() ?? 0;
        if (ch < 1 || ch > 11) return null;
        return _Job(['meshrechan $ch'], 'OK', 'channel $ch');
      case 'meshpush':
        return _Job(['meshpush'], 'CMD', 'filter push');
      case 'filters_set' || 'filters_add' || 'filters_del':
        // the filter transfer (commands.py): mfbegin|mfadd|mfdel <self|id>, mfdata <hex>..., mfend
        final fs = [for (final f in (a['filters'] as List? ?? const [])) (f as Map).cast<String, dynamic>()];
        final buf = encodeFilters(fs);
        if (buf.isEmpty || buf.length > 4000) return null;
        final verb = {'filters_set': 'mfbegin', 'filters_add': 'mfadd', 'filters_del': 'mfdel'}[op]!;
        final hex = [for (var i = 0; i < buf.length; i += 96) buf.sublist(i, math.min(buf.length, i + 96)).map((x) => x.toRadixString(16).padLeft(2, '0')).join().toUpperCase()];
        final lines = ['$verb ${local ? 'self' : tgt}', for (final h in hex) 'mfdata $h', 'mfend'];
        return _Job(lines, 'OK', '${fs.length} filters', wants: [for (var i = 0; i < lines.length - 1; i++) 'OK', local ? 'OK' : 'CMD']);
      case 'raw':
        return _Job([_s(a['line'])], 'OK', _s(a['line']));
    }
    return null;
  }

  @override
  Future<Map<String, dynamic>?> nodeCmd(String id, String op, [Map<String, dynamic>? args]) async {
    if (!link.open) {
      m.toast('the node is not connected');
      return null;
    }
    if (op == 'fw_install') {
      m.toast('firmware installs from the phone are not built yet');
      return null;
    }
    final j = _plan(id, op, args ?? const {});
    if (j == null) {
      m.toast("the node can't do $op");
      return null;
    }
    _q.add(j);
    _pump();
    return j.done.future;
  }

  void _pump() {
    final j = _job;
    if (j != null) {
      if (DateTime.now().difference(_jobSent).inSeconds < 4) return;
      _settle(null, 'no answer from the node');
      return;
    }
    if (_q.isEmpty) return;
    _job = _q.removeAt(0);
    _jobLine = 0;
    _sendLine();
  }

  void _sendLine() {
    final j = _job!;
    _jobSent = DateTime.now();
    link.send(j.lines[_jobLine]);
  }

  void _reply(String l) {
    final j = _job;
    if (j == null) return;
    final verb = j.lines[_jobLine].split(' ').first;
    if (l.startsWith('ERR')) {
      // an error about some other verb is not ours (commands.py)
      final other = RegExp(r'^ERR (\w+):').firstMatch(l)?.group(1);
      if (other != null && other != verb) return;
      return _settle(null, l.substring(3).trim());
    }
    final ok = switch (j.wants?[_jobLine] ?? j.want) {
      'OK' => l.startsWith('OK') || l.startsWith('CMD '),
      'CMD' => l.startsWith('CMD '),
      'RST' => l.startsWith('RST '),
      'RFL' => l.startsWith('RFL '),
      _ => false,
    };
    if (!ok) return;
    if (_jobLine + 1 < j.lines.length) {
      _jobLine++;
      return _sendLine();
    }
    _settle({'ok': true, 'reply': l}, null);
  }

  void _settle(Map<String, dynamic>? r, String? err) {
    final j = _job;
    if (j == null) return;
    _job = null;
    if (err != null && j.label != 'status') m.toast('${j.label}: $err');
    if (r != null && j.label != 'status') m.toast('${j.label} sent');
    j.done.complete(r);
    _pump();
  }

  // ------------------------------------------------------------ the views' reads

  @override
  Future<void> fetchState() async {
    await store.load();
    m.ingestAliases({for (final e in store.aliases.entries) e.key: e.value});
    m.ingestGroups({'group_of': store.groupOf()});
    m.ingestMarks({'marks': store.marks});
    m.droneTags = droneTags;
    if (m.dets.isEmpty) {
      m.ingestState({
        'mode': 'usb',
        'recent': [for (final d in store.dets.reversed.take(300)) d],
      });
    }
    await fetchStats();
    await fetchDroneWindow();
  }

  @override
  Future<void> fetchStats() async {
    m.stats = store.stats(_now());
    m.hourly = store.hourly(_now());
    m.changed();
  }

  @override
  Future<void> fetchDroneWindow() async {
    final now = _now();
    final w = m.droneWindow;
    final since = w.hours > 0 ? now - w.hours * 3600 : (w.hours == 0 ? w.since : 0.0);
    final until = w.hours == 0 ? w.until : double.infinity;
    for (final e in store.droneLast.entries) {
      final last = (e.value['last'] as num?)?.toDouble() ?? 0;
      if (last < since || last > until) continue;
      final live = _drones[e.key];
      m.ingestDrone(live != null ? _dronePublic(live, now) : {...e.value, 'mac': e.key, ...?store.droneMeta[e.key], 'live': false}, notify: false);
    }
    m.droneTags = droneTags;
    m.changed();
  }

  @override
  Future<List<Flight>> flights(String mac) async {
    final rows = store.droneReports[mac.toUpperCase()] ?? const [];
    final groups = <List<Map<String, dynamic>>>[];
    var cur = <Map<String, dynamic>>[];
    for (final r in rows) {
      if (cur.isNotEmpty) {
        final gap = (r['ts'] as num) - (cur.last['ts'] as num);
        final aloft = ((r['hagl'] as num?) ?? 0) > _flightAirM && ((cur.last['hagl'] as num?) ?? 0) > _flightAirM;
        if (gap > (aloft ? _flightAirGap : _flightGap)) {
          groups.add(cur);
          cur = [];
        }
      }
      cur.add(r);
    }
    if (cur.isNotEmpty) groups.add(cur);
    final out = <Flight>[];
    for (final g in groups.reversed.take(10)) {
      final tr = [
        for (final r in g)
          if (r['lat'] is num && r['lon'] is num) [(r['ts'] as num).toDouble(), (r['lat'] as num).toDouble(), (r['lon'] as num).toDouble()],
      ];
      final pt = [
        for (final r in g)
          if (r['plat'] is num && r['plon'] is num) [(r['ts'] as num).toDouble(), (r['plat'] as num).toDouble(), (r['plon'] as num).toDouble()],
      ];
      var dist = 0.0;
      for (var i = 1; i < tr.length; i++) {
        dist += _hav(tr[i - 1][1], tr[i - 1][2], tr[i][1], tr[i][2]);
      }
      double? pmax;
      if (pt.isNotEmpty) {
        for (final t in tr) {
          pmax = math.max(pmax ?? 0, _hav(pt.first[1], pt.first[2], t[1], t[2]));
        }
      }
      double? mx(String k) {
        final v = [
          for (final r in g)
            if (r[k] is num) (r[k] as num).toDouble(),
        ];
        return v.isEmpty ? null : v.reduce(math.max);
      }

      final hagl = mx('hagl');
      out.add(
        Flight({
          'start': g.first['ts'],
          'end': g.last['ts'],
          'reports': g.length,
          'max_hagl': hagl ?? mx('alt'),
          'agl': hagl != null,
          'max_spd': mx('spd'),
          'dist_m': dist.round(),
          'pilot_max_m': pmax?.round(),
          'pilot': pt.isEmpty ? null : [pt.first[1], pt.first[2]],
          'track': [
            for (final t in tr) [t[1], t[2]],
          ],
          'ptrack': [
            for (final t in pt) [t[1], t[2]],
          ],
        }),
      );
    }
    return out;
  }

  @override
  Future<Map<String, dynamic>?> device(String mac) async {
    mac = mac.toUpperCase();
    final d = store.devices[mac];
    if (d == null) return null;
    final it = (d['intel'] as Map?)?.cast<String, dynamic>() ?? const {};
    final rows = <List<String>>[];
    void row(String k, Object? v) {
      final s = v is List ? v.join(', ') : _s(v);
      if (s.isNotEmpty) rows.add([k, s]);
    }

    final vendor = Lookup.I.vendor(mac);
    row('MAC vendor', vendor ?? (Lookup.randomMac(mac) ? 'random address' : 'not registered'));
    row('Broadcast name', it['names']);
    if (d['dom'] == 'wifi') {
      row('SSID', it['ssids']);
      row('Looks for', it['probes']);
      row('Security', it['sec']);
      row('Channel', it['ch']);
      row('Standard', it['phy']);
    } else {
      final cids = [for (final c in (it['cids'] as List? ?? const [])) Lookup.I.companyName(int.tryParse('$c'.replaceFirst('0x', ''), radix: 16) ?? -1) ?? '$c'];
      row('Company', cids);
      final app = it['app'];
      if (app is num) row('Appearance', Lookup.I.appearanceName(app.toInt()) ?? '$app');
      row('Services', [for (final u in (it['uuids'] as List? ?? const [])) Lookup.I.uuidName(int.tryParse('$u'.replaceFirst('0x', ''), radix: 16) ?? -1) ?? '$u']);
      row('TX power', it['tx'] is num ? '${it['tx']} dBm' : null);
    }
    row('Type', d['type']);
    row('Description', d['descr']);
    row('First seen', _clock(d['first']));
    row('Last seen', _clock(d['last']));
    row('Detections', d['hits']);
    row('Best signal', d['best'] is num ? '${d['best']} dBm' : null);
    row('Heard by', [for (final n in (d['nodes'] as Map? ?? const {}).keys) m.nodeName('$n')]);
    final g = store.groupFor(mac);
    return {
      'rows': rows,
      'companions': store.companions(mac),
      'groups': [
        for (final x in store.groups) {'id': x['id'], 'name': x['name'], 'n': (x['members'] as List).length},
      ],
      'group': g == null ? null : {'id': g['id'], 'name': g['name']},
      'alias': store.aliases[mac] ?? '',
      'notes': store.notes[mac] ?? '',
      'name': _s((it['names'] as List?)?.firstOrNull),
    };
  }

  String _clock(Object? ts) {
    if (ts is! num) return '';
    final t = DateTime.fromMillisecondsSinceEpoch((ts * 1000).round());
    String two(int v) => v.toString().padLeft(2, '0');
    return '${t.year}-${two(t.month)}-${two(t.day)} ${two(t.hour)}:${two(t.minute)}';
  }

  @override
  Future<Map<String, dynamic>?> history({String q = '', String dom = '', int limit = 300}) async {
    final ql = q.toLowerCase();
    final all = [
      for (final e in store.devices.entries)
        if ((dom.isEmpty || e.value['dom'] == dom) &&
            (ql.isEmpty || '${e.key} ${e.value['descr']} ${e.value['name']} ${store.aliases[e.key] ?? ''} ${Lookup.I.vendor(e.key) ?? ''}'.toLowerCase().contains(ql)))
          {
            'mac': e.key,
            'dom': e.value['dom'],
            'type': e.value['type'],
            'name': e.value['name'],
            'descr': e.value['descr'],
            'last': e.value['last'],
            'hits': e.value['hits'],
            'best': e.value['best'],
            'nodes': (e.value['nodes'] as Map? ?? const {}).keys.toList(),
          },
    ]..sort((a, b) => ((b['last'] as num?) ?? 0).compareTo((a['last'] as num?) ?? 0));
    return {'rows': all.take(limit).toList(), 'total': all.length};
  }

  // ------------------------------------------------------------ the views' writes

  @override
  Future<void> setAlias(String mac, String alias) async {
    mac = mac.toUpperCase();
    if (alias.isEmpty) {
      store.aliases.remove(mac);
    } else {
      store.aliases[mac] = alias;
    }
    store.dirty();
    m.ingestAliases({for (final e in store.aliases.entries) e.key: e.value});
    m.toast(alias.isEmpty ? 'name cleared' : 'named $alias');
  }

  @override
  Future<void> setNotes(String mac, String notes) async {
    store.notes[mac.toUpperCase()] = notes;
    store.dirty();
    m.toast('notes saved');
  }

  @override
  Future<void> groupAdd(int id, String mac) async {
    store.groupMove(mac.toUpperCase(), id);
    m.ingestGroups({'group_of': store.groupOf()});
    m.toast('added to group');
  }

  @override
  Future<void> groupRemove(int id, String mac) async {
    store.groupMove(mac.toUpperCase(), null);
    m.ingestGroups({'group_of': store.groupOf()});
    m.toast('taken out of group');
  }

  @override
  Future<void> groupNew(String name, String mac) async {
    final id = store.groupCreate(name, DateTime.now().millisecond % 360);
    store.groupMove(mac.toUpperCase(), id);
    m.ingestGroups({'group_of': store.groupOf()});
    m.toast('group $name made');
  }

  @override
  Future<void> droneMeta(String mac, Map<String, dynamic> body) async {
    mac = mac.toUpperCase();
    final d = store.droneMeta.putIfAbsent(mac, () => {});
    for (final k in const ['alias', 'tag', 'notes']) {
      if (body.containsKey(k)) d[k] = body[k];
    }
    store.dirty();
    m.ingestDrone({'mac': mac, ...d, 'meta': true});
    m.toast('saved');
  }

  @override
  Future<void> nodeSet(String id, Map<String, dynamic> body) async {
    final n = store.nodes.putIfAbsent(id, () => {});
    for (final k in const ['alias', 'color', 'lat', 'lon']) {
      if (body.containsKey(k)) n[k] = body[k];
    }
    store.dirty();
    // a name goes onto the node too, so the mesh and the Tab5 show it
    if (body['alias'] is String) nodeCmd(id, 'alias', {'alias': body['alias']});
    _pushFleet();
    m.toast('saved');
  }

  @override
  Future<void> addMark(Map<String, dynamic> mark) async {
    store.marks.add({'id': 'p${DateTime.now().millisecondsSinceEpoch}', ...mark});
    store.dirty();
    m.ingestMarks({'marks': store.marks});
    m.toast('mark saved');
  }
}
