// MAP: nodes, mesh links, triangulated targets, drones with their tracks and pilots, and
// the dashboard's marks, over the dashboard's tiles (cached on the phone for offline use).
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../data/model.dart';
import '../data/tiles.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'drones_view.dart';
import 'map_edit.dart';

/// The base layer, and its labels on top when it has them (tiles.dart mapLayers).
TileLayer _tiles(String id, {bool over = false}) {
  final a = App.I;
  final l = layerById(id);
  return TileLayer(
    tileProvider: DashboardTileProvider(base: a.dash.base, authHeaders: a.dash.authHeaders, layer: id, online: a.s.tilesOnline),
    maxNativeZoom: l.maxZoom,
    maxZoom: 22,
    keepBuffer: over ? 1 : 3,
    errorImage: null,
    tileBuilder: (context, w, tile) => w,
  );
}

Widget dashTiles() {
  final l = layerById(App.I.s.tileLayer);
  if (l.labels == null) return _tiles(l.id);
  return Stack(children: [_tiles(l.id), _tiles(l.labels!, over: true)]);
}

LatLng? fleetCenter() {
  final pts = [
    for (final n in App.I.m.nodes.values)
      if (n.hasPos) LatLng(n.lat!, n.lon!),
  ];
  if (pts.isEmpty) return null;
  return LatLng(pts.map((p) => p.latitude).reduce((a, b) => a + b) / pts.length, pts.map((p) => p.longitude).reduce((a, b) => a + b) / pts.length);
}

MarkerLayer nodeMarkers({void Function(FleetNode)? onTap}) {
  final m = App.I.m;
  return MarkerLayer(
    markers: [
      for (final (i, n) in m.nodes.values.indexed)
        if (n.hasPos)
          Marker(
            point: LatLng(n.lat!, n.lon!),
            width: 110,
            height: 46,
            child: GestureDetector(
              onTap: onTap == null ? null : () => onTap(n),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Container(
                    width: 16,
                    height: 16,
                    decoration: BoxDecoration(
                      color: C.bg,
                      shape: n.isHome ? BoxShape.rectangle : BoxShape.circle,
                      border: Border.all(color: n.online ? C.hue(n.hue, i) : (n.silent ? C.amber : C.red), width: 3),
                      boxShadow: [BoxShadow(color: (n.online ? C.hue(n.hue, i) : (n.silent ? C.amber : C.red)).withValues(alpha: 0.5), blurRadius: 6)],
                    ),
                  ),
                  const SizedBox(height: 2),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
                    decoration: BoxDecoration(color: C.bg.withValues(alpha: 0.75), borderRadius: BorderRadius.circular(5)),
                    child: Text(
                      n.label,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(fontFamily: sans, color: n.online ? C.text : (n.silent ? C.amber : C.red), fontSize: 10.5, fontWeight: FontWeight.w600),
                    ),
                  ),
                ],
              ),
            ),
          ),
    ],
  );
}

/// A small map for detail pages.
class MiniMap extends StatelessWidget {
  const MiniMap({super.key, required this.center, this.zoom = 16, required this.layers});
  final LatLng center;
  final double zoom;
  final List<Widget> layers;
  @override
  Widget build(BuildContext context) => FlutterMap(
    options: MapOptions(initialCenter: center, initialZoom: zoom, backgroundColor: const Color(0xFF0A0D12), maxZoom: 21),
    children: [dashTiles(), ...layers],
  );
}

List<Polyline> droneLines(Drone d, {bool hot = true}) => [
  if (d.ptrack.length > 1)
    Polyline(
      points: [for (final p in d.ptrack) LatLng(p[0], p[1])],
      color: C.orange.withValues(alpha: 0.85),
      strokeWidth: 2.5,
      pattern: StrokePattern.dashed(segments: const [6, 5]),
    ),
  if (d.track.length > 1)
    Polyline(points: [for (final p in d.track) LatLng(p[0], p[1])], color: (hot ? C.hot : C.muted), strokeWidth: 3, borderColor: C.bg, borderStrokeWidth: 1),
];

List<Marker> droneMarkers(Drone d, {VoidCallback? onTap}) {
  final live = App.I.m.now - d.last < droneLiveS;
  final c = live ? C.hot : C.soft;
  return [
    if (d.hasPilot) Marker(point: LatLng(d.plat!, d.plon!), width: 26, height: 26, child: const PilotPin()),
    if (d.hasPos)
      Marker(
        point: LatLng(d.lat!, d.lon!),
        width: 120,
        height: 50,
        child: GestureDetector(
          onTap: onTap,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Transform.rotate(
                angle: ((d.hdg ?? 0) - 45) * math.pi / 180,
                child: Icon(
                  PhF.navigationArrow,
                  color: c,
                  size: 22,
                  shadows: [Shadow(color: C.bg.withValues(alpha: 0.9), blurRadius: 4)],
                ),
              ),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
                decoration: BoxDecoration(color: C.bg.withValues(alpha: 0.75), borderRadius: BorderRadius.circular(5)),
                child: Text(
                  d.label,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(fontFamily: sans, color: c, fontSize: 10.5, fontWeight: FontWeight.w600),
                ),
              ),
            ],
          ),
        ),
      ),
  ];
}

class MapView extends StatefulWidget {
  const MapView({super.key});
  @override
  State<MapView> createState() => _MapViewState();
}

class _MapViewState extends State<MapView> {
  final ctl = MapController();
  bool showNodes = true, showLinks = true, showTargets = true, showDrones = true, showMarks = true;
  String centeredFor = ''; // the dashboard the view was last fitted to
  Map<String, String> layers = {};
  String? prefetch;
  String? moving; // a node id: the next tap places it
  List<LatLng>? area; // corners of the area being drawn
  bool placingPin = false;
  bool _cancel = false;

  @override
  void initState() {
    super.initState();
    App.I.dash.tileLayers().then((l) {
      TileCache.I.dashLayers = l.keys.toSet();
      if (mounted && l.isNotEmpty) setState(() => layers = l);
    });
  }

  void _fit() {
    final m = App.I.m;
    final pts = <LatLng>[
      for (final n in m.nodes.values)
        if (n.hasPos) LatLng(n.lat!, n.lon!),
      for (final t in m.targets.values)
        if (t.triOk) LatLng(t.triLat, t.triLon),
    ];
    if (pts.isEmpty) return;
    if (pts.length == 1) {
      ctl.move(pts.first, 17);
    } else {
      ctl.fitCamera(CameraFit.coordinates(coordinates: pts, padding: const EdgeInsets.all(60), maxZoom: 18));
    }
  }

  Future<void> _download() async {
    final b = ctl.camera.visibleBounds;
    final z = ctl.camera.zoom.floor();
    final layer = layerById(App.I.s.tileLayer);
    if (!layer.bulk) {
      App.I.m.toast("${layer.name} doesn't allow saving areas offline: pick another layer");
      return;
    }
    final zMax = math.min(z + 3, layer.maxZoom);
    final ok = await confirm(
      context,
      'Save this area offline?',
      'Stores the ${layer.name} tiles of this view, zoom $z to $zMax (up to 4000 tiles), on the phone.',
      yes: 'Save',
      color: C.accent,
    );
    if (!ok) return;
    _cancel = false;
    setState(() => prefetch = '0%');
    final got = await TileCache.I.prefetch(
      App.I.dash.base,
      App.I.dash.authHeaders,
      App.I.s.tileLayer,
      LatLngBoundsLike(b.north, b.south, b.east, b.west),
      z,
      zMax,
      progress: (d, t) {
        if (mounted) setState(() => prefetch = '${(100 * d / t).round()}%');
      },
      cancelled: () => _cancel,
    );
    if (!mounted) return;
    setState(() => prefetch = null);
    App.I.m.toast('$got tiles saved for offline');
  }

  void _layerPick() async {
    final pick = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const SectionHead('Map layer'),
            for (final l in mapLayers)
              ListTile(
                dense: true,
                leading: Icon(l.id == App.I.s.tileLayer ? PhF.checkCircle : Ph.stack, color: l.id == App.I.s.tileLayer ? C.accent : C.silk),
                title: Text(l.name),
                subtitle: Text(l.credit, style: T.meta.copyWith(fontSize: 11.5)),
                trailing: layers.containsKey(l.id) ? const Tag('Dashboard', color: C.accent) : null,
                onTap: () => Navigator.pop(c, l.id),
              ),
            SwitchListTile(
              title: const Text('Fetch new tiles'),
              subtitle: const Text('Off: only tiles already saved on the phone'),
              value: App.I.s.tilesOnline,
              onChanged: (v) {
                App.I.s.tilesOnline = v;
                App.I.s.save();
                Navigator.pop(c);
                setState(() {});
              },
            ),
          ],
        ),
      ),
    );
    if (pick == null) return;
    App.I.s.tileLayer = pick;
    await App.I.s.save();
    setState(() {});
  }

  Future<void> _dropPin(LatLng p) => editMark(context, {'kind': 'pin', 'lat': p.latitude, 'lon': p.longitude, 'hue': 190, 'icon': 'pin'}, isNew: true);

  Future<void> _nodeSheet(FleetNode n) async {
    final a = await nodeCard(context, n);
    if (!mounted) return;
    if (a == 'move') {
      setState(() => moving = n.id);
      return;
    }
    await nodeAction(context, n, a);
  }

  /// A tap on the map: places a moving node, adds an area corner, drops a pin, or opens
  /// the area it landed in.
  Future<void> _tap(LatLng p) async {
    if (moving != null) {
      final id = moving!;
      setState(() => moving = null);
      await App.I.link.nodeSet(id, {'lat': p.latitude, 'lon': p.longitude});
      return;
    }
    if (area != null) {
      if (area!.length < 24) setState(() => area!.add(p));
      return;
    }
    if (placingPin) {
      setState(() => placingPin = false);
      await _dropPin(p);
      return;
    }
    for (final mk in App.I.m.marks.reversed) {
      if (mk.kind == 'area' && mk.pts.length >= 3 && _inside(p, mk.pts)) {
        await editMark(context, Map.of(mk.raw));
        return;
      }
    }
  }

  static bool _inside(LatLng p, List<List<double>> poly) {
    var c = false;
    for (var i = 0, j = poly.length - 1; i < poly.length; j = i++) {
      final yi = poly[i][0], xi = poly[i][1], yj = poly[j][0], xj = poly[j][1];
      if ((yi > p.latitude) != (yj > p.latitude) && p.longitude < (xj - xi) * (p.latitude - yi) / (yj - yi) + xi) c = !c;
    }
    return c;
  }

  Future<void> _finishArea() async {
    final pts = area!;
    setState(() => area = null);
    if (pts.length < 3) return;
    await editMark(context, {
      'kind': 'area',
      'pts': [
        for (final q in pts) [q.latitude, q.longitude],
      ],
      'hue': 30,
    }, isNew: true);
  }

  /// What the next tap does, with its way out.
  Widget? _modeBar() {
    String? text;
    final acts = <Widget>[];
    if (moving != null) {
      text = 'Tap where ${App.I.m.nodeName(moving!)} goes';
    } else if (placingPin) {
      text = 'Tap where the pin goes';
    } else if (area != null) {
      text = area!.isEmpty ? 'Tap the corners of the area' : '${area!.length} corner${area!.length == 1 ? '' : 's'}${area!.length < 3 ? ', at least 3' : ''}';
      acts.addAll([
        if (area!.isNotEmpty) Btn('', icon: Ph.arrowCounterClockwise, kind: BtnKind.ghost, color: C.text, onTap: () => setState(() => area!.removeLast())),
        const SizedBox(width: 6),
        Btn('Done', icon: Ph.check, kind: BtnKind.filled, onTap: area!.length >= 3 ? _finishArea : null),
      ]);
    }
    if (text == null) return null;
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 6, 6, 6),
      decoration: BoxDecoration(
        color: C.bg.withValues(alpha: 0.92),
        borderRadius: BorderRadius.circular(3),
        border: Border.all(color: C.accent.withValues(alpha: 0.6)),
      ),
      child: Row(
        children: [
          Expanded(child: Text(text, style: T.row.copyWith(fontSize: 14))),
          ...acts,
          const SizedBox(width: 6),
          Btn(
            '',
            icon: Ph.x,
            kind: BtnKind.ghost,
            color: C.silk,
            onTap: () => setState(() {
              moving = null;
              area = null;
              placingPin = false;
            }),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final center = fleetCenter();
        if (centeredFor != m.target && center != null) {
          centeredFor = m.target;
          WidgetsBinding.instance.addPostFrameCallback((_) => _fit());
        }
        final nodes = {
          for (final n in m.nodes.values)
            if (n.hasPos) n.id: n,
        };
        final links = <Polyline>[];
        if (showLinks) {
          final done = <String>{};
          for (final n in nodes.values) {
            for (final s in n.nbr) {
              final o = nodes[s.node];
              if (o == null) continue;
              final k = ([n.id, o.id]..sort()).join();
              if (!done.add(k)) continue;
              links.add(
                Polyline(
                  points: [LatLng(n.lat!, n.lon!), LatLng(o.lat!, o.lon!)],
                  color: C.rssi(s.rssi).withValues(alpha: 0.55),
                  strokeWidth: 1.6,
                  pattern: const StrokePattern.dotted(spacingFactor: 2.2),
                ),
              );
            }
          }
        }
        final tri = [
          for (final t in m.targets.values)
            if (t.triOk && m.now - t.triTs < 600) t,
        ];
        final drones = showDrones ? m.windowDrones : <Drone>[];
        return Stack(
          children: [
            FlutterMap(
              mapController: ctl,
              options: MapOptions(
                initialCenter: center ?? const LatLng(20, 0),
                initialZoom: center == null ? 2 : 16,
                maxZoom: 22,
                backgroundColor: const Color(0xFF0A0D12),
                onLongPress: (_, p) => _dropPin(p),
                onTap: (_, p) => _tap(p),
              ),
              children: [
                dashTiles(),
                if (showMarks)
                  PolygonLayer(
                    polygons: [
                      for (final mk in m.marks)
                        if (mk.kind == 'area' && mk.pts.length >= 3)
                          Polygon(
                            points: [for (final p in mk.pts) LatLng(p[0], p[1])],
                            color: C.hue(mk.hue).withValues(alpha: 0.14),
                            borderColor: C.hue(mk.hue),
                            borderStrokeWidth: 2,
                            label: mk.name,
                            labelStyle: TextStyle(fontFamily: sans, color: C.hue(mk.hue), fontWeight: FontWeight.w600, fontSize: 11),
                          ),
                    ],
                  ),
                PolylineLayer(polylines: links),
                if (area != null && area!.isNotEmpty) ...[
                  PolylineLayer(
                    polylines: [
                      Polyline(
                        points: [...area!, if (area!.length >= 3) area!.first],
                        color: C.accent,
                        strokeWidth: 2,
                        pattern: StrokePattern.dashed(segments: const [8, 5]),
                      ),
                    ],
                  ),
                  CircleLayer(
                    circles: [for (final q in area!) CircleMarker(point: q, radius: 5, color: C.accent, borderColor: C.bg, borderStrokeWidth: 2)],
                  ),
                ],
                if (showTargets)
                  CircleLayer(
                    circles: [
                      for (final t in tri)
                        CircleMarker(
                          point: LatLng(t.triLat, t.triLon),
                          radius: math.max(3, t.triErr),
                          useRadiusInMeter: true,
                          color: (t.triRough ? C.amber : C.green).withValues(alpha: 0.12),
                          borderColor: (t.triRough ? C.amber : C.green).withValues(alpha: 0.8),
                          borderStrokeWidth: 1.5,
                        ),
                    ],
                  ),
                PolylineLayer(polylines: [for (final d in drones) ...droneLines(d, hot: m.now - d.last < droneLiveS)]),
                if (showNodes) nodeMarkers(onTap: _nodeSheet),
                if (showTargets)
                  MarkerLayer(
                    markers: [
                      for (final t in tri)
                        Marker(
                          point: LatLng(t.triLat, t.triLon),
                          width: 120,
                          height: 36,
                          child: GestureDetector(
                            onTap: () => openDevice(context, t.mac),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Dot(C.dom(t.dom), size: 10, halo: true),
                                Text(
                                  m.labelOf(t.mac, t.descr),
                                  overflow: TextOverflow.ellipsis,
                                  style: TextStyle(fontFamily: sans, color: C.text, fontSize: 10, fontWeight: FontWeight.w600, shadows: glowOf(C.bg, 3)),
                                ),
                              ],
                            ),
                          ),
                        ),
                    ],
                  ),
                if (showMarks)
                  MarkerLayer(
                    markers: [
                      for (final mk in m.marks)
                        if (mk.kind == 'pin' && mk.lat != null)
                          Marker(
                            point: LatLng(mk.lat!, mk.lon!),
                            width: 120,
                            height: 44,
                            child: GestureDetector(
                              onTap: () => editMark(context, Map.of(mk.raw)),
                              child: Column(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  Icon(markIcons[mk.icon] ?? PhF.mapPin, color: C.hue(mk.hue), size: 26, shadows: glowOf(C.bg, 4)),
                                  Text(
                                    mk.name,
                                    overflow: TextOverflow.ellipsis,
                                    style: TextStyle(fontFamily: sans, color: C.text, fontSize: 10, fontWeight: FontWeight.w600, shadows: glowOf(C.bg, 3)),
                                  ),
                                ],
                              ),
                            ),
                          ),
                    ],
                  ),
                MarkerLayer(
                  markers: [
                    for (final d in drones)
                      ...droneMarkers(
                        d,
                        onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => DroneDetail(mac: d.mac))),
                      ),
                  ],
                ),
              ],
            ),
            // the layer toggles: one printed strip that always fits the width
            Positioned(
              top: 8,
              left: 8,
              right: 8,
              child: Container(
                decoration: BoxDecoration(
                  color: C.bg.withValues(alpha: 0.88),
                  borderRadius: BorderRadius.circular(3),
                  border: Border.all(color: C.silkLine),
                ),
                child: IntrinsicHeight(
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      for (final (i, w) in [
                        _chip('Layer', Ph.stack, true, _layerPick, lit: false),
                        _chip('Nodes', Ph.graph, showNodes, () => setState(() => showNodes = !showNodes)),
                        _chip('Links', Ph.linkSimple, showLinks, () => setState(() => showLinks = !showLinks)),
                        _chip('Targets', Ph.crosshair, showTargets, () => setState(() => showTargets = !showTargets), n: tri.length),
                        _chip('Drones', Ph.drone, showDrones, () => setState(() => showDrones = !showDrones), n: m.windowDrones.length),
                        _chip('Marks', Ph.mapPin, showMarks, () => setState(() => showMarks = !showMarks)),
                      ].indexed) ...[if (i > 0) Container(width: 1, color: C.silkLine), Expanded(child: w)],
                    ],
                  ),
                ),
              ),
            ),
            Positioned(
              right: 10,
              bottom: 14,
              child: Column(
                children: [
                  _fab(
                    Ph.mapPin,
                    () => setState(() {
                      placingPin = true;
                      area = null;
                      moving = null;
                    }),
                  ),
                  const SizedBox(height: 10),
                  _fab(
                    Ph.polygon,
                    () => setState(() {
                      area = [];
                      placingPin = false;
                      moving = null;
                    }),
                  ),
                  const SizedBox(height: 10),
                  _fab(Ph.crosshair, _fit),
                  const SizedBox(height: 10),
                  _fab(prefetch == null ? Ph.downloadSimple : Ph.x, prefetch == null ? _download : () => _cancel = true),
                ],
              ),
            ),
            if (_modeBar() != null) Positioned(left: 8, right: 64, bottom: 14, child: _modeBar()!),
            if (prefetch != null)
              Positioned(
                left: 12,
                bottom: 18,
                child: _float(
                  Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2)),
                      const SizedBox(width: 8),
                      Text('Saving tiles  $prefetch', style: T.meta.copyWith(color: C.text)),
                    ],
                  ),
                ),
              ),
            if (!App.I.s.tilesOnline)
              Positioned(
                left: 12,
                bottom: 54,
                child: _float(
                  Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const Icon(Ph.cloudSlash, size: 15, color: C.amber),
                      const SizedBox(width: 6),
                      Text('Saved tiles only', style: T.meta.copyWith(color: C.amber)),
                    ],
                  ),
                ),
              ),
          ],
        );
      },
    );
  }

  Widget _float(Widget child) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
    decoration: BoxDecoration(
      color: C.bg.withValues(alpha: 0.9),
      borderRadius: BorderRadius.circular(3),
      border: Border.all(color: C.silkLine),
    ),
    child: child,
  );

  /// One cell of the toggle strip: icon over a silk label, lit with an accent rule when on.
  Widget _chip(String l, IconData i, bool on, VoidCallback f, {int? n, bool lit = true}) => InkWell(
    onTap: f,
    child: Container(
      padding: const EdgeInsets.fromLTRB(2, 7, 2, 6),
      decoration: BoxDecoration(
        color: on && lit ? C.accent.withValues(alpha: 0.08) : null,
        border: Border(bottom: BorderSide(color: on && lit ? C.accent : Colors.transparent, width: 2)),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(i, size: 17, color: on ? (lit ? C.accent : C.text) : C.silk),
              if (n != null && n > 0) ...[const SizedBox(width: 3), Text('$n', style: T.code.copyWith(fontSize: 10.5, color: on ? C.accent : C.silk))],
            ],
          ),
          const SizedBox(height: 3),
          Text(
            l.toUpperCase(),
            maxLines: 1,
            overflow: TextOverflow.fade,
            softWrap: false,
            style: T.silk.copyWith(fontSize: 9, letterSpacing: 0.8, color: on ? C.text : C.silk),
          ),
        ],
      ),
    ),
  );

  Widget _fab(IconData i, VoidCallback f) => Material(
    color: C.bg.withValues(alpha: 0.9),
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(3),
      side: const BorderSide(color: C.silkLine),
    ),
    clipBehavior: Clip.antiAlias,
    child: InkWell(
      onTap: f,
      child: SizedBox(width: 46, height: 46, child: Icon(i, color: C.text, size: 21)),
    ),
  );
}

/// The pilot's position.
class PilotPin extends StatelessWidget {
  const PilotPin({super.key});
  @override
  Widget build(BuildContext context) => Container(
    width: 24,
    height: 24,
    decoration: BoxDecoration(
      color: C.bg,
      shape: BoxShape.circle,
      border: Border.all(color: C.orange, width: 2),
    ),
    child: const Icon(PhF.user, size: 13, color: C.orange),
  );
}
