// The map's extra layers and their state, as the Tab5 map has them (tab5-dashboard/src/ui.cpp
// draw_map) and the dashboard's map (home-dashboard/static/js/app.js): the toggles (TRI,
// RANGE, TRACKS, PILOT, WD, ME, LINKS), labels placed so they never overlap, the phone's own
// position and trail, GPS nodes' trails, and the distance a target is from each node.
import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:geolocator/geolocator.dart';
import 'package:latlong2/latlong.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../data/dashboard.dart';
import '../data/model.dart';
import '../data/tri.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

/// Map lines: home to each remote, every link a node reports hearing, or none.
enum LinkMode { home, mesh, off }

/// The map's toggles, kept on the phone between runs.
class MapPrefs {
  bool nodes = true, targets = true, drones = true, marks = true, more = false;
  bool tri = true, tracks = true, pilot = true;
  int wd = 0; // 0 off, 1 this session, 2 everything
  LinkMode links = LinkMode.mesh;

  static const _key = 'map.prefs.v1';

  Map<String, dynamic> toJson() => {
    'nodes': nodes,
    'targets': targets,
    'drones': drones,
    'marks': marks,
    'more': more,
    'tri': tri,
    'tracks': tracks,
    'pilot': pilot,
    'wd': wd,
    'links': links.name,
  };

  void _from(Map<String, dynamic> j) {
    bool b(String k, bool d) => j[k] is bool ? j[k] as bool : d;
    nodes = b('nodes', nodes);
    targets = b('targets', targets);
    drones = b('drones', drones);
    marks = b('marks', marks);
    more = b('more', more);
    tri = b('tri', tri);
    tracks = b('tracks', tracks);
    pilot = b('pilot', pilot);
    wd = j['wd'] is int ? (j['wd'] as int).clamp(0, 2) : wd;
    links = LinkMode.values.firstWhere((l) => l.name == j['links'], orElse: () => links);
  }

  Future<void> load() async {
    try {
      final p = await SharedPreferences.getInstance();
      final s = p.getString(_key);
      if (s != null) _from((jsonDecode(s) as Map).cast<String, dynamic>());
    } catch (_) {}
  }

  Future<void> save() async {
    try {
      final p = await SharedPreferences.getInstance();
      await p.setString(_key, jsonEncode(toJson()));
    } catch (_) {}
  }
}

LinkMode nextLinkMode(LinkMode l) => switch (l) {
  LinkMode.home => LinkMode.mesh,
  LinkMode.mesh => LinkMode.off,
  LinkMode.off => LinkMode.home,
};

// ------------------------------------------------------------------ distances

/// Metres from a node to a device heard at [rssi], the log-distance model tri.dart uses.
double rssiDistance(int rssi, String dom, {TriModel model = const TriModel()}) {
  final p0 = dom == 'wifi' ? model.p0Wifi : model.p0Ble;
  return math.pow(10, (p0 - rssi) / (10 * model.n)).toDouble();
}

/// Each placed node that hears [t] and how far it puts it: {node id: metres}. A fix that
/// carried its own per-node distances (a `tri` reply's nodes [{id, d}]) wins; otherwise
/// they come from the RSSI each node last heard it at.
Map<String, double> rangeFor(Target t, Map<String, FleetNode> placed, {List<Map<String, dynamic>>? fixNodes}) {
  final out = <String, double>{};
  for (final f in fixNodes ?? const <Map<String, dynamic>>[]) {
    final id = '${f['id'] ?? ''}', d = f['d'];
    if (placed.containsKey(id) && d is num && d > 0) out[id] = d.toDouble();
  }
  if (out.isNotEmpty) return out;
  for (final s in t.seen) {
    if (!placed.containsKey(s.node) || s.rssi <= -127) continue;
    out[s.node] = rssiDistance(s.rssi, t.dom);
  }
  return out;
}

/// Where a target is without a fix: the nodes that hear it, weighted by signal power (the
/// Tab5's target_pos).
LatLng? signalCentre(Target t, Map<String, FleetNode> placed) {
  var sw = 0.0, a = 0.0, o = 0.0;
  for (final s in t.seen) {
    final n = placed[s.node];
    if (n == null) continue;
    final w = math.pow(10, s.rssi / 10).toDouble();
    sw += w;
    a += w * n.lat!;
    o += w * n.lon!;
  }
  return sw == 0 ? null : LatLng(a / sw, o / sw);
}

/// A point [m] metres north of [p].
LatLng northOf(LatLng p, double m) => LatLng(p.latitude + m / 110540, p.longitude);

String fmtDist(double m) => m >= 1000 ? '${(m / 1000).toStringAsFixed(m >= 10000 ? 0 : 1)} km' : '${m.round()} m';

// ------------------------------------------------------------------ labels

/// A label on the map: a small pill placed beside its dot (below, above, right or left),
/// or centred on its point (link distances, range circles). [dot] is the dot's box around
/// the point in screen pixels; labels keep off every dot.
class MapLabel {
  const MapLabel({
    required this.point,
    required this.text,
    this.color = C.text,
    this.edge,
    this.dot = const Rect.fromLTRB(-8, -8, 8, 8),
    this.onTap,
    this.centred = false,
    this.span,
    this.minSpan = 0,
  });
  final LatLng point;
  final String text;
  final Color color;
  final Color? edge;
  final Rect dot;
  final VoidCallback? onTap;
  final bool centred;

  /// The far end of a line the label sits on: the label is dropped when the line is
  /// shorter than [minSpan] on screen.
  final LatLng? span;
  final double minSpan;
}

/// Greedy placement in screen space, in list order (first = most important). Each label
/// tries below, above, right and left of its dot (centred ones try the point itself first),
/// and is skipped (null) when every spot collides with a dot, a placed label or the edge.
List<Rect?> placeLabels(List<Offset> anchors, List<Rect> dots, List<Size> sizes, Size view, {List<bool>? centred, List<bool>? skip, double gap = 3}) {
  final placed = <Rect>[
    for (var i = 0; i < anchors.length; i++)
      if (!dots[i].isEmpty) dots[i].shift(anchors[i]).inflate(1),
  ];
  final bounds = Offset.zero & view;
  bool fits(Rect r) => r.left >= bounds.left && r.top >= bounds.top && r.right <= bounds.right && r.bottom <= bounds.bottom && !placed.any((p) => p.overlaps(r));
  final out = List<Rect?>.filled(anchors.length, null);
  for (var i = 0; i < anchors.length; i++) {
    if (skip != null && skip[i]) continue;
    final a = anchors[i], d = dots[i], s = sizes[i];
    if (!bounds.inflate(40).contains(a)) continue;
    final cands = <Rect>[
      if (centred != null && centred[i]) Rect.fromCenter(center: a, width: s.width, height: s.height),
      Rect.fromLTWH(a.dx - s.width / 2, a.dy + d.bottom + gap, s.width, s.height), // below
      Rect.fromLTWH(a.dx - s.width / 2, a.dy + d.top - gap - s.height, s.width, s.height), // above
      Rect.fromLTWH(a.dx + d.right + gap, a.dy - s.height / 2, s.width, s.height), // right
      Rect.fromLTWH(a.dx + d.left - gap - s.width, a.dy - s.height / 2, s.width, s.height), // left
    ];
    for (final r in cands) {
      if (fits(r)) {
        out[i] = r;
        placed.add(r);
        break;
      }
    }
  }
  return out;
}

const _labelStyle = TextStyle(fontFamily: mono, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 0.3, height: 1.2);
const _labelPad = EdgeInsets.symmetric(horizontal: 6, vertical: 2.5);
const _labelMaxW = 150.0;
final _sizes = <String, Size>{};

Size _measure(String s) => _sizes.putIfAbsent(s, () {
  if (_sizes.length > 2000) _sizes.clear();
  final tp = TextPainter(
    text: TextSpan(text: s, style: _labelStyle),
    maxLines: 1,
    ellipsis: '…',
    textDirection: TextDirection.ltr,
  )..layout(maxWidth: _labelMaxW);
  final w = tp.width + _labelPad.horizontal + 2, h = tp.height + _labelPad.vertical + 2;
  tp.dispose();
  return Size(w.ceilToDouble(), h.ceilToDouble());
});

/// The labels of every dot, placed where they fit. Goes last in FlutterMap's children.
class LabelLayer extends StatelessWidget {
  const LabelLayer({super.key, required this.labels});
  final List<MapLabel> labels;
  @override
  Widget build(BuildContext context) {
    final cam = MapCamera.of(context);
    final view = cam.nonRotatedSize;
    final anchors = [for (final l in labels) cam.latLngToScreenOffset(l.point)];
    final skip = [for (final (i, l) in labels.indexed) l.span != null && l.minSpan > 0 && (cam.latLngToScreenOffset(l.span!) - anchors[i]).distance * 2 < l.minSpan];
    final rects = placeLabels(
      anchors,
      [for (final l in labels) l.dot],
      [for (final l in labels) _measure(l.text)],
      view,
      centred: [for (final l in labels) l.centred],
      skip: skip,
    );
    return OverflowBox(
      minWidth: view.width,
      maxWidth: view.width,
      minHeight: view.height,
      maxHeight: view.height,
      child: Stack(
        children: [
          for (final (i, l) in labels.indexed)
            if (rects[i] != null)
              Positioned.fromRect(
                rect: rects[i]!,
                child: GestureDetector(
                  onTap: l.onTap,
                  child: Container(
                    padding: _labelPad,
                    alignment: Alignment.center,
                    decoration: BoxDecoration(
                      color: C.bg.withValues(alpha: 0.86),
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(color: (l.edge ?? l.color).withValues(alpha: 0.7)),
                    ),
                    child: Text(
                      l.text,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: _labelStyle.copyWith(color: l.color),
                    ),
                  ),
                ),
              ),
        ],
      ),
    );
  }
}

// ------------------------------------------------------------------ trails

/// GPS nodes' trails: the dashboard's stored tracks (GET /api/tracks) plus every move the
/// phone saw itself, so a node on USB gets one too.
class NodeTrails {
  final _local = <String, List<List<double>>>{}; // id -> [[ts, lat, lon]]
  final _dash = <String, List<List<double>>>{};
  static const keep = 600;
  double _fetched = 0;
  bool _busy = false;

  /// Notes where each GPS node is now (a node only ever moved by hand gets no trail).
  void record(Iterable<FleetNode> nodes, double now) {
    for (final n in nodes) {
      if (!n.hasPos || n.kind != 'wardriver gps') continue;
      final xs = _local.putIfAbsent(n.id, () => []);
      if (xs.isNotEmpty && distM(xs.last[1], xs.last[2], n.lat!, n.lon!) < 2) continue;
      xs.add([now, n.lat!, n.lon!]);
      if (xs.length > keep) xs.removeAt(0);
    }
  }

  /// {tracks: {node: [[ts, lat, lon, acc]]}}
  void ingest(Map<String, dynamic> j) {
    final tr = j['tracks'];
    if (tr is! Map) return;
    _dash.clear();
    for (final e in tr.entries) {
      final pts = <List<double>>[];
      for (final p in (e.value is List ? e.value as List : const [])) {
        if (p is List && p.length >= 3 && p[1] is num && p[2] is num) pts.add([(p[0] as num? ?? 0).toDouble(), (p[1] as num).toDouble(), (p[2] as num).toDouble()]);
      }
      if (pts.isNotEmpty) _dash['${e.key}'] = pts;
    }
  }

  /// Fetches the dashboard's last hour of tracks, at most every 30 s.
  Future<bool> refresh({bool force = false}) async {
    final b = App.I.link;
    final now = DateTime.now().millisecondsSinceEpoch / 1000;
    if (b is! Dashboard || _busy || (!force && now - _fetched < 30)) return false;
    _busy = true;
    _fetched = now;
    try {
      final j = await b.getJson('/api/tracks?seconds=3600');
      if (j is Map) {
        ingest(j.cast<String, dynamic>());
        return true;
      }
      return false;
    } finally {
      _busy = false;
    }
  }

  void clear() {
    _local.clear();
    _dash.clear();
    _fetched = 0;
  }

  List<LatLng> of(String id) {
    final d = _dash[id] ?? const [], l = _local[id] ?? const [];
    final last = d.isEmpty ? 0.0 : d.last[0];
    return [
      for (final p in d) LatLng(p[1], p[2]),
      for (final p in l)
        if (p[0] > last) LatLng(p[1], p[2]),
    ];
  }

  Iterable<String> get ids => {..._dash.keys, ..._local.keys};
}

// ------------------------------------------------------------------ the phone

/// The phone's own position while ME is on: a stream from the GPS, with a 600-point trail.
class PhoneTrack extends ChangeNotifier {
  Position? pos;
  final trail = <LatLng>[];
  StreamSubscription<Position>? _sub;
  bool get running => _sub != null;
  static const keep = 600;

  /// True when moving fast enough for the heading to mean something.
  bool get moving => pos != null && pos!.speed > 0.8 && pos!.heading >= 0;
  LatLng? get at => pos == null ? null : LatLng(pos!.latitude, pos!.longitude);

  void start() {
    if (_sub != null) return;
    try {
      _sub = Geolocator.getPositionStream(
        locationSettings: const LocationSettings(accuracy: LocationAccuracy.high, distanceFilter: 2),
      ).listen(_add, onError: (_) => stop());
    } catch (_) {
      _sub = null;
    }
  }

  void _add(Position p) {
    pos = p;
    final q = LatLng(p.latitude, p.longitude);
    if (trail.isEmpty || distM(trail.last.latitude, trail.last.longitude, q.latitude, q.longitude) >= 2) {
      trail.add(q);
      if (trail.length > keep) trail.removeAt(0);
    }
    notifyListeners();
  }

  void stop() {
    _sub?.cancel();
    _sub = null;
  }

  @override
  void dispose() {
    stop();
    super.dispose();
  }
}

/// Asks for the phone's location the polite way: says why first, and where to go when it
/// was refused for good. True when the map may use it.
Future<bool> askLocation(BuildContext context) async {
  try {
    if (!await Geolocator.isLocationServiceEnabled()) {
      App.I.m.toast('location is off on the phone: turn it on to see yourself on the map');
      return false;
    }
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.deniedForever) {
      App.I.m.toast("location isn't allowed for this app: allow it in the phone's settings");
      return false;
    }
    if (p == LocationPermission.denied) {
      if (!context.mounted) return false;
      final ok = await confirm(
        context,
        'Show where you are?',
        'The map puts a dot where the phone is, which way it moves and the way it came. The position stays on the phone.',
        yes: 'Allow',
        color: C.me,
      );
      if (!ok) return false;
      p = await Geolocator.requestPermission();
    }
    return p == LocationPermission.whileInUse || p == LocationPermission.always;
  } catch (_) {
    return false;
  }
}

/// The phone on the map: a blue dot with a white ring, and an arrow the way it moves.
class MeDot extends StatelessWidget {
  const MeDot({super.key, required this.heading});
  final double? heading;
  @override
  Widget build(BuildContext context) => Stack(
    alignment: Alignment.center,
    children: [
      if (heading != null)
        Transform.rotate(
          angle: heading! * math.pi / 180,
          child: const Align(
            alignment: Alignment.topCenter,
            child: Icon(PhF.caretUp, size: 16, color: C.me),
          ),
        ),
      Container(
        width: 18,
        height: 18,
        decoration: BoxDecoration(
          color: C.me,
          shape: BoxShape.circle,
          border: Border.all(color: C.text, width: 2),
          boxShadow: [BoxShadow(color: C.me.withValues(alpha: 0.6), blurRadius: 10, spreadRadius: 2)],
        ),
      ),
    ],
  );
}
