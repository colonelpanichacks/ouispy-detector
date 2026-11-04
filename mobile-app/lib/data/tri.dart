// RSSI triangulation, a straight port of home-dashboard/tri.py (itself the web map's
// triFixCompute), so the phone shows the same fix the dashboard and the Tab5 would:
//
//   RSSI_i = P0 - 10 n log10(r_i) + noise(sigma), P0 integrated out per grid cell,
//   posterior exp(-chi^2/2) over a grid round the nodes, position = the best cell refined
//   by its 5x5 posterior mean, error = radius holding 68% of the posterior, smoothed.
//
// The model is the dashboard's settings.tri: p0_ble / p0_wifi / n / sigma / max_err /
// smooth, per-node RSSI offsets (dB subtracted from a node's readings) and per-device
// calibrations {p0, n, rms, nodes, ts} from "calibrate at a spot". On USB the phone keeps
// its own copy ([triModel], in shared_preferences) and every Tri without a model of its own
// (LocalMesh's) reads it.
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'model.dart';

const triWindowS = 45.0;
const _kMax = 5;
const calibrateS = 30;

double _d(Object? v, double def) => v is num ? v.toDouble() : (double.tryParse('${v ?? ''}') ?? def);

/// One device's calibration: its power at 1 m and path-loss exponent, the fit's error.
class TriCal {
  const TriCal({required this.p0, required this.n, this.rms = 0, this.nodes = 0, this.ts = 0});
  final double p0, n, rms, ts;
  final int nodes;
  Map<String, dynamic> toJson() => {'p0': p0, 'n': n, 'rms': rms, 'nodes': nodes, 'ts': ts};
  static TriCal fromJson(Map<String, dynamic> j) =>
      TriCal(p0: _d(j['p0'], -59), n: _d(j['n'], 2.7), rms: _d(j['rms'], 0), nodes: (j['nodes'] as num?)?.toInt() ?? 0, ts: _d(j['ts'], 0));
}

class TriModel {
  const TriModel({
    this.p0Ble = -59,
    this.p0Wifi = -40,
    this.n = 2.7,
    this.sigma = 4,
    this.maxErr = 8,
    this.smooth = 0.35,
    this.offsets = const {},
    this.devices = const {},
  });
  final double p0Ble, p0Wifi, n, sigma, maxErr, smooth;

  /// node id -> dB taken off that node's readings (a deaf or hot radio).
  final Map<String, double> offsets;

  /// MAC -> its calibration.
  final Map<String, TriCal> devices;

  TriModel copyWith({
    double? p0Ble,
    double? p0Wifi,
    double? n,
    double? sigma,
    double? maxErr,
    double? smooth,
    Map<String, double>? offsets,
    Map<String, TriCal>? devices,
  }) => TriModel(
    p0Ble: p0Ble ?? this.p0Ble,
    p0Wifi: p0Wifi ?? this.p0Wifi,
    n: n ?? this.n,
    sigma: sigma ?? this.sigma,
    maxErr: maxErr ?? this.maxErr,
    smooth: smooth ?? this.smooth,
    offsets: offsets ?? this.offsets,
    devices: devices ?? this.devices,
  );

  /// The dashboard's settings.tri shape.
  Map<String, dynamic> toJson() => {
    'p0_ble': p0Ble,
    'p0_wifi': p0Wifi,
    'n': n,
    'sigma': sigma,
    'max_err': maxErr,
    'smooth': smooth,
    'offsets': offsets,
    'devices': {for (final e in devices.entries) e.key: e.value.toJson()},
  };

  static TriModel fromJson(Map<String, dynamic>? j) {
    if (j == null) return const TriModel();
    const d = TriModel();
    return TriModel(
      p0Ble: _d(j['p0_ble'], d.p0Ble),
      p0Wifi: _d(j['p0_wifi'], d.p0Wifi),
      n: _d(j['n'], d.n),
      sigma: _d(j['sigma'], d.sigma),
      maxErr: _d(j['max_err'], d.maxErr),
      smooth: _d(j['smooth'], d.smooth),
      offsets: {for (final e in ((j['offsets'] as Map?) ?? const {}).entries) '${e.key}'.toUpperCase(): _d(e.value, 0)},
      devices: {
        for (final e in ((j['devices'] as Map?) ?? const {}).entries)
          if (e.value is Map) '${e.key}'.toUpperCase(): TriCal.fromJson((e.value as Map).cast<String, dynamic>()),
      },
    );
  }

  /// The dashboard's clamps (ouispymesh.py clean_tri), so the phone never keeps a model
  /// the dashboard would refuse.
  TriModel clamped() => copyWith(
    p0Ble: p0Ble.clamp(-90, -20).toDouble(),
    p0Wifi: p0Wifi.clamp(-90, -10).toDouble(),
    n: n.clamp(1.6, 5.0).toDouble(),
    smooth: smooth.clamp(0.05, 1.0).toDouble(),
    sigma: sigma.clamp(1.5, 12.0).toDouble(),
    maxErr: maxErr.clamp(2.0, 30.0).toDouble(),
    offsets: {for (final e in offsets.entries) e.key: e.value.clamp(-40.0, 40.0).toDouble()},
  );

  /// The device's model: its calibration, else the generic one for its radio.
  ({double p0, double n, bool cal, double p0Sd}) forDevice(String mac, String dom) {
    final c = devices[mac.toUpperCase()];
    if (c != null) return (p0: c.p0, n: c.n, cal: true, p0Sd: math.max(1.0, c.rms == 0 ? 1.5 : c.rms));
    return (p0: dom == 'wifi' ? p0Wifi : p0Ble, n: n, cal: false, p0Sd: 10.0);
  }
}

// ---- the phone's own model (USB): read by every Tri built without one

const _prefKey = 'tri_model';
final triModelNotifier = ValueNotifier<TriModel>(const TriModel());
bool _triLoaded = false;

/// The phone's triangulation model (what LocalMesh's Tri uses).
TriModel get triModel => triModelNotifier.value;

/// Sets the phone's model and keeps it in shared_preferences.
set triModel(TriModel v) {
  triModelNotifier.value = v;
  _triLoaded = true;
  () async {
    try {
      final p = await SharedPreferences.getInstance();
      await p.setString(_prefKey, jsonEncode(v.toJson()));
    } catch (_) {}
  }();
}

/// Reads the phone's model once (Tri does it on its own; call it early to have it sooner).
Future<TriModel> loadTriModel() async {
  if (_triLoaded) return triModel;
  _triLoaded = true;
  try {
    final p = await SharedPreferences.getInstance();
    final s = p.getString(_prefKey);
    if (s != null && s.isNotEmpty) triModelNotifier.value = TriModel.fromJson((jsonDecode(s) as Map).cast<String, dynamic>());
  } catch (_) {}
  return triModel;
}

/// "Calibrate at a spot" (the web dashboard's calibFinish): with each node's mean RSSI at
/// its true distance, RSSI = P0 - 10 n log10(d) is linear in (P0, n). Both are fitted when
/// the distances differ enough (ratio >= 1.5) and the slope is physical (1.6..5); else n
/// stays [nGeneric] and only P0 is fitted. Null with no points.
({TriCal cal, bool fitted})? fitCalibration(List<({double d, double rssi})> pts, double nGeneric, {double? ts}) {
  if (pts.isEmpty) return null;
  final xs = [for (final p in pts) -10 * math.log(math.max(0.5, p.d)) / math.ln10];
  final ys = [for (final p in pts) p.rssi];
  var n = nGeneric;
  double? p0;
  var fitted = false;
  final ds = [for (final p in pts) math.max(0.5, p.d)];
  if (pts.length >= 2 && ds.reduce(math.max) / ds.reduce(math.min) >= 1.5) {
    final mx = xs.reduce((a, b) => a + b) / xs.length, my = ys.reduce((a, b) => a + b) / ys.length;
    var sxx = 0.0, sxy = 0.0;
    for (var i = 0; i < xs.length; i++) {
      sxx += (xs[i] - mx) * (xs[i] - mx);
      sxy += (xs[i] - mx) * (ys[i] - my);
    }
    final slope = sxx == 0 ? 0.0 : sxy / sxx;
    if (slope >= 1.6 && slope <= 5) {
      n = slope;
      p0 = my - n * mx;
      fitted = true;
    }
  }
  if (!fitted) {
    var acc = 0.0;
    for (var i = 0; i < xs.length; i++) {
      acc += ys[i] - n * xs[i];
    }
    p0 = acc / xs.length;
  }
  var se = 0.0;
  for (var i = 0; i < xs.length; i++) {
    final r = ys[i] - (p0! + n * xs[i]);
    se += r * r;
  }
  final rms = math.sqrt(se / xs.length);
  return (
    cal: TriCal(
      p0: (p0! * 10).round() / 10,
      n: (n * 100).round() / 100,
      rms: (rms * 10).round() / 10,
      nodes: pts.length,
      ts: ts ?? DateTime.now().millisecondsSinceEpoch / 1000,
    ),
    fitted: fitted,
  );
}

class Tri {
  Tri(this.nodePos, {TriModel? model}) : _model = model {
    loadTriModel();
  }
  final (double, double)? Function(String node) nodePos;
  TriModel? _model;

  /// Its own model, else the phone's ([triModel]).
  TriModel get model => _model ?? triModel;
  set model(TriModel? v) => _model = v;
  final _samples = <String, Map<String, List<(double, double)>>>{}; // mac -> node -> [(ts, rssi)]
  final _dom = <String, String>{};
  final _state = <String, List<double>>{};
  final _last = <String, double>{};

  void add(String mac, String node, int? rssi, String dom, double ts) {
    if (rssi == null || node.isEmpty) return;
    final xs = _samples.putIfAbsent(mac, () => {}).putIfAbsent(node, () => []);
    xs.add((ts, rssi.toDouble()));
    while (xs.isNotEmpty && ts - xs.first.$1 > triWindowS) {
      xs.removeAt(0);
    }
    if (dom.isNotEmpty) _dom[mac] = dom;
  }

  bool due(String mac, double now, [double every = 1]) {
    if (now - (_last[mac] ?? 0) < every) return false;
    _last[mac] = now;
    return true;
  }

  /// Drops a device's smoothed position (its model changed: start the fix afresh).
  void reset(String mac) => _state.remove(mac.toUpperCase());

  void forget(double now) => _samples.removeWhere((_, per) {
    per.removeWhere((_, xs) => xs.isEmpty || now - xs.last.$1 > triWindowS);
    return per.isEmpty;
  });

  /// {mac, ok, lat, lon, err, rough, n, nodes: [{id, d}], why}
  Map<String, dynamic> fix(String mac, double now) {
    final m = model;
    final dom = _dom[mac] ?? 'ble';
    final dm = m.forDevice(mac, dom);
    final p0Prior = dm.p0;
    final nodes = <({String id, double lat, double lon, double rssi, int k, double d})>[];
    for (final e in (_samples[mac] ?? const <String, List<(double, double)>>{}).entries) {
      final pos = nodePos(e.key);
      final xs = [
        for (final s in e.value)
          if (now - s.$1 <= triWindowS) s.$2,
      ];
      if (pos == null || xs.isEmpty) continue;
      final mean = xs.reduce((a, b) => a + b) / xs.length - (m.offsets[e.key] ?? 0);
      nodes.add((id: e.key, lat: pos.$1, lon: pos.$2, rssi: mean, k: math.min(_kMax, xs.length), d: math.pow(10, (p0Prior - mean) / (10 * dm.n)).toDouble()));
    }
    final base = <String, dynamic>{
      'mac': mac,
      'ok': false,
      'cal': dm.cal,
      'ts': now,
      'n': nodes.length,
      'nodes': [
        for (final n in nodes) {'id': n.id, 'd': (n.d * 10).round() / 10},
      ],
    };
    if (nodes.length < 3) return {...base, 'why': 'heard by ${nodes.length} of the 3 nodes needed'};
    final lat0 = nodes.first.lat, lon0 = nodes.first.lon;
    final kx = 111320 * math.cos(lat0 * math.pi / 180), ky = 110540.0;
    final px = [for (final n in nodes) (n.lon - lon0) * kx], py = [for (final n in nodes) (n.lat - lat0) * ky];
    final sig2 = m.sigma * m.sigma;
    final w = [for (final n in nodes) n.k / sig2];
    final wp = 1 / (dm.p0Sd * dm.p0Sd);
    const pad = 60.0;
    final x0 = px.reduce(math.min) - pad, x1 = px.reduce(math.max) + pad, y0 = py.reduce(math.min) - pad, y1 = py.reduce(math.max) + pad;
    final cell = math.max(1.0, math.max(x1 - x0, y1 - y0) / 140);
    final gw = ((x1 - x0) / cell).ceil(), gh = ((y1 - y0) / cell).ceil();
    final sumW = w.reduce((a, b) => a + b) + wp;
    final tenN = 10 * dm.n;
    final chi = List<double>.filled(gw * gh, 0);
    var chiMin = double.infinity;
    var best = 0;
    final k = nodes.length;
    final av = List<double>.filled(k, 0);
    for (var j = 0; j < gh; j++) {
      final y = y0 + (j + 0.5) * cell;
      for (var i = 0; i < gw; i++) {
        final x = x0 + (i + 0.5) * cell;
        var acc = wp * p0Prior;
        for (var q = 0; q < k; q++) {
          final r = math.max(0.5, math.sqrt((x - px[q]) * (x - px[q]) + (y - py[q]) * (y - py[q])));
          av[q] = nodes[q].rssi + tenN * math.log(r) / math.ln10;
          acc += w[q] * av[q];
        }
        final p0 = acc / sumW;
        var c = wp * (p0 - p0Prior) * (p0 - p0Prior);
        for (var q = 0; q < k; q++) {
          c += w[q] * (av[q] - p0) * (av[q] - p0);
        }
        final o = j * gw + i;
        chi[o] = c;
        if (c < chiMin) {
          chiMin = c;
          best = o;
        }
      }
    }
    final dof = k + 1 - 3;
    final s2 = dof > 0 ? math.max(1.0, chiMin / dof) : 1.0;
    final pw = [for (final c in chi) math.exp(-0.5 * (c - chiMin) / s2)];
    final tw = pw.reduce((a, b) => a + b);
    final bi = best % gw, bj = best ~/ gw;
    var lw = 0.0, mx = 0.0, my = 0.0;
    for (var j = math.max(0, bj - 2); j <= math.min(gh - 1, bj + 2); j++) {
      for (var i = math.max(0, bi - 2); i <= math.min(gw - 1, bi + 2); i++) {
        final v = pw[j * gw + i];
        lw += v;
        mx += v * (x0 + (i + 0.5) * cell);
        my += v * (y0 + (j + 0.5) * cell);
      }
    }
    final x = mx / lw, y = my / lw;
    final dist = <(double, double)>[];
    for (var o = 0; o < pw.length; o++) {
      final v = pw[o];
      if (v < 1e-9 * tw) continue;
      final dx = x0 + ((o % gw) + 0.5) * cell - x, dy = y0 + ((o ~/ gw) + 0.5) * cell - y;
      dist.add((math.sqrt(dx * dx + dy * dy), v));
    }
    dist.sort((a, b) => a.$1.compareTo(b.$1));
    var acc = 0.0, r68 = dist.isEmpty ? 0.0 : dist.last.$1;
    for (final (d, v) in dist) {
      acc += v;
      if (acc >= 0.68 * tw) {
        r68 = d;
        break;
      }
    }
    final err = math.max(r68, cell * 0.6);
    final raw = [lat0 + y / ky, lon0 + x / kx];
    var st = _state[mac] ?? raw;
    st = [st[0] + m.smooth * (raw[0] - st[0]), st[1] + m.smooth * (raw[1] - st[1])];
    _state[mac] = st;
    return {...base, 'ok': true, 'lat': st[0], 'lon': st[1], 'err': (err * 10).round() / 10, 'rough': err > m.maxErr};
  }
}

/// A device's triangulated fixes as they come in: a small in-memory trail per device (the
/// device page draws it). [attach] once to the model; it records every new fix.
class TriTrails {
  TriTrails._();
  static final I = TriTrails._();
  static const keep = 120, keepDevices = 300;
  final _trails = <String, List<({double ts, double lat, double lon, double err})>>{};
  Model? _m;

  void attach(Model m) {
    if (identical(_m, m)) return;
    _m?.removeListener(_take);
    _m = m;
    m.addListener(_take);
    _take();
  }

  List<({double ts, double lat, double lon, double err})> of(String mac) => _trails[mac.toUpperCase()] ?? const [];

  void clear(String mac) => _trails.remove(mac.toUpperCase());

  /// Records one fix (also what [attach] does with the model's).
  void add(String mac, double ts, double lat, double lon, double err) {
    final l = _trails.putIfAbsent(mac.toUpperCase(), () => []);
    if (l.isNotEmpty && ts <= l.last.ts) return;
    // the same spot again is not a step
    if (l.isNotEmpty && distM(l.last.lat, l.last.lon, lat, lon) < 0.3) return;
    l.add((ts: ts, lat: lat, lon: lon, err: err));
    if (l.length > keep) l.removeRange(0, l.length - keep);
    if (_trails.length > keepDevices) {
      final oldest = _trails.entries.reduce((a, b) => (a.value.isEmpty ? 0 : a.value.last.ts) < (b.value.isEmpty ? 0 : b.value.last.ts) ? a : b).key;
      _trails.remove(oldest);
    }
  }

  void _take() {
    final m = _m;
    if (m == null) return;
    for (final t in m.targets.values) {
      if (!t.triOk || t.triTs <= 0) continue;
      add(t.mac, t.triTs, t.triLat, t.triLon, t.triErr);
    }
  }
}
