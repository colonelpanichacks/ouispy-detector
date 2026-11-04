// Wardriving, WiGLE style: every access point and Bluetooth device a wardriver node heard
// (the `wardrive` firmware build prints "WD {json}" lines, src/wardrive.cpp), kept on the
// phone the way home-dashboard/wardrive.py keeps them on the dashboard.
//
// Per device (MAC + kind): name / SSID, security, channel, band, maker, the strongest signal
// and where it was heard (WiGLE places a device where it was loudest), first and last seen,
// sightings, and the sessions it was seen in. Every positioned sighting is logged too, for
// the WiGLE CSV export. A line with no fix of its own takes the phone's GPS (a fix every
// 5 s while a session is recording on a node on USB).
//
// Sessions: a new one starts after 30 min without wardrive data, or on request. With the
// home dashboard as the source its sessions and devices are loaded and merged in
// (/api/wardrive), and the export comes from the dashboard's own WiGLE CSV.
//
// Kinds are open-ended: wifi and ble today; 5 GHz WiFi is a band of wifi, and a Zigbee
// (802.15.4) find slots in as kind 'zigbee' with band 'zigbee'.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../views/map_edit.dart' show phonePosition;
import 'backend.dart';
import 'dashboard.dart';
import 'lookup.dart';

const wdSessionGapS = 1800.0;
const _keepObs = 100000, _keepDevices = 30000;

/// A position: latitude, longitude, accuracy in metres.
class WdFix {
  const WdFix(this.lat, this.lon, [this.acc]);
  final double lat, lon;
  final double? acc;
}

class WdSession {
  WdSession(this.id, this.start, {double? last, this.note = '', this.remote = false}) : last = last ?? start;
  final String id; // 'L3' on the phone, 'D12' on the dashboard
  final double start;
  double last;
  String note;
  final bool remote;

  String get label => remote ? 'Dashboard session ${id.substring(1)}' : 'Session ${id.substring(1)}';

  Map<String, dynamic> toJson() => {'id': id, 'start': start, 'last': last, 'note': note, 'remote': remote};
  static WdSession fromJson(Map<String, dynamic> j) =>
      WdSession('${j['id']}', _d(j['start']) ?? 0, last: _d(j['last']), note: '${j['note'] ?? ''}', remote: j['remote'] == true);
}

/// One positioned sighting (for the WiGLE export and a device's places).
class WdObs {
  WdObs(this.ts, this.mac, this.kind, this.rssi, this.lat, this.lon, this.acc, this.session);
  final double ts;
  final String mac, kind;
  final int? rssi;
  final double lat, lon;
  final double? acc;
  final String session;

  List<dynamic> toJson() => [ts, mac, kind, rssi, lat, lon, acc, session];
  static WdObs fromJson(List<dynamic> l) => WdObs(_d(l[0]) ?? 0, '${l[1]}', '${l[2]}', _i(l[3]), _d(l[4]) ?? 0, _d(l[5]) ?? 0, _d(l[6]), '${l[7]}');
}

class WdDevice {
  WdDevice(this.mac, this.kind);
  final String mac, kind; // kind: wifi | ble | zigbee | whatever a later build sends
  String band = ''; // '2.4' | '5' | 'ble' | 'zigbee'
  String name = ''; // SSID for wifi, advertised name for ble
  String sec = ''; // open | WEP | WPA | WPA2 | WPA3 | WPA2/WPA3
  String maker = '';
  String node = '';
  String std = '', cc = '';
  int? ch, rssi, mfr, app, tx, fl, rate;
  double? lat, lon, acc;
  double first = 0, last = 0;
  int seen = 0;
  bool rnd = false, hidden = false, wps = false;
  List<String> svc = [], vend = [];
  Set<String> sessions = {};

  String get key => '$kind|$mac';
  bool get isWifi => kind == 'wifi';
  bool get placed => lat != null && lon != null;
  bool get open => isWifi && sec == 'open';
  String get label {
    if (name.isNotEmpty) return name;
    if (isWifi) return '(hidden network)';
    if (maker.isNotEmpty) return maker;
    return kind == 'ble' ? 'BLE device' : '${kind.toUpperCase()} device';
  }

  Map<String, dynamic> toJson() => {
    'mac': mac,
    'kind': kind,
    'band': band,
    if (name.isNotEmpty) 'name': name,
    if (sec.isNotEmpty) 'sec': sec,
    if (maker.isNotEmpty) 'maker': maker,
    if (node.isNotEmpty) 'node': node,
    if (std.isNotEmpty) 'std': std,
    if (cc.isNotEmpty) 'cc': cc,
    'ch': ?ch,
    'rssi': ?rssi,
    'mfr': ?mfr,
    'app': ?app,
    'tx': ?tx,
    'fl': ?fl,
    'rate': ?rate,
    'lat': ?lat,
    'lon': ?lon,
    'acc': ?acc,
    'first': first,
    'last': last,
    'seen': seen,
    if (rnd) 'rnd': true,
    if (hidden) 'hidden': true,
    if (wps) 'wps': true,
    if (svc.isNotEmpty) 'svc': svc,
    if (vend.isNotEmpty) 'vend': vend,
    'sessions': sessions.toList(),
  };

  static WdDevice fromJson(Map<String, dynamic> j) {
    final d = WdDevice('${j['mac']}', '${j['kind']}')
      ..band = '${j['band'] ?? ''}'
      ..name = '${j['name'] ?? ''}'
      ..sec = '${j['sec'] ?? ''}'
      ..maker = '${j['maker'] ?? ''}'
      ..node = '${j['node'] ?? ''}'
      ..std = '${j['std'] ?? ''}'
      ..cc = '${j['cc'] ?? ''}'
      ..ch = _i(j['ch'])
      ..rssi = _i(j['rssi'])
      ..mfr = _i(j['mfr'])
      ..app = _i(j['app'])
      ..tx = _i(j['tx'])
      ..fl = _i(j['fl'])
      ..rate = _i(j['rate'])
      ..lat = _d(j['lat'])
      ..lon = _d(j['lon'])
      ..acc = _d(j['acc'])
      ..first = _d(j['first']) ?? 0
      ..last = _d(j['last']) ?? 0
      ..seen = _i(j['seen']) ?? 0
      ..rnd = j['rnd'] == true
      ..hidden = j['hidden'] == true
      ..wps = j['wps'] == true
      ..svc = [for (final s in (j['svc'] as List? ?? const [])) '$s']
      ..vend = [for (final s in (j['vend'] as List? ?? const [])) '$s']
      ..sessions = {for (final s in (j['sessions'] as List? ?? const [])) '$s'};
    return d;
  }
}

/// What a set of devices adds up to.
class WdStats {
  int total = 0, placed = 0, open = 0, hidden = 0, wps = 0, newHour = 0;
  final byKind = <String, int>{}, byBand = <String, int>{}, bySec = <String, int>{};
  final byCh = <int, int>{};
  List<(String, int)> byMaker = [];

  /// Devices first seen in each of the last 24 hours, oldest first; hour 0 starts at [hourStart].
  List<int> newPerHour = List.filled(24, 0);
  double hourStart = 0;
}

class WigleResult {
  const WigleResult(this.ok, this.message);
  final bool ok;
  final String message;
}

double? _d(Object? v) => v is num ? v.toDouble() : (v is String ? double.tryParse(v) : null);
int? _i(Object? v) => v is num ? v.toInt() : (v is bool ? (v ? 1 : 0) : (v is String ? int.tryParse(v) : null));
double? _num(Object? v, double lo, double hi) {
  final x = _d(v);
  if (x == null || x.isNaN || x < lo || x > hi) return null;
  return x;
}

bool _truthy(Object? v) => v == true || (v is num && v != 0) || v == '1' || v == 'true';

class Wardrive extends ChangeNotifier {
  Wardrive({this.persist = true});
  static final I = Wardrive();

  /// False in tests: nothing is read from or written to disk.
  final bool persist;

  final devices = <String, WdDevice>{}; // kind|mac
  final sessions = <String, WdSession>{};
  final obs = <WdObs>[];
  String? currentId;
  int _nextLocal = 1;

  /// Paused: WD lines are not kept.
  bool paused = false;

  /// When the last WD line came in.
  double lastLine = 0;

  /// The phone's latest fix and when it was taken.
  WdFix? fix;
  double fixTs = 0;

  /// Where the phone is; replaceable (tests, a GPS node).
  Future<WdFix?> Function() locate = _phoneFix;

  Backend? _backend;
  StreamSubscription<ConsoleLine>? _sub;
  Timer? _gps, _poll, _save, _ping;
  bool _locating = false, _loaded = false;
  double _lastConsoleTs = 0;
  String _dashSession = '';
  String? _wigleName, _wigleToken;

  static Future<WdFix?> _phoneFix() async {
    final p = await phonePosition();
    return p == null ? null : WdFix(p.latitude, p.longitude, p.accuracy);
  }

  double _now() => DateTime.now().millisecondsSinceEpoch / 1000;

  WdSession? get current => currentId == null ? null : sessions[currentId];

  /// A wardriver is talking (a WD line in the last minute) and lines are being kept.
  bool get recording => !paused && _now() - lastLine < 60;

  bool get onDashboard => _backend is Dashboard;

  /// Sessions, newest first.
  List<WdSession> get sessionList => sessions.values.toList()..sort((a, b) => b.start.compareTo(a.start));

  // ------------------------------------------------------------------ wiring

  /// Takes WD lines from this backend's console (call again when the source changes).
  void attach(Backend b) {
    if (identical(_backend, b)) return;
    _backend = b;
    _poll?.cancel();
    _gps?.cancel();
    _poll = _gps = null;
    attachConsole(b.console);
    if (b is Dashboard) {
      refreshDashboard(history: true);
      _poll = Timer.periodic(const Duration(seconds: 20), (_) => refreshDashboard());
    } else {
      _gps = Timer.periodic(const Duration(seconds: 5), (_) => _gpsTick());
    }
  }

  /// Takes WD lines from [c]: the ones already in its ring that are newer than the last
  /// kept, then the live ones.
  void attachConsole(Console c) {
    _sub?.cancel();
    load();
    for (final l in c.ring.toList()) {
      if (l.ts > _lastConsoleTs) _onConsole(l);
    }
    _sub = c.stream.listen(_onConsole);
  }

  void _onConsole(ConsoleLine c) {
    if (c.src == 'phone') return;
    _lastConsoleTs = math.max(_lastConsoleTs, c.ts);
    ingestLine(c.line, ts: c.ts);
  }

  Future<void> _gpsTick() async {
    if (!recording || _locating) return;
    _locating = true;
    try {
      final f = await locate();
      if (f != null) {
        fix = f;
        fixTs = _now();
      }
    } catch (_) {
    } finally {
      _locating = false;
    }
  }

  WdFix? _freshFix(double now) => fix != null && (now - fixTs).abs() < 20 ? fix : null;

  // ------------------------------------------------------------------ input

  /// One console line; true when it was a WD line that was kept.
  bool ingestLine(String line, {double? ts, String? node}) {
    final l = line.trimLeft();
    if (!l.startsWith('WD ')) return false;
    try {
      final j = jsonDecode(l.substring(3));
      if (j is! Map) return false;
      return ingest(j.cast<String, dynamic>(), ts: ts, node: node);
    } catch (_) {
      return false; // a torn line
    }
  }

  static String kindOf(String t) => switch (t.toLowerCase()) {
    'wifi' || 'wifi5' || 'wifi6' || 'wlan' => 'wifi',
    'ble' || 'bt' => 'ble',
    'zigbee' || '154' || '802154' || '802.15.4' || 'zb' => 'zigbee',
    final o => o,
  };

  static String bandOf(String kind, int? ch, Object? band) {
    final b = '${band ?? ''}'.toLowerCase().replaceAll('ghz', '').trim();
    if (b.isNotEmpty) {
      if (b.startsWith('2')) return '2.4';
      if (b.startsWith('5')) return '5';
      if (b.startsWith('6')) return '6';
      return b;
    }
    return switch (kind) {
      'wifi' => (ch != null && ch > 14) ? '5' : '2.4',
      _ => kind,
    };
  }

  /// One WD object (already JSON-decoded), the firmware's fields: t, mac, ssid, ch, rssi,
  /// sec, hidden, wps, std, rate, cc, vend / name, rnd, mfr, app, tx, fl, svc; lat, lon, acc;
  /// new. Optional: band, node.
  bool ingest(Map<String, dynamic> o, {double? ts, String? node}) {
    if (paused) return false;
    final kind = kindOf('${o['t'] ?? ''}');
    final mac = '${o['mac'] ?? ''}'.toUpperCase();
    if (kind.isEmpty || mac.isEmpty) return false;
    if ((kind == 'wifi' || kind == 'ble') && mac.length != 17) return false;
    final now = ts ?? _now();
    lastLine = math.max(lastLine, now);
    final sess = _sessionFor(now);
    final rssi = _num(o['rssi'], -127, 20)?.round();
    var lat = _num(o['lat'], -90, 90), lon = _num(o['lon'], -180, 180);
    var acc = _num(o['acc'], 0, 100000);
    if (lat == null || lon == null || (lat == 0 && lon == 0)) {
      lat = lon = acc = null;
      final f = _freshFix(now);
      if (f != null) {
        lat = f.lat;
        lon = f.lon;
        acc = f.acc;
      }
    }
    var name = '${(kind == 'wifi' ? o['ssid'] : o['name']) ?? ''}';
    if (name.length > 64) name = name.substring(0, 64);
    final key = '$kind|$mac';
    final d = devices[key] ?? (devices[key] = WdDevice(mac, kind)..first = now);
    if (name.isNotEmpty) d.name = name;
    final sec = '${o['sec'] ?? ''}';
    if (sec.isNotEmpty) d.sec = sec;
    d.ch = _i(o['ch']) ?? d.ch;
    d.band = bandOf(kind, d.ch, o['band']);
    d.mfr = _i(o['mfr']) ?? d.mfr;
    d.app = _i(o['app']) ?? d.app;
    d.tx = _i(o['tx']) ?? d.tx;
    d.fl = _i(o['fl']) ?? d.fl;
    d.rate = _i(o['rate']) ?? d.rate;
    if (o['std'] != null && '${o['std']}'.isNotEmpty) d.std = '${o['std']}';
    if (o['cc'] != null && '${o['cc']}'.isNotEmpty) d.cc = '${o['cc']}';
    if (o.containsKey('hidden')) d.hidden = _truthy(o['hidden']);
    if (o.containsKey('wps')) d.wps = _truthy(o['wps']);
    if (o.containsKey('rnd')) d.rnd = _truthy(o['rnd']);
    if (o['svc'] is List) d.svc = {...d.svc, for (final s in o['svc'] as List) '$s'.toUpperCase()}.toList()..sort();
    final vend = o['vend'] ?? o['vendor'];
    if (vend is List) {
      d.vend = {...d.vend, for (final s in vend) '$s'.toUpperCase()}.toList();
    } else if (vend is String && vend.isNotEmpty) {
      d.maker = vend;
    }
    if (d.maker.isEmpty) d.maker = makerOf(d) ?? '';
    final nd = node ?? '${o['node'] ?? ''}';
    if (nd.isNotEmpty) d.node = nd.toUpperCase();
    // Position: where it was loudest (with a fix).
    final better = lat != null && (d.lat == null || (rssi != null && (d.rssi == null || rssi > d.rssi!)));
    if (better) {
      d.lat = lat;
      d.lon = lon;
      d.acc = acc;
      d.rssi = rssi ?? d.rssi;
    } else if (rssi != null && (d.rssi == null || rssi > d.rssi!)) {
      d.rssi = rssi;
    }
    d.last = math.max(d.last, now);
    d.seen++;
    d.sessions.add(sess.id);
    sess.last = math.max(sess.last, now);
    if (lat != null && lon != null) {
      obs.add(WdObs(now, mac, kind, rssi, lat, lon, acc, sess.id));
      if (obs.length > _keepObs) obs.removeRange(0, obs.length - _keepObs);
    }
    if (devices.length > _keepDevices) {
      final oldest = devices.values.reduce((a, b) => a.last < b.last ? a : b);
      devices.remove(oldest.key);
    }
    _changed();
    return true;
  }

  /// The maker: the line's own, the BLE company ID, else the MAC's OUI (not for random addresses).
  static String? makerOf(WdDevice d) {
    if (d.mfr != null) {
      final c = Lookup.I.companyName(d.mfr!);
      if (c != null) return c;
    }
    if (d.rnd || Lookup.randomMac(d.mac)) return null;
    return Lookup.I.vendor(d.mac);
  }

  WdSession _sessionFor(double now) {
    if (_backend is Dashboard && _dashSession.isNotEmpty) {
      return sessions.putIfAbsent(_dashSession, () => WdSession(_dashSession, now, remote: true));
    }
    final c = current;
    if (c == null || c.remote || now - c.last > wdSessionGapS) return newLocalSession(now);
    return c;
  }

  /// A new session on the phone.
  WdSession newLocalSession([double? at, String note = '']) {
    final s = WdSession('L${_nextLocal++}', at ?? _now(), note: note);
    sessions[s.id] = s;
    currentId = s.id;
    _changed();
    return s;
  }

  /// Starts a new session: on the dashboard when it is the source, else on the phone.
  Future<WdSession> startSession({String note = ''}) async {
    final b = _backend;
    if (b is Dashboard) {
      final r = await b.post('/api/wardrive/session', {'note': note}, okToast: 'new wardrive session');
      final id = _i(r?['session']);
      if (id != null) {
        _dashSession = 'D$id';
        final s = sessions.putIfAbsent(_dashSession, () => WdSession(_dashSession, _now(), note: note, remote: true));
        currentId = s.id;
        _changed();
        return s;
      }
    }
    return newLocalSession(null, note);
  }

  void setPaused(bool v) {
    paused = v;
    _changed();
  }

  /// Forgets one session (its devices stay if another session saw them), or everything.
  void clear({String? session}) {
    if (session == null) {
      devices.clear();
      obs.clear();
      sessions.clear();
      currentId = null;
    } else {
      sessions.remove(session);
      obs.removeWhere((o) => o.session == session);
      devices.removeWhere((_, d) => (d.sessions..remove(session)).isEmpty);
      if (currentId == session) currentId = null;
    }
    _changed();
  }

  // ------------------------------------------------------------------ dashboard

  /// Loads the dashboard's wardrive devices (this session, or the whole history) and merges them.
  Future<void> refreshDashboard({bool history = false}) async {
    final b = _backend;
    if (b is! Dashboard) return;
    final j = await b.getJson('/api/wardrive?scope=${history ? 'history' : 'session'}', timeout: const Duration(seconds: 30));
    if (j is! Map) return;
    mergeDashboard(j.cast<String, dynamic>());
  }

  /// One /api/wardrive reply: {session, session_start, devices: [{mac, kind, name, sec, ch,
  /// mfr, app, rssi, lat, lon, acc, first, last, seen, node, rnd, now, vendor}]}.
  void mergeDashboard(Map<String, dynamic> j) {
    final sid = _i(j['session']);
    if (sid != null) {
      _dashSession = 'D$sid';
      final st = _d(j['session_start']) ?? _now();
      sessions.putIfAbsent(_dashSession, () => WdSession(_dashSession, st, remote: true));
      currentId = _dashSession;
    }
    for (final r in (j['devices'] as List? ?? const [])) {
      if (r is Map) _mergeRemote(r.cast<String, dynamic>());
    }
    _changed();
  }

  void _mergeRemote(Map<String, dynamic> r) {
    final kind = kindOf('${r['kind'] ?? ''}');
    final mac = '${r['mac'] ?? ''}'.toUpperCase();
    if (kind.isEmpty || mac.isEmpty) return;
    final first = _d(r['first']) ?? _now(), last = _d(r['last']) ?? first;
    final d = devices.putIfAbsent('$kind|$mac', () => WdDevice(mac, kind)..first = first);
    d.first = math.min(d.first, first);
    d.last = math.max(d.last, last);
    d.seen = math.max(d.seen, _i(r['seen']) ?? 1);
    if ('${r['name'] ?? ''}'.isNotEmpty) d.name = '${r['name']}';
    if ('${r['sec'] ?? ''}'.isNotEmpty) d.sec = '${r['sec']}';
    d.ch = _i(r['ch']) ?? d.ch;
    d.band = bandOf(kind, d.ch, r['band']);
    d.mfr = _i(r['mfr']) ?? d.mfr;
    d.app = _i(r['app']) ?? d.app;
    if (r.containsKey('rnd')) d.rnd = _truthy(r['rnd']);
    if ('${r['node'] ?? ''}'.isNotEmpty) d.node = '${r['node']}';
    if (r['vendor'] is String && (r['vendor'] as String).isNotEmpty) d.maker = r['vendor'];
    if (d.maker.isEmpty) d.maker = makerOf(d) ?? '';
    final rssi = _i(r['rssi']);
    final lat = _num(r['lat'], -90, 90), lon = _num(r['lon'], -180, 180);
    if (lat != null && lon != null && (d.lat == null || (rssi != null && (d.rssi == null || rssi > d.rssi!)))) {
      d.lat = lat;
      d.lon = lon;
      d.acc = _d(r['acc']);
      d.rssi = rssi ?? d.rssi;
    } else if (rssi != null && (d.rssi == null || rssi > d.rssi!)) {
      d.rssi = rssi;
    }
    if (r['now'] == true && _dashSession.isNotEmpty) {
      d.sessions.add(_dashSession);
      final s = sessions[_dashSession];
      if (s != null) s.last = math.max(s.last, d.last);
    }
  }

  /// Where a device was heard: the phone's sightings, and the dashboard's when it is the source.
  Future<List<WdObs>> sightings(WdDevice d) async {
    final out = [
      for (final o in obs)
        if (o.mac == d.mac && o.kind == d.kind) o,
    ];
    final b = _backend;
    if (b is Dashboard) {
      final j = await b.getJson('/api/wardrive/device/${d.mac}?kind=${d.kind}');
      if (j is Map && j['sightings'] is List) {
        for (final s in j['sightings'] as List) {
          final m = (s as Map).cast<String, dynamic>();
          final la = _d(m['lat']), lo = _d(m['lon']);
          if (la == null || lo == null) continue;
          out.add(WdObs(_d(m['ts']) ?? 0, d.mac, d.kind, _i(m['rssi']), la, lo, _d(m['acc']), 'D${m['session'] ?? ''}'));
        }
      }
    }
    out.sort((a, b) => b.ts.compareTo(a.ts));
    return out;
  }

  // ------------------------------------------------------------------ queries

  /// The devices of one session, or of all of them.
  List<WdDevice> inScope({String? session}) => [
    for (final d in devices.values)
      if (session == null || d.sessions.contains(session)) d,
  ];

  WdStats stats({String? session, double? now}) {
    final t = now ?? _now();
    final st = WdStats();
    final makers = <String, int>{};
    final h0 = (t ~/ 3600 - 23) * 3600.0;
    st.hourStart = h0;
    for (final d in inScope(session: session)) {
      st.total++;
      st.byKind[d.kind] = (st.byKind[d.kind] ?? 0) + 1;
      final band = d.band.isEmpty ? bandOf(d.kind, d.ch, null) : d.band;
      st.byBand[band] = (st.byBand[band] ?? 0) + 1;
      if (d.placed) st.placed++;
      if (d.isWifi) {
        final s = d.sec.isEmpty ? 'unknown' : d.sec;
        st.bySec[s] = (st.bySec[s] ?? 0) + 1;
        if (d.open) st.open++;
        if (d.hidden) st.hidden++;
        if (d.wps) st.wps++;
        if (d.ch != null && d.ch! > 0) st.byCh[d.ch!] = (st.byCh[d.ch!] ?? 0) + 1;
      }
      final mk = d.maker.isNotEmpty ? d.maker : (d.rnd || Lookup.randomMac(d.mac) ? 'Random address' : 'Unknown');
      makers[mk] = (makers[mk] ?? 0) + 1;
      final i = ((d.first - h0) ~/ 3600);
      if (i >= 0 && i < 24) st.newPerHour[i]++;
      if (t - d.first < 3600) st.newHour++;
    }
    st.byMaker = [for (final e in makers.entries) (e.key, e.value)]..sort((a, b) => b.$2.compareTo(a.$2));
    if (st.byMaker.length > 10) st.byMaker = st.byMaker.sublist(0, 10);
    return st;
  }

  /// The wardriving half of the Metrics view, the same keys as wardrive.py metrics().
  Map<String, dynamic> metrics(double since, double until, {String? node, String? dom, required int bucket, required double start, required int nb}) {
    final rows = [
      for (final d in devices.values)
        if (d.last >= since && d.first <= until && (node == null || d.node == node) && (dom == null || (dom == 'wifi' ? d.isWifi : d.kind == dom))) d,
    ];
    final newSeries = <String, List<int>>{'wifi': List.filled(nb, 0), 'ble': List.filled(nb, 0)};
    final kinds = <String, int>{'wifi': 0, 'ble': 0};
    final perNode = <String, int>{}, perMaker = <String, int>{}, perSec = <String, int>{};
    final perCh = <int, int>{};
    var nw = 0, placed = 0, open = 0;
    for (final d in rows) {
      kinds[d.kind] = (kinds[d.kind] ?? 0) + 1;
      if (d.first >= since) {
        nw++;
        final i = math.min(nb - 1, math.max(0, (d.first - start) ~/ bucket));
        newSeries.putIfAbsent(d.kind, () => List.filled(nb, 0))[i]++;
      }
      if (d.placed) placed++;
      if (d.node.isNotEmpty) perNode[d.node] = (perNode[d.node] ?? 0) + 1;
      if (d.isWifi) {
        final s = d.sec.isEmpty ? 'unknown' : d.sec;
        perSec[s] = (perSec[s] ?? 0) + 1;
        if (s == 'open') open++;
        if (d.ch != null && d.ch! > 0) perCh[d.ch!] = (perCh[d.ch!] ?? 0) + 1;
      }
      final mk = d.maker.isNotEmpty ? d.maker : (d.rnd ? 'Random address' : 'Unknown');
      perMaker[mk] = (perMaker[mk] ?? 0) + 1;
    }
    List<Map<String, dynamic>> top(Map<String, int> m, int n) {
      final l = m.entries.toList()..sort((a, b) => b.value.compareTo(a.value));
      return [
        for (final e in l.take(n)) {'name': e.key, 'count': e.value},
      ];
    }

    final nodes = perNode.entries.toList()..sort((a, b) => b.value.compareTo(a.value));
    return {
      'total': rows.length,
      'new': nw,
      'wifi': kinds['wifi'] ?? 0,
      'ble': kinds['ble'] ?? 0,
      'placed': placed,
      'open': open,
      'nodes': perNode.length,
      'new_series': newSeries,
      'by_node': [
        for (final e in nodes) {'id': e.key, 'count': e.value},
      ],
      'by_maker': top(perMaker, 10),
      'by_sec': top(perSec, 8),
      'by_ch': [
        for (final c in (perCh.keys.toList()..sort())) {'ch': c, 'count': perCh[c]},
      ],
    };
  }

  // ------------------------------------------------------------------ WiGLE CSV

  static const wigleHead =
      'WigleWifi-1.4,appRelease=1.0,model=OUI-SPY Mesh,release=1.0,device=OUI-SPY wardriver,'
      'display=,board=,brand=colonelpanic.tech\n'
      'MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,'
      'AltitudeMeters,AccuracyMeters,Type\n';

  static const _auth = {
    'open': '[ESS]',
    'WEP': '[WEP][ESS]',
    'WPA': '[WPA-PSK-CCMP][ESS]',
    'WPA2': '[WPA2-PSK-CCMP][ESS]',
    'WPA3': '[WPA3-SAE-CCMP][ESS]',
    'WPA2/WPA3': '[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]',
  };

  static String _esc(String s) => (s.contains(',') || s.contains('"') || s.contains('\n')) ? '"${s.replaceAll('"', '""')}"' : s;

  static String _utc(double ts) {
    final t = DateTime.fromMillisecondsSinceEpoch((ts * 1000).floor(), isUtc: true);
    String p(int v) => v.toString().padLeft(2, '0');
    return '${t.year.toString().padLeft(4, '0')}-${p(t.month)}-${p(t.day)} ${p(t.hour)}:${p(t.minute)}:${p(t.second)}';
  }

  /// One WiGLE row (wardrive.py wigle_csv), or null for a kind WiGLE has no type for.
  static String? wigleRow(WdDevice d, double ts, int? rssi, double lat, double lon, double? acc) {
    final r = rssi == null ? '' : '$rssi';
    final pos = '${lat.toStringAsFixed(7)},${lon.toStringAsFixed(7)},0,${(acc ?? 0).toStringAsFixed(1)}';
    if (d.kind == 'wifi') return '${d.mac},${_esc(d.name)},${_auth[d.sec] ?? '[ESS]'},${_utc(ts)},${d.ch ?? ''},$r,$pos,WIFI\n';
    if (d.kind == 'ble') return '${d.mac},${_esc(d.name)},Misc [LE],${_utc(ts)},0,$r,$pos,BLE\n';
    return null;
  }

  /// WiGLE CSV (format 1.4), one row per positioned sighting, oldest first, for one session
  /// or all. A placed device with no sighting of its own here (merged from the dashboard)
  /// gets one row at its loudest place.
  String wigleCsv({String? session}) {
    final b = StringBuffer(wigleHead);
    final rows = <(double, String)>[];
    final had = <String>{};
    for (final o in obs) {
      if (session != null && o.session != session) continue;
      final d = devices['${o.kind}|${o.mac}'];
      if (d == null) continue;
      final r = wigleRow(d, o.ts, o.rssi, o.lat, o.lon, o.acc);
      if (r == null) continue;
      rows.add((o.ts, r));
      had.add(d.key);
    }
    for (final d in inScope(session: session)) {
      if (!d.placed || had.contains(d.key)) continue;
      final r = wigleRow(d, d.last, d.rssi, d.lat!, d.lon!, d.acc);
      if (r != null) rows.add((d.last, r));
    }
    rows.sort((a, c) => a.$1.compareTo(c.$1));
    for (final r in rows) {
      b.write(r.$2);
    }
    return b.toString();
  }

  static String wigleFileName([DateTime? at]) {
    final t = at ?? DateTime.now();
    String p(int v) => v.toString().padLeft(2, '0');
    return 'WigleWifi_${t.year}${p(t.month)}${p(t.day)}_${p(t.hour)}${p(t.minute)}${p(t.second)}.csv';
  }

  /// Writes the WiGLE CSV to the app's documents folder and returns its path. With the
  /// dashboard as the source, its current session or whole history comes from the
  /// dashboard's own export (every sighting it logged).
  Future<String> exportWigle({String? session}) async {
    var csv = '';
    final b = _backend;
    if (b is Dashboard && (session == null || session == _dashSession)) {
      try {
        final r = await http
            .get(b.uri('/api/wardrive/wigle.csv?scope=${session == null ? 'history' : 'session'}'), headers: b.authHeaders)
            .timeout(const Duration(seconds: 60));
        if (r.statusCode == 200 && utf8.decode(r.bodyBytes).startsWith('WigleWifi-1.4')) csv = utf8.decode(r.bodyBytes);
      } catch (_) {}
    }
    if (csv.isEmpty) csv = wigleCsv(session: session);
    final dir = await getApplicationDocumentsDirectory();
    final f = File('${dir.path}/${wigleFileName()}');
    await f.writeAsString(csv);
    return f.path;
  }

  /// Rows (sightings) in a CSV file or string, not counting the two header lines.
  static int csvRows(String csv) => math.max(0, '\n'.allMatches(csv).length - 2);

  // ------------------------------------------------------------------ WiGLE upload

  static const _kName = 'wigle_api_name', _kToken = 'wigle_api_token';

  Future<void> _loadWigle() async {
    if (_wigleName != null) return;
    try {
      final p = await SharedPreferences.getInstance();
      _wigleName = p.getString(_kName) ?? '';
      _wigleToken = p.getString(_kToken) ?? '';
    } catch (_) {
      _wigleName = _wigleToken = '';
    }
  }

  /// The user's own WiGLE API name (never the token: that stays inside upload).
  Future<String> wigleName() async {
    await _loadWigle();
    return _wigleName ?? '';
  }

  Future<bool> hasWigle() async {
    await _loadWigle();
    return (_wigleName ?? '').isNotEmpty && (_wigleToken ?? '').isNotEmpty;
  }

  /// Keeps the user's WiGLE API name and token (wigle.net, Account, API Token) on the phone.
  /// Empty clears them.
  Future<void> setWigle(String name, String token) async {
    _wigleName = name.trim();
    _wigleToken = token.trim();
    try {
      final p = await SharedPreferences.getInstance();
      if (_wigleName!.isEmpty || _wigleToken!.isEmpty) {
        await p.remove(_kName);
        await p.remove(_kToken);
        _wigleName = _wigleToken = '';
      } else {
        await p.setString(_kName, _wigleName!);
        await p.setString(_kToken, _wigleToken!);
      }
    } catch (_) {}
    notifyListeners();
  }

  /// Changes the API name, keeping the saved token.
  Future<void> renameWigle(String name) async {
    await _loadWigle();
    await setWigle(name, _wigleToken ?? '');
  }

  /// Uploads a WiGLE CSV to the user's WiGLE account (POST /api/v2/file/upload, basic auth).
  Future<WigleResult> uploadWigle(String path) async {
    if (!await hasWigle()) return const WigleResult(false, 'Set your WiGLE API name and token first.');
    try {
      final req = http.MultipartRequest('POST', Uri.parse('https://api.wigle.net/api/v2/file/upload'))
        ..headers['Authorization'] = 'Basic ${base64Encode(utf8.encode('$_wigleName:$_wigleToken'))}'
        ..headers['Accept'] = 'application/json'
        ..fields['donate'] = 'on'
        ..files.add(await http.MultipartFile.fromPath('file', path));
      final res = await http.Response.fromStream(await req.send().timeout(const Duration(seconds: 120)));
      Map<String, dynamic> j = {};
      try {
        final x = jsonDecode(utf8.decode(res.bodyBytes));
        if (x is Map) j = x.cast<String, dynamic>();
      } catch (_) {}
      if (res.statusCode == 401 || res.statusCode == 403) return const WigleResult(false, 'WiGLE refused the API name / token.');
      final ok = res.statusCode == 200 && j['success'] != false;
      final msg = '${j['message'] ?? j['observer'] ?? ''}'.trim();
      final results = j['results'];
      final extra = results is Map && results['filesize'] != null ? ' (${results['filesize']} bytes)' : '';
      if (ok) return WigleResult(true, msg.isNotEmpty ? 'Uploaded to WiGLE: $msg$extra' : 'Uploaded to WiGLE$extra');
      return WigleResult(false, msg.isNotEmpty ? 'WiGLE: $msg' : 'WiGLE said HTTP ${res.statusCode}');
    } on TimeoutException {
      return const WigleResult(false, 'WiGLE did not answer in time.');
    } catch (e) {
      return WigleResult(false, 'Upload failed (${e.runtimeType}).');
    }
  }

  // ------------------------------------------------------------------ persistence

  Future<File> _f() async => File('${(await getApplicationSupportDirectory()).path}/wardrive.json');

  Future<void> load() async {
    if (_loaded || !persist) return;
    _loaded = true;
    try {
      final f = await _f();
      if (!await f.exists()) return;
      final j = jsonDecode(await f.readAsString()) as Map<String, dynamic>;
      loadJson(j);
    } catch (_) {
      // a torn file: start clean
    }
  }

  /// Merges a saved state in (what [toJson] wrote).
  void loadJson(Map<String, dynamic> j) {
    for (final s in (j['sessions'] as List? ?? const [])) {
      final x = WdSession.fromJson((s as Map).cast<String, dynamic>());
      sessions.putIfAbsent(x.id, () => x);
    }
    for (final d in (j['devices'] as List? ?? const [])) {
      final x = WdDevice.fromJson((d as Map).cast<String, dynamic>());
      devices.putIfAbsent(x.key, () => x);
    }
    obs.insertAll(0, [for (final o in (j['obs'] as List? ?? const [])) WdObs.fromJson(o as List)]);
    _nextLocal = math.max(_nextLocal, _i(j['next']) ?? 1);
    currentId ??= j['current'] as String?;
    paused = j['paused'] == true;
    notifyListeners();
  }

  Map<String, dynamic> toJson() => {
    'v': 1,
    'next': _nextLocal,
    'current': currentId,
    'paused': paused,
    'sessions': [for (final s in sessions.values) s.toJson()],
    'devices': [for (final d in devices.values) d.toJson()],
    'obs': [for (final o in obs) o.toJson()],
  };

  void _changed() {
    if (persist) _save ??= Timer(const Duration(seconds: 5), flush);
    _ping ??= Timer(const Duration(milliseconds: 300), () {
      _ping = null;
      notifyListeners();
    });
  }

  Future<void> flush() async {
    _save?.cancel();
    _save = null;
    if (!persist) return;
    try {
      final f = await _f();
      final tmp = File('${f.path}.tmp');
      await tmp.writeAsString(jsonEncode(toJson()));
      await tmp.rename(f.path);
    } catch (_) {}
  }

  @override
  void dispose() {
    _sub?.cancel();
    _gps?.cancel();
    _poll?.cancel();
    _save?.cancel();
    _ping?.cancel();
    super.dispose();
  }
}
