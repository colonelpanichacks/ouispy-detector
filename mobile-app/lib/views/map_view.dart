// MAP: nodes, mesh links, triangulated targets, drones with their tracks and pilots, and
// the dashboard's marks, over the dashboard's tiles (cached on the phone for offline use).
import 'dart:async';
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
import 'map_layers.dart';
import 'wardrive_view.dart' show wdColor;
import '../data/rid_lookup.dart';
import '../data/wardrive.dart';

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
  final prefs = MapPrefs();
  final trails = NodeTrails();
  final me = PhoneTrack();
  Map<String, String> layers = {};
  String? prefetch;
  String? moving; // a node id: the next tap places it
  List<LatLng>? area; // corners of the area being drawn
  bool placingPin = false;
  bool _cancel = false;

  // ME: the phone's dot and trail; follow keeps it centred until the map is dragged
  bool meOn = false, meFollow = false;
  // LOCK (a card's): this node, or this drone's pilot, stays centred (a drone: followDrone)
  String? lockNode, lockPilot;
  // RANGE: the device whose distance circles are drawn (picked, or the last one opened here)
  bool rangeOn = false;
  String? rangeMac, lastOpened;
  final hiddenTrails = <String>{}; // GPS nodes whose trail is off (their card's TRACK)
  final hiddenTracks = <String>{}; // drones whose track is off (their card's TRACK)
  (String, Flight)? flight; // a past flight drawn on the map (drone mac, flight)

  bool _ready = false, _visible = true, _userMoved = false;
  String _source = '';
  int _fitCount = 0; // placed nodes when the view was last fitted on its own
  Timer? _poll;

  @override
  void initState() {
    super.initState();
    showDrone.addListener(_show);
    followDrone.addListener(_onFollow);
    me.addListener(_onMe);
    prefs.load().then((_) {
      if (!mounted) return;
      setState(() {});
      if (prefs.tracks) _refreshTrails(force: true);
      if (prefs.wd > 0) Wardrive.I.refreshDashboard(history: prefs.wd == 2);
    });
    App.I.dash.tileLayers().then((l) {
      TileCache.I.dashLayers = l.keys.toSet();
      if (mounted && l.isNotEmpty) setState(() => layers = l);
    });
    _sync();
  }

  @override
  void dispose() {
    showDrone.removeListener(_show);
    followDrone.removeListener(_onFollow);
    me.removeListener(_onMe);
    me.dispose();
    _poll?.cancel();
    super.dispose();
  }

  void _show() {
    final d = App.I.m.drones[showDrone.value ?? ''];
    if (d == null) return;
    final p = d.hasPos ? LatLng(d.lat!, d.lon!) : (d.hasPilot ? LatLng(d.plat!, d.plon!) : null);
    showDrone.value = null;
    if (p != null) WidgetsBinding.instance.addPostFrameCallback((_) => _ready ? ctl.move(p, 17) : null);
  }

  void _onFollow() {
    if (followDrone.value == null || !mounted) return;
    setState(() {
      lockNode = null;
      lockPilot = null;
      meFollow = false;
    });
  }

  void _onMe() {
    if (mounted) setState(() {});
  }

  /// The GPS stream and the trails poll run only while the map tab is on screen.
  void _sync() {
    if (_visible && meOn) {
      me.start();
    } else {
      me.stop();
    }
    if (_visible && _poll == null) {
      _poll = Timer.periodic(const Duration(seconds: 30), (_) {
        if (prefs.tracks) _refreshTrails();
      });
    } else if (!_visible) {
      _poll?.cancel();
      _poll = null;
    }
  }

  Future<void> _refreshTrails({bool force = false}) async {
    if (await trails.refresh(force: force) && mounted) setState(() {});
  }

  void _unlock() {
    lockNode = null;
    lockPilot = null;
    meFollow = false;
    if (followDrone.value != null) followDrone.value = null;
  }

  void _savePrefs(void Function() f) {
    setState(f);
    prefs.save();
  }

  Map<String, FleetNode> get _placed => {
    for (final n in App.I.m.nodes.values)
      if (n.hasPos) n.id: n,
  };

  /// Fits the fleet (and, from the button, the targets with a fix too).
  void _fit({bool targets = true}) {
    if (!_ready) return;
    final m = App.I.m;
    final pts = <LatLng>[
      for (final n in m.nodes.values)
        if (n.hasPos) LatLng(n.lat!, n.lon!),
      if (targets)
        for (final t in m.targets.values)
          if (t.triOk && m.now - t.triTs < 600) LatLng(t.triLat, t.triLon),
    ];
    if (pts.isEmpty) return;
    if (pts.length == 1) {
      ctl.move(pts.first, 17);
    } else {
      ctl.fitCamera(CameraFit.coordinates(coordinates: pts, padding: _fitPad, maxZoom: 18));
    }
  }

  EdgeInsets get _fitPad => EdgeInsets.fromLTRB(40, prefs.more ? 130 : 84, 70, 40);

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

  bool _gpsNode(FleetNode n) => n.kind == 'wardriver gps' || trails.of(n.id).length > 1;

  Future<void> _nodeSheet(FleetNode n) async {
    final a = await nodeCard(context, n, tracking: _gpsNode(n) ? !hiddenTrails.contains(n.id) : null, locked: lockNode == n.id);
    if (!mounted) return;
    switch (a) {
      case 'move':
        setState(() => moving = n.id);
        return;
      case 'track':
        setState(() => hiddenTrails.contains(n.id) ? hiddenTrails.remove(n.id) : hiddenTrails.add(n.id));
        return;
      case 'lock':
        final on = lockNode != n.id;
        setState(() {
          _unlock();
          lockNode = on ? n.id : null;
        });
        return;
    }
    await nodeAction(context, n, a);
  }

  Future<void> _droneSheet(Drone d) async {
    final a = await droneCard(context, d, lockedDrone: followDrone.value == d.mac, lockedPilot: lockPilot == d.mac, tracking: !hiddenTracks.contains(d.mac));
    if (!mounted) return;
    switch (a) {
      case 'lockDrone':
        final on = followDrone.value != d.mac;
        setState(_unlock);
        if (on) followDrone.value = d.mac;
      case 'lockPilot':
        final on = lockPilot != d.mac;
        setState(() {
          _unlock();
          lockPilot = on ? d.mac : null;
        });
      case 'track':
        setState(() => hiddenTracks.contains(d.mac) ? hiddenTracks.remove(d.mac) : hiddenTracks.add(d.mac));
      case 'flights':
        await _flights(d);
      case 'details':
        await Navigator.of(context).push(MaterialPageRoute(builder: (_) => DroneDetail(mac: d.mac)));
    }
  }

  /// A drone's past flights: pick one to draw it on the map (the banner clears it).
  Future<void> _flights(Drone d) async {
    final list = await App.I.link.flights(d.mac);
    if (!mounted) return;
    if (list.isEmpty) {
      App.I.m.toast('no stored flights for ${d.label}');
      return;
    }
    final pick = await showModalBottomSheet<int>(
      context: context,
      isScrollControlled: true,
      builder: (c) => SafeArea(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxHeight: MediaQuery.sizeOf(c).height * 0.7),
          child: ListView(
            shrinkWrap: true,
            children: [
              SectionHead('${d.label} · flights'),
              for (final (i, fl) in list.indexed)
                ListTile(
                  dense: true,
                  leading: Icon(flight?.$2.start == fl.start ? PhF.path : Ph.path, color: flight?.$2.start == fl.start ? C.accent : C.silk),
                  title: Text('${clock(fl.start, date: true)}  ·  ${dur(fl.end - fl.start)}'),
                  subtitle: Text(
                    [
                      if (fl.maxH != null) 'max ${fl.maxH!.round()} m ${fl.agl ? 'AGL' : 'MSL'}',
                      if (fl.dist != null) '${fl.dist} m flown',
                      if (fl.pilotMax != null) 'pilot to ${fl.pilotMax} m',
                      '${fl.reports} reports',
                    ].join(' · '),
                    style: T.meta.copyWith(fontSize: 11.5),
                  ),
                  onTap: () => Navigator.pop(c, i),
                ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
                child: Btn('Open the drone page', icon: Ph.drone, kind: BtnKind.ghost, color: C.text, expand: true, onTap: () => Navigator.pop(c, -1)),
              ),
            ],
          ),
        ),
      ),
    );
    if (pick == null || !mounted) return;
    if (pick < 0) {
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => DroneDetail(mac: d.mac)));
      return;
    }
    final fl = list[pick];
    setState(() {
      _unlock();
      flight = (d.mac, fl);
    });
    final pts = [
      for (final p in [...fl.track, ...fl.ptrack])
        if (p.length >= 2) LatLng(p[0], p[1]),
    ];
    if (pts.length > 1 && _ready) ctl.fitCamera(CameraFit.coordinates(coordinates: pts, padding: _fitPad, maxZoom: 18));
  }

  /// RANGE's device: the last one opened from the map first, then everything heard lately.
  Future<String?> _pickRange() {
    final m = App.I.m;
    final ts = m.liveTargets.where((t) => t.seen.isNotEmpty).take(40).toList();
    final last = lastOpened == null ? null : m.targets[lastOpened!];
    if (last != null) {
      ts
        ..remove(last)
        ..insert(0, last);
    }
    if (ts.isEmpty) {
      m.toast('nothing heard lately to range');
      return Future.value(null);
    }
    return showModalBottomSheet<String>(
      context: context,
      isScrollControlled: true,
      builder: (c) => SafeArea(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxHeight: MediaQuery.sizeOf(c).height * 0.7),
          child: ListView(
            shrinkWrap: true,
            children: [
              const SectionHead('Range circles for'),
              for (final t in ts)
                ListTile(
                  dense: true,
                  leading: Dot(C.dom(t.dom), size: 9),
                  title: Text(m.labelOf(t.mac, t.descr), maxLines: 1, overflow: TextOverflow.ellipsis),
                  subtitle: Text('${t.mac}  ·  ${t.seen.length} node${t.seen.length == 1 ? '' : 's'}', style: T.code.copyWith(fontSize: 11)),
                  trailing: t.mac == lastOpened ? const Tag('Last opened', color: C.accent) : RssiBar(t.best),
                  onTap: () => Navigator.pop(c, t.mac),
                ),
            ],
          ),
        ),
      ),
    );
  }

  Future<void> _rangeTap({bool change = false}) async {
    if (rangeOn && !change) {
      setState(() => rangeOn = false);
      return;
    }
    final mac = await _pickRange();
    if (mac == null || !mounted) return;
    setState(() {
      rangeOn = true;
      rangeMac = mac;
    });
  }

  Future<void> _meTap() async {
    if (meOn && meFollow) {
      setState(() {
        meOn = false;
        meFollow = false;
      });
      _sync();
      return;
    }
    if (meOn) {
      setState(() {
        _unlock();
        meFollow = true;
      });
      return;
    }
    if (!await askLocation(context) || !mounted) return;
    setState(() {
      _unlock();
      meOn = true;
      meFollow = true;
    });
    _sync();
  }

  void _wdTap() {
    _savePrefs(() => prefs.wd = (prefs.wd + 1) % 3);
    if (prefs.wd > 0) Wardrive.I.refreshDashboard(history: prefs.wd == 2);
  }

  void _openTarget(Target t) {
    lastOpened = t.mac;
    if (rangeOn) setState(() => rangeMac = t.mac);
    openDevice(context, t.mac);
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

  /// Where a lock or ME wants the camera, if anywhere.
  LatLng? _lockAt(Model m) {
    final fd = followDrone.value == null ? null : m.drones[followDrone.value!];
    if (fd != null && fd.hasPos) return LatLng(fd.lat!, fd.lon!);
    final pd = lockPilot == null ? null : m.drones[lockPilot!];
    if (pd != null && pd.hasPilot) return LatLng(pd.plat!, pd.plon!);
    final n = lockNode == null ? null : m.nodes[lockNode!];
    if (n != null && n.hasPos) return LatLng(n.lat!, n.lon!);
    if (meFollow) return me.at;
    return null;
  }

  String? _lockText(Model m) {
    if (followDrone.value != null) return 'Locked on ${m.drones[followDrone.value!]?.label ?? 'drone'}';
    if (lockPilot != null) return 'Locked on the pilot of ${m.drones[lockPilot!]?.label ?? 'drone'}';
    if (lockNode != null) return 'Locked on ${m.nodeName(lockNode!)}';
    if (meFollow) return me.at == null ? 'Finding where you are' : 'Following you';
    return null;
  }

  @override
  Widget build(BuildContext context) {
    final vis = Visibility.of(context);
    if (vis != _visible) {
      _visible = vis;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _sync();
      });
    }
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        // a new source: forget its trails and fit the fleet again as its nodes come in
        final src = '${App.I.link.runtimeType}|${m.target}';
        if (src != _source) {
          _source = src;
          _userMoved = false;
          _fitCount = 0;
          trails.clear();
          flight = null;
          rangeMac = null;
          rangeOn = false;
          lockNode = null;
          lockPilot = null;
          if (prefs.tracks) WidgetsBinding.instance.addPostFrameCallback((_) => _refreshTrails(force: true));
        }
        trails.record(m.nodes.values, m.now);
        final placed = _placed;
        if (_ready && !_userMoved && placed.length > _fitCount && _lockAt(m) == null) {
          _fitCount = placed.length;
          WidgetsBinding.instance.addPostFrameCallback((_) => _fit(targets: false));
        }
        final want = _lockAt(m);
        if (want != null && _ready) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (!mounted) return;
            final c = ctl.camera.center;
            if (distM(c.latitude, c.longitude, want.latitude, want.longitude) > 0.3) ctl.move(want, ctl.camera.zoom);
          });
        }
        return ListenableBuilder(listenable: prefs.wd > 0 ? Wardrive.I : const _Never(), builder: (context, _) => _map(m, placed));
      },
    );
  }

  Widget _map(Model m, Map<String, FleetNode> placed) {
    final now = m.now;
    final hueOf = {for (final (i, n) in m.nodes.values.indexed) n.id: C.hue(n.hue, i)};
    final labels = <MapLabel>[];
    final lines = <Polyline>[];
    final circles = <CircleMarker>[];
    final markers = <Marker>[];

    // ---- nodes (home first: its label wins a crowded spot)
    final nodes = placed.values.toList()..sort((a, b) => (b.isHome ? 1 : 0) - (a.isHome ? 1 : 0));
    final nodeMk = <Marker>[];
    if (prefs.nodes) {
      for (final n in nodes) {
        final c = n.online ? hueOf[n.id]! : (n.silent ? C.amber : C.red);
        final p = LatLng(n.lat!, n.lon!);
        nodeMk.add(
          Marker(
            point: p,
            width: 22,
            height: 22,
            child: GestureDetector(
              onTap: () => _nodeSheet(n),
              child: Center(
                child: Container(
                  width: n.isHome ? 17 : 16,
                  height: n.isHome ? 17 : 16,
                  decoration: BoxDecoration(
                    color: C.bg,
                    shape: n.isHome ? BoxShape.rectangle : BoxShape.circle,
                    border: Border.all(color: c, width: 3),
                    boxShadow: [BoxShadow(color: c.withValues(alpha: 0.5), blurRadius: 6)],
                  ),
                ),
              ),
            ),
          ),
        );
        labels.add(
          MapLabel(
            point: p,
            text: n.label,
            color: n.online ? C.text : (n.silent ? C.amber : C.red),
            edge: c,
            dot: const Rect.fromLTRB(-10, -10, 10, 10),
            onTap: () => _nodeSheet(n),
          ),
        );
      }
    }

    // ---- the phone
    final meAt = meOn ? me.at : null;
    if (meOn && prefs.tracks && me.trail.length > 1) lines.add(Polyline(points: List.of(me.trail), color: C.me.withValues(alpha: 0.7), strokeWidth: 3));
    if (meAt != null) {
      markers.add(
        Marker(
          point: meAt,
          width: 44,
          height: 44,
          child: MeDot(heading: me.moving ? me.pos!.heading : null),
        ),
      );
      labels.add(MapLabel(point: meAt, text: 'YOU', color: C.me, dot: const Rect.fromLTRB(-14, -14, 14, 14)));
    }

    // ---- links: home to each remote, or every first-hand mesh link
    if (prefs.links == LinkMode.home) {
      final home = nodes.where((n) => n.isHome).firstOrNull;
      if (home != null) {
        for (final n in nodes) {
          if (n.isHome || !n.online) continue;
          _link(lines, home, n, n.rssi, faded: n.silent, oneWay: false);
        }
      }
    } else if (prefs.links == LinkMode.mesh) {
      final pairs = <String, (FleetNode, FleetNode, Map<String, Seen>)>{};
      for (final n in nodes) {
        for (final s in n.nbr) {
          final o = placed[s.node];
          if (o == null || o.id == n.id) continue;
          final ab = n.id.compareTo(o.id) < 0;
          final k = ab ? '${n.id}|${o.id}' : '${o.id}|${n.id}';
          final e = pairs.putIfAbsent(k, () => ab ? (n, o, <String, Seen>{}) : (o, n, <String, Seen>{}));
          e.$3[n.id] = s;
        }
      }
      for (final (a, b, seen) in pairs.values) {
        final rs = [for (final s in seen.values) s.rssi].where((r) => r > -127).toList();
        final fresh = seen.values.any((s) => s.age < 30) && a.online && b.online;
        _link(lines, a, b, rs.isEmpty ? null : rs.reduce(math.max), faded: !fresh, oneWay: seen.length < 2);
      }
    }

    // ---- GPS nodes' trails
    if (prefs.tracks) {
      for (final id in trails.ids) {
        if (hiddenTrails.contains(id) || !m.nodes.containsKey(id)) continue;
        final pts = trails.of(id);
        if (pts.length > 1) lines.add(Polyline(points: pts, color: (hueOf[id] ?? C.cyan).withValues(alpha: 0.6), strokeWidth: 2.5));
      }
    }

    // ---- targets: at the fix (TRI), else between the nodes that hear them
    var nTargets = 0;
    if (prefs.targets) {
      for (final t in m.targets.values) {
        final tri = prefs.tri && t.triOk && now - t.triTs < 600;
        if (!tri && now - t.last > targetLiveS) continue;
        final p = tri ? LatLng(t.triLat, t.triLon) : signalCentre(t, placed);
        if (p == null) continue;
        nTargets++;
        final c = m.muted.contains(t.mac) ? C.muted : C.dom(t.dom);
        if (tri) {
          for (final s in t.seen) {
            final n = placed[s.node];
            if (n == null) continue;
            lines.add(
              Polyline(
                points: [LatLng(n.lat!, n.lon!), p],
                color: C.rssi(s.rssi).withValues(alpha: 0.55),
                strokeWidth: 1.4,
                pattern: StrokePattern.dashed(segments: const [5, 5]),
              ),
            );
          }
          final rc = t.triRough ? C.amber : C.green;
          circles.add(
            CircleMarker(
              point: p,
              radius: math.max(3, t.triErr),
              useRadiusInMeter: true,
              color: rc.withValues(alpha: 0.1),
              borderColor: rc.withValues(alpha: 0.8),
              borderStrokeWidth: 1.5,
            ),
          );
        }
        markers.add(
          Marker(
            point: p,
            width: 22,
            height: 22,
            child: GestureDetector(
              onTap: () => _openTarget(t),
              child: Center(
                child: tri
                    ? Dot(c, size: 10, halo: true)
                    : Container(
                        width: 9,
                        height: 9,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: C.bg.withValues(alpha: 0.6),
                          border: Border.all(color: c.withValues(alpha: 0.8), width: 1.6),
                        ),
                      ),
              ),
            ),
          ),
        );
      }
    }

    // ---- range: a circle round each node at the distance it puts the device
    final rt = rangeOn ? m.targets[rangeMac ?? ''] : null;
    if (rt != null) {
      for (final e in rangeFor(rt, placed).entries) {
        final n = placed[e.key]!, c = hueOf[n.id] ?? C.cyan;
        final p = LatLng(n.lat!, n.lon!);
        circles.add(
          CircleMarker(
            point: p,
            radius: e.value,
            useRadiusInMeter: true,
            color: c.withValues(alpha: 0.05),
            borderColor: c.withValues(alpha: 0.85),
            borderStrokeWidth: 1.6,
          ),
        );
      }
    }

    // ---- drones, their pilots and tracks
    final drones = prefs.drones ? m.windowDrones : <Drone>[];
    final droneLabels = <MapLabel>[], pilotLabels = <MapLabel>[];
    for (final d in drones) {
      final live = now - d.last < droneLiveS;
      final dp = d.hasPos ? LatLng(d.lat!, d.lon!) : null;
      final pp = d.hasPilot ? LatLng(d.plat!, d.plon!) : null;
      final tracking = prefs.tracks && !hiddenTracks.contains(d.mac);
      if (tracking && d.track.length > 1) {
        lines.add(
          Polyline(points: [for (final p in d.track) LatLng(p[0], p[1])], color: live ? C.hot : C.muted, strokeWidth: 3, borderColor: C.bg, borderStrokeWidth: 1),
        );
      }
      if (prefs.pilot && tracking && d.ptrack.length > 1) {
        lines.add(
          Polyline(
            points: [for (final p in d.ptrack) LatLng(p[0], p[1])],
            color: C.orange.withValues(alpha: 0.85),
            strokeWidth: 2.5,
            pattern: StrokePattern.dashed(segments: const [6, 5]),
          ),
        );
      }
      if (prefs.pilot && dp != null && pp != null) {
        lines.add(Polyline(points: [pp, dp], color: C.orange.withValues(alpha: 0.6), strokeWidth: 1.4, pattern: const StrokePattern.dotted(spacingFactor: 2)));
      }
      if (prefs.pilot && pp != null) {
        markers.add(
          Marker(
            point: pp,
            width: 26,
            height: 26,
            child: GestureDetector(onTap: () => _droneSheet(d), child: const PilotPin()),
          ),
        );
        pilotLabels.add(
          MapLabel(
            point: pp,
            text: d.pilotDist == null ? 'PILOT' : 'PILOT ${fmtDist(d.pilotDist!.toDouble())}',
            color: C.orange,
            dot: const Rect.fromLTRB(-13, -13, 13, 13),
            onTap: () => _droneSheet(d),
          ),
        );
      }
      if (dp != null) {
        final c = live ? C.hot : C.soft;
        markers.add(
          Marker(
            point: dp,
            width: 28,
            height: 28,
            child: GestureDetector(
              onTap: () => _droneSheet(d),
              child: Transform.rotate(
                angle: ((d.hdg ?? 0) - 45) * math.pi / 180,
                child: Icon(
                  PhF.navigationArrow,
                  color: c,
                  size: 22,
                  shadows: [Shadow(color: C.bg.withValues(alpha: 0.9), blurRadius: 4)],
                ),
              ),
            ),
          ),
        );
        droneLabels.add(MapLabel(point: dp, text: d.label, color: c, dot: const Rect.fromLTRB(-12, -12, 12, 12), onTap: () => _droneSheet(d)));
      }
    }

    // ---- a past flight: its track solid, the pilot's dashed, takeoff and landing marked
    final fl = flight?.$2;
    if (fl != null) {
      final tr = [
        for (final p in fl.track)
          if (p.length >= 2) LatLng(p[0], p[1]),
      ];
      final ptr = [
        for (final p in fl.ptrack)
          if (p.length >= 2) LatLng(p[0], p[1]),
      ];
      if (ptr.length > 1) {
        lines.add(
          Polyline(
            points: ptr,
            color: C.orange,
            strokeWidth: 2.5,
            pattern: StrokePattern.dashed(segments: const [6, 5]),
          ),
        );
      }
      if (tr.length > 1) lines.add(Polyline(points: tr, color: C.accent, strokeWidth: 3.5, borderColor: C.bg, borderStrokeWidth: 1));
      if (tr.isNotEmpty) {
        circles.add(CircleMarker(point: tr.first, radius: 6, color: C.green, borderColor: C.bg, borderStrokeWidth: 2));
        labels.add(MapLabel(point: tr.first, text: 'TAKEOFF', color: C.green, dot: const Rect.fromLTRB(-7, -7, 7, 7)));
      }
      if (tr.length > 1) {
        circles.add(CircleMarker(point: tr.last, radius: 6, color: C.red, borderColor: C.bg, borderStrokeWidth: 2));
        labels.add(MapLabel(point: tr.last, text: 'LANDING', color: C.red, dot: const Rect.fromLTRB(-7, -7, 7, 7)));
      }
      if (fl.pilot != null && fl.pilot!.length >= 2) markers.add(Marker(point: LatLng(fl.pilot![0], fl.pilot![1]), width: 26, height: 26, child: const PilotPin()));
    }

    // ---- target labels (after nodes, drones: those win a crowded spot)
    labels.addAll(droneLabels);
    if (prefs.targets) {
      for (final t in m.targets.values) {
        final tri = prefs.tri && t.triOk && now - t.triTs < 600;
        if (!tri && now - t.last > targetLiveS) continue;
        final p = tri ? LatLng(t.triLat, t.triLon) : signalCentre(t, placed);
        if (p == null) continue;
        final name = m.labelOf(t.mac, t.descr.isNotEmpty ? t.descr : t.type);
        labels.add(
          MapLabel(
            point: p,
            text: tri ? '$name ±${t.triErr.round()} m${t.triRough ? ' rough' : ''}' : name,
            color: m.muted.contains(t.mac) ? C.muted : C.text,
            edge: C.dom(t.dom),
            dot: const Rect.fromLTRB(-6, -6, 6, 6),
            onTap: () => _openTarget(t),
          ),
        );
      }
    }
    labels.addAll(pilotLabels);

    // ---- marks
    final pins = <Marker>[];
    if (prefs.marks) {
      for (final mk in m.marks) {
        if (mk.kind == 'pin' && mk.lat != null && mk.lon != null) {
          final p = LatLng(mk.lat!, mk.lon!);
          pins.add(
            Marker(
              point: p,
              width: 28,
              height: 28,
              alignment: Alignment.topCenter,
              child: GestureDetector(
                onTap: () => editMark(context, Map.of(mk.raw)),
                child: Icon(markIcons[mk.icon] ?? PhF.mapPin, color: C.hue(mk.hue), size: 26, shadows: glowOf(C.bg, 4)),
              ),
            ),
          );
          if (mk.name.isNotEmpty) {
            labels.add(
              MapLabel(
                point: p,
                text: mk.name,
                color: C.text,
                edge: C.hue(mk.hue),
                dot: const Rect.fromLTRB(-13, -27, 13, 1),
                onTap: () => editMark(context, Map.of(mk.raw)),
              ),
            );
          }
        } else if (mk.kind == 'area' && mk.pts.length >= 3 && mk.name.isNotEmpty) {
          final la = mk.pts.map((q) => q[0]).reduce((a, b) => a + b) / mk.pts.length, lo = mk.pts.map((q) => q[1]).reduce((a, b) => a + b) / mk.pts.length;
          labels.add(MapLabel(point: LatLng(la, lo), text: mk.name, color: C.hue(mk.hue), dot: Rect.zero, centred: true, onTap: () => editMark(context, Map.of(mk.raw))));
        }
      }
    }

    // ---- range labels, then link distances: the least important, placed last
    if (rt != null) {
      for (final e in rangeFor(rt, placed).entries) {
        final n = placed[e.key]!;
        labels.add(
          MapLabel(point: northOf(LatLng(n.lat!, n.lon!), e.value), text: '${n.label} ${fmtDist(e.value)}', color: hueOf[n.id] ?? C.cyan, dot: Rect.zero, centred: true),
        );
      }
    }
    labels.addAll(_linkLabels);
    _linkLabels.clear();

    // ---- wardriven devices
    final wd = <CircleMarker>[];
    if (prefs.wd > 0) {
      final w = Wardrive.I;
      final sess = prefs.wd == 1 ? (w.currentId ?? w.sessionList.firstOrNull?.id) : null;
      final ds = [
        for (final d in (prefs.wd == 1 && sess == null ? const <WdDevice>[] : w.inScope(session: sess)))
          if (d.placed) d,
      ]..sort((a, b) => b.last.compareTo(a.last));
      for (final d in ds.take(4000)) {
        wd.add(CircleMarker(point: LatLng(d.lat!, d.lon!), radius: 3.5, color: wdColor(d).withValues(alpha: 0.85), borderColor: C.bg, borderStrokeWidth: 1));
      }
    }

    final lockText = _lockText(m);
    final fd = flight == null ? null : m.drones[flight!.$1];
    return Stack(
      children: [
        FlutterMap(
          mapController: ctl,
          options: MapOptions(
            initialCenter: fleetCenter() ?? const LatLng(20, 0),
            initialZoom: placed.isEmpty ? 2 : 16,
            initialCameraFit: placed.length > 1
                ? CameraFit.coordinates(coordinates: [for (final n in placed.values) LatLng(n.lat!, n.lon!)], padding: _fitPad, maxZoom: 18)
                : null,
            maxZoom: 22,
            backgroundColor: const Color(0xFF0A0D12),
            onMapReady: () {
              _ready = true;
              _fitCount = placed.length;
              if (mounted) setState(() {});
            },
            onLongPress: (_, p) => _dropPin(p),
            onTap: (_, p) => _tap(p),
            // dragging the map lets go of any lock and stops fitting on its own
            onPositionChanged: (_, gesture) {
              if (!gesture) return;
              _userMoved = true;
              if (lockNode != null || lockPilot != null || meFollow || followDrone.value != null) setState(_unlock);
            },
          ),
          children: [
            dashTiles(),
            if (prefs.marks)
              PolygonLayer(
                polygons: [
                  for (final mk in m.marks)
                    if (mk.kind == 'area' && mk.pts.length >= 3)
                      Polygon(
                        points: [for (final p in mk.pts) LatLng(p[0], p[1])],
                        color: C.hue(mk.hue).withValues(alpha: 0.14),
                        borderColor: C.hue(mk.hue),
                        borderStrokeWidth: 2,
                      ),
                ],
              ),
            if (wd.isNotEmpty) CircleLayer(circles: wd),
            CircleLayer(circles: circles),
            PolylineLayer(polylines: lines),
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
            MarkerLayer(markers: markers),
            MarkerLayer(markers: nodeMk),
            MarkerLayer(markers: pins),
            LabelLayer(labels: labels),
          ],
        ),
        // the toggles: one printed strip that always fits the width, MORE opens a second
        Positioned(
          top: 8,
          left: 8,
          right: 8,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _strip([
                _chip('Layer', Ph.stack, true, _layerPick, lit: false),
                _chip('Nodes', Ph.graph, prefs.nodes, () => _savePrefs(() => prefs.nodes = !prefs.nodes), n: placed.length),
                _chip('Targets', Ph.crosshair, prefs.targets, () => _savePrefs(() => prefs.targets = !prefs.targets), n: nTargets),
                _chip('Drones', Ph.drone, prefs.drones, () => _savePrefs(() => prefs.drones = !prefs.drones), n: m.windowDrones.length),
                _chip('Marks', Ph.mapPin, prefs.marks, () => _savePrefs(() => prefs.marks = !prefs.marks)),
                _chip('More', prefs.more ? Ph.caretUp : Ph.caretDown, prefs.more, () => _savePrefs(() => prefs.more = !prefs.more), lit: false),
              ]),
              if (prefs.more) ...[
                const SizedBox(height: 4),
                _strip([
                  _chip('Tri', Ph.crosshairSimple, prefs.tri, () => _savePrefs(() => prefs.tri = !prefs.tri)),
                  _chip('Range', Ph.circleDashed, rangeOn, _rangeTap),
                  _chip('Tracks', Ph.path, prefs.tracks, () {
                    _savePrefs(() => prefs.tracks = !prefs.tracks);
                    if (prefs.tracks) _refreshTrails(force: true);
                  }),
                  _chip('Pilot', Ph.user, prefs.pilot, () => _savePrefs(() => prefs.pilot = !prefs.pilot)),
                  _chip(['WD off', 'WD now', 'WD all'][prefs.wd], Ph.wifiHigh, prefs.wd > 0, _wdTap),
                  _chip('Me', meFollow ? PhF.navigationArrow : Ph.navigationArrow, meOn, _meTap),
                  _chip(
                    switch (prefs.links) {
                      LinkMode.home => 'Home',
                      LinkMode.mesh => 'Mesh',
                      LinkMode.off => 'Links',
                    },
                    Ph.linkSimple,
                    prefs.links != LinkMode.off,
                    () => _savePrefs(() => prefs.links = nextLinkMode(prefs.links)),
                  ),
                ]),
              ],
              const SizedBox(height: 6),
              if (lockText != null)
                _banner(
                  meFollow ? Ph.navigationArrow : PhF.lockSimple,
                  followDrone.value != null ? C.hot : (meFollow ? C.me : C.amber),
                  lockText,
                  () => setState(_unlock),
                ),
              if (rt != null) ...[
                const SizedBox(height: 6),
                GestureDetector(
                  onTap: () => _rangeTap(change: true),
                  child: _banner(Ph.circleDashed, C.cyan, 'Range: ${m.labelOf(rt.mac, rt.descr)}  ·  tap to change', () => setState(() => rangeOn = false)),
                ),
              ],
              if (fl != null) ...[
                const SizedBox(height: 6),
                GestureDetector(
                  onTap: () => setState(() => flight = null),
                  child: _banner(PhF.path, C.accent, '${fd?.label ?? 'Flight'} ${clock(fl.start, date: true)}  ·  tap to clear', () => setState(() => flight = null)),
                ),
              ],
            ],
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
              _fab(Ph.crosshair, () {
                setState(_unlock);
                _fit();
              }),
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
  }

  // link distance labels, gathered by _link and placed after everything else
  final _linkLabels = <MapLabel>[];

  /// One link line: solid when both ends hear each other lately, dashed one-way or quiet;
  /// its signal and length at the middle when there is room.
  void _link(List<Polyline> lines, FleetNode a, FleetNode b, int? rssi, {required bool faded, required bool oneWay}) {
    final pa = LatLng(a.lat!, a.lon!), pb = LatLng(b.lat!, b.lon!);
    final c = C.rssi(rssi);
    lines.add(
      Polyline(
        points: [pa, pb],
        color: c.withValues(alpha: faded ? 0.35 : 0.75),
        strokeWidth: 2,
        pattern: oneWay
            ? const StrokePattern.dotted(spacingFactor: 2.2)
            : faded
            ? StrokePattern.dashed(segments: const [6, 6])
            : const StrokePattern.solid(),
      ),
    );
    final mid = LatLng((pa.latitude + pb.latitude) / 2, (pa.longitude + pb.longitude) / 2);
    final d = distM(pa.latitude, pa.longitude, pb.latitude, pb.longitude);
    _linkLabels.add(MapLabel(point: mid, text: '${rssi == null ? '' : '$rssi dBm · '}${fmtDist(d)}', color: c, dot: Rect.zero, centred: true, span: pa, minSpan: 130));
  }

  Widget _strip(List<Widget> cells) => Container(
    decoration: BoxDecoration(
      color: C.bg.withValues(alpha: 0.88),
      borderRadius: BorderRadius.circular(3),
      border: Border.all(color: C.silkLine),
    ),
    child: IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (final (i, w) in cells.indexed) ...[if (i > 0) Container(width: 1, color: C.silkLine), Expanded(child: w)],
        ],
      ),
    ),
  );

  Widget _banner(IconData icon, Color c, String text, VoidCallback clear) => _float(
    Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(icon, size: 15, color: c),
        const SizedBox(width: 6),
        Flexible(
          child: Text(
            text,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: T.meta.copyWith(color: C.text),
          ),
        ),
        const SizedBox(width: 8),
        GestureDetector(
          onTap: clear,
          child: const Icon(Ph.x, size: 16, color: C.silk),
        ),
      ],
    ),
  );

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

/// A listenable that never fires (WD off: the map doesn't rebuild on wardrive lines).
class _Never implements Listenable {
  const _Never();
  @override
  void addListener(VoidCallback listener) {}
  @override
  void removeListener(VoidCallback listener) {}
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
