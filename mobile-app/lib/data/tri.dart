// RSSI triangulation, a straight port of home-dashboard/tri.py (itself the web map's
// triFixCompute), so the phone shows the same fix the dashboard and the Tab5 would:
//
//   RSSI_i = P0 - 10 n log10(r_i) + noise(sigma), P0 integrated out per grid cell,
//   posterior exp(-chi^2/2) over a grid round the nodes, position = the best cell refined
//   by its 5x5 posterior mean, error = radius holding 68% of the posterior, smoothed.
import 'dart:math' as math;

const triWindowS = 45.0;
const _kMax = 5;

class TriModel {
  const TriModel({this.p0Ble = -59, this.p0Wifi = -40, this.n = 2.7, this.sigma = 4, this.maxErr = 8, this.smooth = 0.35});
  final double p0Ble, p0Wifi, n, sigma, maxErr, smooth;
}

class Tri {
  Tri(this.nodePos, {this.model = const TriModel()});
  final (double, double)? Function(String node) nodePos;
  TriModel model;
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

  void forget(double now) => _samples.removeWhere((_, per) {
    per.removeWhere((_, xs) => xs.isEmpty || now - xs.last.$1 > triWindowS);
    return per.isEmpty;
  });

  /// {mac, ok, lat, lon, err, rough, n, nodes: [{id, d}], why}
  Map<String, dynamic> fix(String mac, double now) {
    final m = model;
    final dom = _dom[mac] ?? 'ble';
    final p0Prior = dom == 'wifi' ? m.p0Wifi : m.p0Ble;
    final nodes = <({String id, double lat, double lon, double rssi, int k, double d})>[];
    for (final e in (_samples[mac] ?? const <String, List<(double, double)>>{}).entries) {
      final pos = nodePos(e.key);
      final xs = [
        for (final s in e.value)
          if (now - s.$1 <= triWindowS) s.$2,
      ];
      if (pos == null || xs.isEmpty) continue;
      final mean = xs.reduce((a, b) => a + b) / xs.length;
      nodes.add((id: e.key, lat: pos.$1, lon: pos.$2, rssi: mean, k: math.min(_kMax, xs.length), d: math.pow(10, (p0Prior - mean) / (10 * m.n)).toDouble()));
    }
    final base = <String, dynamic>{
      'mac': mac,
      'ok': false,
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
    const p0Sd = 10.0;
    const wp = 1 / (p0Sd * p0Sd);
    const pad = 60.0;
    final x0 = px.reduce(math.min) - pad, x1 = px.reduce(math.max) + pad, y0 = py.reduce(math.min) - pad, y1 = py.reduce(math.max) + pad;
    final cell = math.max(1.0, math.max(x1 - x0, y1 - y0) / 140);
    final gw = ((x1 - x0) / cell).ceil(), gh = ((y1 - y0) / cell).ceil();
    final sumW = w.reduce((a, b) => a + b) + wp;
    final tenN = 10 * m.n;
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
