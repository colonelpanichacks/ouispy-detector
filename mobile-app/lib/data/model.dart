// Live copy of the dashboard's state, mirroring tab5-dashboard/src/model.*. Every source
// (the dashboard today; a USB or BLE node later, see docs/PHASE2.md) feeds it through the
// same ingest_* calls, and the views only ever read it.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';

const targetLiveS = 60.0; // server SEEN_WINDOW: "heard lately"
const targetKeepS = 600.0; // drop a target this long after it was last heard
const alertCooldownS = 600.0; // same MAC again within this = no new alert
const droneLiveS = 60.0;
const maxDets = 2000;
const nodeSilentS = 90.0; // Tab5 NODE_TIMEOUT (75 s) plus a status round

double _d(Object? v, [double def = 0]) => v is num ? v.toDouble() : def;
double? _dn(Object? v) => v is num ? v.toDouble() : null;
int? _in(Object? v) => v is num ? v.round() : null;
String _s(Object? v) => v is String ? v : (v == null ? '' : '$v');

class Seen {
  Seen(this.node, this.rssi, this.age);
  final String node;
  final int rssi;
  final double age;
  static List<Seen> list(Object? o) {
    if (o is! List) return [];
    return [
      for (final e in o)
        if (e is List && e.isNotEmpty) Seen(_s(e[0]), _in(e.length > 1 ? e[1] : null) ?? -127, _d(e.length > 2 ? e[2] : 0)),
    ];
  }
}

class Det {
  Det.fromJson(Map<String, dynamic> o)
      : id = _in(o['id']) ?? 0,
        ts = _d(o['ts']),
        node = _s(o['node']),
        mac = _s(o['mac']).toUpperCase(),
        descr = _s(o['descr']),
        alias = _s(o['alias']),
        type = _s(o['type']),
        dom = _s(o['dom']),
        closest = _s(o['closest']),
        name = _s(o['name']),
        group = _s(o['group']),
        source = _s(o['source']),
        rssi = _in(o['rssi']),
        isNew = o['is_new'] == true || o['is_new'] == 1,
        seen = Seen.list(o['seen_by']);
  final int id;
  final double ts;
  final String node, mac, descr, alias, type, dom, closest, name, group, source;
  final int? rssi;
  final bool isNew;
  final List<Seen> seen;
}

class Target {
  Target(this.mac);
  final String mac;
  String name = '', descr = '', type = '', dom = '';
  double first = 0, last = 0;
  int hits = 0;
  List<Seen> seen = [];
  Map<String, dynamic>? info;
  // Triangulated fix from the dashboard (tri.py).
  bool triOk = false, triRough = false;
  double triLat = 0, triLon = 0, triErr = 0, triTs = 0;
  int? get best => seen.isEmpty ? null : seen.first.rssi;
}

class FleetNode {
  FleetNode(this.raw);
  final Map<String, dynamic> raw;
  String get id => _s(raw['id']);
  String get alias => _s(raw['alias']);
  String get label => alias.isNotEmpty ? alias : id;
  String get role => _s(raw['role']);
  // The dashboard can keep a node "online" after its link to the fleet went quiet (the
  // home's serial gone): a node unheard for longer than the relay timeout is silent.
  bool get flagOnline => raw['online'] == true;
  bool get online => flagOnline && age < nodeSilentS;
  bool get silent => flagOnline && age >= nodeSilentS;
  bool get isHome => raw['is_home'] == true;
  int? get rssi => _in(raw['rssi']);
  double get age => _d(raw['age']);
  double get since => _d(raw['since']);
  int? get hue => _in(raw['color']);
  Object? get filtersFresh => raw['filters_fresh'];
  Map<String, dynamic> get cfg => (raw['cfg'] as Map?)?.cast<String, dynamic>() ?? const {};
  Map<String, dynamic> get ota => (raw['ota'] as Map?)?.cast<String, dynamic>() ?? const {};
  Map<String, dynamic> get tlm => (raw['tlm'] as Map?)?.cast<String, dynamic>() ?? const {};
  double? get lat => _dn(raw['lat']);
  double? get lon => _dn(raw['lon']);
  bool get hasPos => lat != null && lon != null;
  String get fw => _s(cfg['fw']);
  int? get lrssi => _in(cfg['lrssi']);
  int? get filters => _in(cfg['filters']);
  int? get peers => _in(cfg['peers']);
  int? get ch => _in(cfg['ch']);
  int? get up => _in(cfg['up']);
  int? get heap => _in(cfg['heap']);
  int? get buzzer => _in(cfg['buzzer']);
  int? get led => _in(cfg['led']);
  List<Seen> get nbr => Seen.list(cfg['nbr']);
  /// What it runs, from the build stamp's last letter (as the dashboard and the Tab5 do).
  String get kind {
    final f = fw;
    if (f.isEmpty) return 'detector';
    return switch (f[f.length - 1]) { 'W' => 'wardriver', 'Y' => 'wardriver gps', 'S' => 'sky-spy', _ => 'detector' };
  }
}

class Drone {
  Drone(this.raw);
  Map<String, dynamic> raw;
  String get mac => _s(raw['mac']).toUpperCase();
  String get alias => _s(raw['alias']);
  String get serial => _s(raw['id']);
  String get op => _s(raw['op']);
  String get desc => _s(raw['desc']);
  String get ua => _s(raw['ua_type']);
  String get status => _s(raw['status']);
  String get tag => _s(raw['tag']).isEmpty ? 'unknown' : _s(raw['tag']);
  String get src => _s(raw['src']);
  String get closest => _s(raw['closest'] ?? raw['node']);
  String get vendor => _s(raw['vendor']);
  String get notes => _s(raw['notes']);
  String get label => alias.isNotEmpty ? alias : (serial.isNotEmpty ? serial : (maker.isNotEmpty ? maker : mac));
  double? get lat => _dn(raw['lat']);
  double? get lon => _dn(raw['lon']);
  double? get plat => _dn(raw['plat']);
  double? get plon => _dn(raw['plon']);
  double? get alt => _dn(raw['alt']);
  double? get hagl => _dn(raw['hagl']);
  double? get spd => _dn(raw['spd']);
  double? get vspd => _dn(raw['vspd']);
  double? get hdg => _dn(raw['hdg']);
  int? get rssi => _in(raw['rssi']);
  int? get pilotDist => _in(raw['pilot_dist_m']);
  double get first => _d(raw['first']);
  double get last => _d(raw['last']);
  int get reports => _in(raw['reports'] ?? raw['n']) ?? 0;
  bool get hasPos => lat != null && lon != null && !(lat == 0 && lon == 0);
  bool get hasPilot => plat != null && plon != null && !(plat == 0 && plon == 0);
  /// [[ts, lat, lon], ...] or [[lat, lon], ...]: points only.
  List<List<double>> _pts(Object? o) {
    if (o is! List) return [];
    final out = <List<double>>[];
    for (final p in o) {
      if (p is! List || p.length < 2) continue;
      final n = p.map((e) => _d(e)).toList();
      out.add(n.length >= 3 ? [n[1], n[2]] : [n[0], n[1]]);
    }
    return out;
  }

  List<List<double>> get track => _pts(raw['track'] ?? raw['rtrack']);
  List<List<double>> get ptrack => _pts(raw['ptrack']);
  /// FAA registry make + model for its serial, when the dashboard looked it up.
  String get maker {
    final items = (((raw['faa'] as Map?)?['data'] as Map?)?['items'] as List?);
    if (items == null || items.isEmpty) return '';
    final i = items.first as Map;
    return '${_s(i['makeName'])} ${_s(i['modelName'])}'.trim();
  }
}

class Flight {
  Flight(Map<String, dynamic> o)
      : start = _d(o['start']),
        end = _d(o['end']),
        reports = _in(o['reports']) ?? 0,
        maxH = _dn(o['max_hagl']),
        agl = o['agl'] == true,
        maxSpd = _dn(o['max_spd']),
        dist = _in(o['dist_m']),
        pilotMax = _in(o['pilot_max_m']),
        pilot = (o['pilot'] is List) ? (o['pilot'] as List).map((e) => _d(e)).toList() : null,
        track = [for (final p in (o['track'] as List? ?? const [])) if (p is List) p.map((e) => _d(e)).toList()],
        ptrack = [for (final p in (o['ptrack'] as List? ?? const [])) if (p is List) p.map((e) => _d(e)).toList()];
  final double start, end;
  final int reports;
  final double? maxH, maxSpd;
  final bool agl;
  final int? dist, pilotMax;
  final List<double>? pilot;
  final List<List<double>> track, ptrack;
}

class Mark {
  Mark(this.raw);
  final Map<String, dynamic> raw;
  String get id => _s(raw['id']);
  String get kind => _s(raw['kind']);
  String get name => _s(raw['name']);
  String get icon => _s(raw['icon']);
  int? get hue => _in(raw['hue']);
  double? get lat => _dn(raw['lat']);
  double? get lon => _dn(raw['lon']);
  List<List<double>> get pts => [for (final p in (raw['pts'] as List? ?? const [])) if (p is List && p.length >= 2) [_d(p[0]), _d(p[1])]];
}

class Alert {
  Alert(this.det);
  final Det det;
}

/// The drones window: hours back (1/4/8/24), all time (-1), or a span (0).
class DroneWindow {
  const DroneWindow.hours(this.hours) : since = 0, until = 0;
  const DroneWindow.all() : hours = -1, since = 0, until = 0;
  const DroneWindow.span(this.since, this.until) : hours = 0;
  final int hours;
  final double since, until;
  String get query => hours < 0
      ? 'since=0'
      : hours > 0
          ? 'hours=$hours'
          : until > 0
              ? 'since=${since.round()}&until=${until.round()}'
              : 'since=${since.round()}';
  bool contains(double t, double now) {
    if (hours < 0) return true;
    if (hours > 0) return t >= now - hours * 3600;
    return t >= since && (until <= 0 || t <= until);
  }
}

enum Link { off, connecting, up, refused }

class Model extends ChangeNotifier {
  // ---- link
  Link link = Link.off;
  String lastError = '';
  String target = ''; // host:port as resolved
  String mode = ''; // live / demo / replay
  double clockOffset = 0; // server time - local time

  // ---- data
  final List<Det> dets = []; // newest first
  final Map<String, Target> targets = {};
  final Map<String, FleetNode> nodes = {};
  Map<String, dynamic> home = {};
  final Map<String, Drone> drones = {};
  final Map<String, Map<String, dynamic>> aliases = {};
  final Map<String, String> deviceNames = {};
  final Map<String, String> groupOf = {};
  List<Mark> marks = [];
  Map<String, dynamic> stats = {};
  List<Map<String, dynamic>> hourly = [];
  List<String> droneTags = const ['unknown', 'friendly', 'neutral', 'suspicious', 'hostile'];
  int events = 0;
  double lastEvent = 0;

  // ---- alerts (as the Tab5: a live detection of a MAC not heard for 10 min, not muted)
  final List<Alert> alerts = [];
  int alertsUnacked = 0;
  bool muteAll = false;
  final Set<String> muted = {};
  final _alertCtl = StreamController<Alert>.broadcast();
  Stream<Alert> get alertStream => _alertCtl.stream;
  final _toastCtl = StreamController<String>.broadcast();
  Stream<String> get toasts => _toastCtl.stream;

  DroneWindow droneWindow = const DroneWindow.hours(24);

  double get now => DateTime.now().millisecondsSinceEpoch / 1000 + clockOffset;

  void _clock(double serverTs) {
    if (serverTs <= 0) return;
    final local = DateTime.now().millisecondsSinceEpoch / 1000;
    // Event times trail the server clock a little; only ever move forward.
    final off = serverTs - local;
    if (clockOffset == 0 || off > clockOffset) clockOffset = off;
  }

  // Coalesce bursts (one socket event per serial line) into one rebuild a frame.
  Timer? _tick;
  void changed() {
    _tick ??= Timer(const Duration(milliseconds: 120), () {
      _tick = null;
      notifyListeners();
    });
  }

  void toast(String s) => _toastCtl.add(s);

  String nodeName(String id) {
    final n = nodes[id];
    return n == null || n.alias.isEmpty ? id : n.alias;
  }

  String aliasOf(String mac) => _s(aliases[mac.toUpperCase()]?['alias']);
  String nameOf(String mac) => deviceNames[mac.toUpperCase()] ?? targets[mac.toUpperCase()]?.name ?? '';
  String labelOf(String mac, [String fallback = '']) {
    final a = aliasOf(mac);
    if (a.isNotEmpty) return a;
    final n = nameOf(mac);
    if (n.isNotEmpty) return n;
    return fallback.isNotEmpty ? fallback : mac;
  }

  int get liveDrones => drones.values.where((d) => now - d.last < droneLiveS).length;
  int get nodesOnline => nodes.values.where((n) => n.online).length;

  List<Target> get liveTargets {
    final l = targets.values.where((t) => now - t.last < targetKeepS).toList();
    l.sort((a, b) => b.last.compareTo(a.last));
    return l;
  }

  List<Drone> get windowDrones {
    final t = now;
    final l = drones.values.where((d) => droneWindow.contains(d.last, t)).toList();
    l.sort((a, b) => b.last.compareTo(a.last));
    return l;
  }

  // ------------------------------------------------------------ ingest

  void ingestState(Map<String, dynamic> s) {
    mode = _s(s['mode']);
    if (s['fleet'] is Map) ingestFleet((s['fleet'] as Map).cast<String, dynamic>(), notify: false);
    if (s['stats'] is Map) stats = (s['stats'] as Map).cast<String, dynamic>();
    if (s['hourly'] is List) hourly = [for (final h in s['hourly']) (h as Map).cast<String, dynamic>()];
    if (s['mac_aliases'] is Map) _aliases(s['mac_aliases']);
    if (s['device_names'] is Map) {
      deviceNames
        ..clear()
        ..addAll((s['device_names'] as Map).map((k, v) => MapEntry(_s(k).toUpperCase(), _s(v))));
    }
    if (s['group_of'] is Map) _groups(s['group_of']);
    if (s['marks'] is List) marks = [for (final m in s['marks']) Mark((m as Map).cast<String, dynamic>())];
    if (s['drones'] is List) {
      for (final d in s['drones']) {
        ingestDrone((d as Map).cast<String, dynamic>(), notify: false);
      }
    }
    if (s['recent'] is List) {
      final rec = [for (final d in (s['recent'] as List).reversed) Det.fromJson((d as Map).cast<String, dynamic>())];
      for (final d in rec) {
        _addDet(d, live: false);
      }
    }
    if (s['tri'] is List) {
      for (final f in s['tri']) {
        ingestTri((f as Map).cast<String, dynamic>(), notify: false);
      }
    }
    changed();
  }

  void ingestFleet(Map<String, dynamic> f, {bool notify = true}) {
    if (f['home'] is Map) home = (f['home'] as Map).cast<String, dynamic>();
    _clock(_d(f['ts']));
    if (f['nodes'] is List) {
      final seen = <String>{};
      for (final n in f['nodes']) {
        final node = FleetNode((n as Map).cast<String, dynamic>());
        if (node.id.isEmpty) continue;
        seen.add(node.id);
        nodes[node.id] = node;
      }
      nodes.removeWhere((k, _) => !seen.contains(k));
    }
    if (notify) changed();
  }

  void _aliases(Object o) {
    aliases
      ..clear()
      ..addAll((o as Map).map((k, v) => MapEntry(_s(k).toUpperCase(), v is Map ? v.cast<String, dynamic>() : {'alias': _s(v)})));
  }

  void _groups(Object o) {
    groupOf
      ..clear()
      ..addAll((o as Map).map((k, v) => MapEntry(_s(k).toUpperCase(), v is Map ? _s(v['name'] ?? v['alias']) : _s(v))));
  }

  void ingestAliases(Map<String, dynamic> o) {
    _aliases(o);
    changed();
  }

  void ingestGroups(Map<String, dynamic> o) {
    if (o['group_of'] is Map) _groups(o['group_of']);
    changed();
  }

  void ingestMarks(Map<String, dynamic> o) {
    if (o['marks'] is List) marks = [for (final m in o['marks']) Mark((m as Map).cast<String, dynamic>())];
    changed();
  }

  int _maxDetId = 0;
  void _addDet(Det d, {required bool live}) {
    if (d.mac.isEmpty) return;
    if (d.id != 0 && d.id <= _maxDetId && dets.any((x) => x.id == d.id)) return;
    if (d.id > _maxDetId) _maxDetId = d.id;
    final prev = targets[d.mac];
    final cooled = prev == null || d.ts - prev.last > alertCooldownS;
    dets.insert(0, d);
    if (dets.length > maxDets) dets.removeLast();
    _target(d.mac, d.descr, d.type, d.dom, d.ts, d.seen, hit: true, name: d.name);
    _clock(d.ts);
    if (live && cooled && !muted.contains(d.mac)) {
      final a = Alert(d);
      alerts.insert(0, a);
      if (alerts.length > 200) alerts.removeLast();
      alertsUnacked++;
      if (!muteAll) _alertCtl.add(a);
    }
    events++;
    lastEvent = d.ts;
  }

  void ingestDet(Map<String, dynamic> o) {
    _addDet(Det.fromJson(o), live: true);
    changed();
  }

  void _target(String mac, String descr, String type, String dom, double ts, List<Seen> seen, {required bool hit, String name = ''}) {
    final t = targets.putIfAbsent(mac, () => Target(mac)..first = ts);
    if (descr.isNotEmpty) t.descr = descr;
    if (type.isNotEmpty) t.type = type;
    if (dom.isNotEmpty) t.dom = dom;
    if (name.isNotEmpty) t.name = name;
    if (ts > t.last) t.last = ts;
    if (t.first == 0 || ts < t.first) t.first = ts;
    if (seen.isNotEmpty) t.seen = seen;
    if (hit) t.hits++;
  }

  void ingestSighting(Map<String, dynamic> s) {
    final mac = _s(s['mac']).toUpperCase();
    if (mac.isEmpty) return;
    final ts = _d(s['ts']);
    _target(mac, _s(s['descr']), _s(s['type']), _s(s['dom']), ts, Seen.list(s['seen_by']), hit: false, name: _s(s['name']));
    if (s['info'] is Map) targets[mac]!.info = (s['info'] as Map).cast<String, dynamic>();
    _clock(ts);
    changed();
  }

  void ingestCleared(Map<String, dynamic> e) {
    final node = _s(e['node']), mac = _s(e['mac']).toUpperCase();
    dets.removeWhere((d) => (node.isEmpty || d.node == node) && (mac.isEmpty || d.mac == mac));
    if (node.isEmpty && mac.isEmpty) targets.clear();
    changed();
  }

  void ingestTri(Map<String, dynamic> f, {bool notify = true}) {
    final mac = _s(f['mac']).toUpperCase();
    if (mac.isEmpty) return;
    final ok = f['ok'] == true;
    if (!ok && !targets.containsKey(mac)) return;
    final t = targets.putIfAbsent(mac, () => Target(mac)..first = _d(f['ts'])..last = _d(f['ts']));
    t.triOk = ok;
    t.triRough = f['rough'] == true;
    t.triLat = _d(f['lat']);
    t.triLon = _d(f['lon']);
    t.triErr = _d(f['err']);
    t.triTs = _d(f['ts']);
    if (notify) changed();
  }

  void ingestDrone(Map<String, dynamic> o, {bool notify = true}) {
    final mac = _s(o['mac']).toUpperCase();
    if (mac.isEmpty) return;
    final cur = drones[mac];
    if (cur == null) {
      drones[mac] = Drone(Map.of(o));
    } else {
      // An edit ("meta") carries the name/tag/notes; a report carries the rest. Merge, and
      // keep the longer track (a live report carries only its recent tail).
      final keepTrack = cur.raw['track'];
      cur.raw.addAll(o);
      if (o['track'] is! List || (keepTrack is List && (o['track'] as List).length < keepTrack.length && o['meta'] == true)) {
        if (keepTrack != null) cur.raw['track'] = keepTrack;
      }
    }
    _clock(_d(o['last']));
    if (notify) changed();
  }

  void pruneTargets() {
    final t = now;
    final before = targets.length;
    targets.removeWhere((_, v) => t - v.last > targetKeepS && !v.triOk);
    if (targets.length != before) changed();
  }

  // ------------------------------------------------------------ alerts

  void ackAlerts() {
    alertsUnacked = 0;
    changed();
  }

  void setMuteAll(bool v) {
    muteAll = v;
    changed();
  }

  void toggleMute(String mac) {
    mac = mac.toUpperCase();
    if (!muted.remove(mac)) muted.add(mac);
    changed();
  }

  void clearData() {
    dets.clear();
    targets.clear();
    drones.clear();
    nodes.clear();
    home = {};
    _maxDetId = 0;
    changed();
  }

  @override
  void dispose() {
    _alertCtl.close();
    _toastCtl.close();
    super.dispose();
  }
}

/// Great-circle distance in metres.
double distM(double la1, double lo1, double la2, double lo2) {
  const r = 6371000.0;
  final p1 = la1 * math.pi / 180, p2 = la2 * math.pi / 180;
  final dp = (la2 - la1) * math.pi / 180, dl = (lo2 - lo1) * math.pi / 180;
  final a = math.sin(dp / 2) * math.sin(dp / 2) + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) * math.sin(dl / 2);
  return 2 * r * math.asin(math.sqrt(a));
}
