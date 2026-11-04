// TRIANGULATION: the model every fix is drawn with (the dashboard's settings.tri): the 1 m
// power for BLE and Wi-Fi, the path-loss exponent, the noise, what counts as rough, the
// smoothing, each node's RSSI offset, and the devices calibrated at a spot. On the
// dashboard it is saved there (POST /api/settings {tri}); on USB it is the phone's own
// (tri.dart triModel), which LocalMesh's Tri reads.
//
// Also here: "calibrate at a spot" for one device (calibrateDevice), as the web dashboard's
// calibFinish: tap where the device is, listen 30 s, fit P0 and n from the nodes' readings.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../data/dashboard.dart';
import '../data/local_mesh.dart';
import '../data/model.dart';
import '../data/tri.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'map_view.dart';

/// Where the model lives right now.
bool get triOnDashboard => App.I.link is Dashboard;

/// The model in force: the dashboard's settings.tri, or the phone's. Null when the
/// dashboard did not answer.
Future<TriModel?> fetchTriModel() async {
  final b = App.I.link;
  if (b is Dashboard) {
    final j = await b.getJson('/api/settings');
    if (j is! Map) return null;
    return TriModel.fromJson((j['tri'] as Map?)?.cast<String, dynamic>() ?? const {});
  }
  return loadTriModel();
}

/// Saves part of the model (any of the settings.tri keys; `devices` and `offsets` whole).
/// [reset] are the devices whose fix should start afresh. True when it took.
Future<bool> saveTriParts(Map<String, dynamic> part, {Iterable<String> reset = const []}) async {
  final b = App.I.link;
  if (b is Dashboard) return await b.post('/api/settings', {'tri': part}) != null;
  final cur = await loadTriModel();
  triModel = TriModel.fromJson({...cur.toJson(), ...part}).clamped();
  if (b is LocalMesh) {
    for (final mac in reset) {
      b.tri.reset(mac);
    }
  }
  return true;
}

class TriView extends StatefulWidget {
  const TriView({super.key});
  @override
  State<TriView> createState() => _TriViewState();
}

class _TriViewState extends State<TriView> {
  TriModel? saved, cur;
  bool loading = true, saving = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() => loading = true);
    final t = await fetchTriModel();
    if (!mounted) return;
    setState(() {
      loading = false;
      saved = t;
      cur = t;
    });
  }

  bool get dirty {
    final a = saved, b = cur;
    if (a == null || b == null) return false;
    return a.p0Ble != b.p0Ble ||
        a.p0Wifi != b.p0Wifi ||
        a.n != b.n ||
        a.sigma != b.sigma ||
        a.maxErr != b.maxErr ||
        a.smooth != b.smooth ||
        a.offsets.length != b.offsets.length ||
        a.offsets.entries.any((e) => b.offsets[e.key] != e.value);
  }

  Future<void> _save() async {
    final c = cur;
    if (c == null) return;
    setState(() => saving = true);
    final j = c.clamped().toJson()..remove('devices');
    final ok = await saveTriParts(j, reset: App.I.m.targets.keys);
    if (!mounted) return;
    setState(() => saving = false);
    App.I.m.toast(ok ? (triOnDashboard ? 'saved on the dashboard' : 'saved on this phone') : "didn't save");
    if (ok) _load();
  }

  Future<void> _forget(String mac) async {
    if (!await confirm(context, 'Forget this calibration?', '${deviceTitle(mac)} goes back to the generic model.', yes: 'Forget')) return;
    final devs = {...?saved?.devices}..remove(mac);
    final ok = await saveTriParts(
      {
        'devices': {for (final e in devs.entries) e.key: e.value.toJson()},
      },
      reset: [mac],
    );
    if (ok) _load();
  }

  @override
  Widget build(BuildContext context) {
    final c = cur;
    return ReconPage(
      title: 'Triangulation',
      actions: [
        IconButton(
          onPressed: _load,
          icon: const Icon(Ph.arrowsClockwise, color: C.silk, size: 20),
        ),
      ],
      body: loading
          ? const Center(child: CircularProgressIndicator())
          : c == null
          ? Empty(
              "Can't read the model",
              body: 'The dashboard did not answer. Pull to try again.',
              icon: Ph.cloudSlash,
              action: Btn('Try again', icon: Ph.arrowsClockwise, onTap: _load),
            )
          : ListView(
              padding: const EdgeInsets.only(bottom: 30),
              children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
                  child: Text(
                    triOnDashboard
                        ? 'The dashboard computes every fix; these are its settings, and every screen on it uses them.'
                        : 'The phone computes every fix from the node on USB; these settings stay on this phone.',
                    style: T.meta.copyWith(fontSize: 13),
                  ),
                ),
                Group(
                  title: 'Model',
                  children: [
                    _Slide('P0 Bluetooth', 'dBm at 1 m', c.p0Ble, -90, -20, 1, (v) => setState(() => cur = c.copyWith(p0Ble: v))),
                    _Slide('P0 Wi-Fi', 'dBm at 1 m', c.p0Wifi, -90, -10, 1, (v) => setState(() => cur = c.copyWith(p0Wifi: v))),
                    _Slide('Path loss n', 'exponent: 2 open air, 3+ indoors', c.n, 1.6, 5, 0.05, (v) => setState(() => cur = c.copyWith(n: v))),
                    _Slide('Noise sigma', 'dB per reading', c.sigma, 1.5, 12, 0.5, (v) => setState(() => cur = c.copyWith(sigma: v))),
                    _Slide('Max error', 'm, worse fixes are rough', c.maxErr, 2, 30, 1, (v) => setState(() => cur = c.copyWith(maxErr: v))),
                    _Slide('Smoothing', 'share of each new fix', c.smooth, 0.05, 1, 0.05, (v) => setState(() => cur = c.copyWith(smooth: v))),
                  ],
                ),
                _offsets(c),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 6, 16, 0),
                  child: Row(
                    children: [
                      Expanded(
                        child: Btn(
                          'Defaults',
                          icon: Ph.arrowCounterClockwise,
                          kind: BtnKind.ghost,
                          expand: true,
                          onTap: () => setState(() => cur = const TriModel().copyWith(offsets: c.offsets, devices: c.devices)),
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Btn('Save', icon: Ph.check, kind: BtnKind.filled, expand: true, onTap: dirty && !saving ? _save : null),
                      ),
                    ],
                  ),
                ),
                _calibrated(saved ?? c),
              ],
            ),
    );
  }

  Widget _offsets(TriModel c) {
    final m = App.I.m;
    final ids = {...m.nodes.keys, ...c.offsets.keys}.toList()..sort((a, b) => m.nodeName(a).toLowerCase().compareTo(m.nodeName(b).toLowerCase()));
    void set(String id, double v) {
      final o = {...c.offsets};
      if (v == 0) {
        o.remove(id);
      } else {
        o[id] = v.clamp(-40.0, 40.0).toDouble();
      }
      setState(() => cur = c.copyWith(offsets: o));
    }

    return Group(
      title: 'Node offsets',
      trailing: Text('DB OFF EACH READING', style: T.silk),
      children: [
        if (ids.isEmpty)
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text('No nodes yet.', style: T.meta),
          ),
        for (final (i, id) in ids.indexed)
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 6, 8, 6),
            child: Row(
              children: [
                Dot(C.hue(m.nodes[id]?.hue, i), size: 8),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(m.nodeName(id), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row.copyWith(fontSize: 14.5)),
                      Text(id, style: T.code.copyWith(fontSize: 11)),
                    ],
                  ),
                ),
                IconButton(
                  onPressed: () => set(id, (c.offsets[id] ?? 0) - 1),
                  icon: const Icon(Ph.minus, size: 16, color: C.silk),
                ),
                SizedBox(
                  width: 52,
                  child: Text(
                    _signed(c.offsets[id] ?? 0),
                    textAlign: TextAlign.center,
                    style: T.code.copyWith(fontSize: 13, color: (c.offsets[id] ?? 0) == 0 ? C.muted : C.accent, fontWeight: FontWeight.w600),
                  ),
                ),
                IconButton(
                  onPressed: () => set(id, (c.offsets[id] ?? 0) + 1),
                  icon: const Icon(Ph.plus, size: 16, color: C.silk),
                ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _calibrated(TriModel c) {
    final l = c.devices.entries.toList()..sort((a, b) => b.value.ts.compareTo(a.value.ts));
    return Group(
      title: 'Calibrated devices',
      trailing: Text('${l.length}', style: T.code),
      children: [
        if (l.isEmpty)
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text('None yet. Open a device, put it somewhere you can point to on the map, and use Calibrate here.', style: T.meta.copyWith(fontSize: 13)),
          ),
        for (final e in l)
          Tile(
            icon: Ph.crosshair,
            color: C.accent,
            title: deviceTitle(e.key),
            subtitle: Text(calText(e.value), maxLines: 2, style: T.meta.copyWith(fontSize: 12)),
            onTap: () => openDevice(context, e.key),
            trailing: IconButton(
              onPressed: () => _forget(e.key),
              icon: const Icon(Ph.trash, size: 18, color: C.red),
            ),
          ),
      ],
    );
  }
}

String _signed(double v) => v == 0 ? '0' : '${v > 0 ? '+' : ''}${v.toStringAsFixed(v == v.roundToDouble() ? 0 : 1)}';

/// "-61.2 dBm at 1 m · n 2.41 · fit ±1.8 dB · 3 nodes · 2h ago"
String calText(TriCal c) =>
    '${c.p0.toStringAsFixed(1)} dBm at 1 m · n ${c.n.toStringAsFixed(2)} · fit ±${c.rms.toStringAsFixed(1)} dB · ${c.nodes} node${c.nodes == 1 ? '' : 's'}${c.ts > 0 ? ' · ${agoText(c.ts)}' : ''}';

class _Slide extends StatelessWidget {
  const _Slide(this.label, this.help, this.v, this.lo, this.hi, this.step, this.onChanged);
  final String label, help;
  final double v, lo, hi, step;
  final ValueChanged<double> onChanged;
  @override
  Widget build(BuildContext context) {
    final x = v.clamp(lo, hi).toDouble();
    final dp = step >= 1 ? 0 : (step >= 0.1 ? 1 : 2);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Text(label.toUpperCase(), style: T.silk),
              const SizedBox(width: 8),
              Expanded(
                child: Text(help, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta.copyWith(fontSize: 11.5)),
              ),
              Text(
                x.toStringAsFixed(dp),
                style: T.code.copyWith(color: C.text, fontSize: 13, fontWeight: FontWeight.w600),
              ),
            ],
          ),
          SliderTheme(
            data: SliderTheme.of(context).copyWith(
              trackHeight: 2,
              activeTrackColor: C.accent,
              inactiveTrackColor: C.silkLine,
              thumbColor: C.accent,
              overlayColor: C.accent.withValues(alpha: 0.1),
              thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 7),
            ),
            child: Slider(value: x, min: lo, max: hi, divisions: ((hi - lo) / step).round(), onChanged: (n) => onChanged(double.parse(n.toStringAsFixed(dp)))),
          ),
        ],
      ),
    );
  }
}

// ------------------------------------------------------------ calibrate at a spot

/// Calibrates one device: pick its spot on a map, listen [calibrateS] seconds, fit and save.
/// Returns the calibration when one was saved.
Future<TriCal?> calibrateDevice(BuildContext context, String mac, {LatLng? near}) => showModalBottomSheet<TriCal>(
  context: context,
  isScrollControlled: true,
  enableDrag: false,
  builder: (c) => _Calibrate(mac: mac.toUpperCase(), near: near),
);

/// Takes a device's calibration away (back to the generic model).
Future<bool> resetCalibration(String mac) async {
  mac = mac.toUpperCase();
  final t = await fetchTriModel();
  if (t == null) return false;
  final devs = {...t.devices}..remove(mac);
  return saveTriParts(
    {
      'devices': {for (final e in devs.entries) e.key: e.value.toJson()},
    },
    reset: [mac],
  );
}

/// The readings a calibration collects: every node's RSSI of one device, from its
/// detections and its live sightings (each node's latest reading, once per moment).
class CalSampler {
  CalSampler(this.m, this.mac, {double? start}) : start = start ?? m.now;
  final Model m;
  final String mac;
  final double start;
  final per = <String, List<double>>{};
  final _keys = <String>{};

  void take() {
    for (final d in m.dets) {
      if (d.ts < start - 2) break; // newest first
      if (d.mac != mac || d.rssi == null || d.node.isEmpty) continue;
      _add(d.node, d.rssi!.toDouble(), d.ts);
    }
    final t = m.targets[mac];
    if (t != null && t.last >= start) {
      for (final s in t.seen) {
        if (s.age > 2 || s.rssi <= -127) continue;
        _add(s.node, s.rssi.toDouble(), t.last - s.age);
      }
    }
  }

  void _add(String node, double rssi, double ts) {
    if (!_keys.add('$node|${ts.round()}|${rssi.round()}')) return;
    per.putIfAbsent(node, () => []).add(rssi);
  }

  int get readings => per.values.fold(0, (a, l) => a + l.length);

  /// Each placed node's mean reading (less its offset) at its true distance from [at].
  List<({double d, double rssi})> points(LatLng at, TriModel tm) => [
    for (final e in per.entries)
      if (m.nodes[e.key]?.hasPos == true)
        (
          d: distM(m.nodes[e.key]!.lat!, m.nodes[e.key]!.lon!, at.latitude, at.longitude),
          rssi: e.value.reduce((a, b) => a + b) / e.value.length - (tm.offsets[e.key] ?? 0),
        ),
  ];
}

enum _Stage { place, listen, saving, done, failed }

class _Calibrate extends StatefulWidget {
  const _Calibrate({required this.mac, this.near});
  final String mac;
  final LatLng? near;
  @override
  State<_Calibrate> createState() => _CalibrateState();
}

class _CalibrateState extends State<_Calibrate> {
  _Stage stage = _Stage.place;
  LatLng? at;
  CalSampler? sampler;
  Timer? tick;
  int left = calibrateS;
  String result = '';
  TriCal? cal;

  @override
  void dispose() {
    tick?.cancel();
    App.I.m.removeListener(_take);
    super.dispose();
  }

  void _take() => sampler?.take();

  void _listen() {
    final m = App.I.m;
    sampler = CalSampler(m, widget.mac)..take();
    m.addListener(_take);
    setState(() {
      stage = _Stage.listen;
      left = calibrateS;
    });
    tick = Timer.periodic(const Duration(seconds: 1), (t) {
      sampler?.take();
      if (!mounted) return t.cancel();
      setState(() => left--);
      if (left <= 0) {
        t.cancel();
        _finish();
      }
    });
  }

  Future<void> _finish() async {
    App.I.m.removeListener(_take);
    setState(() => stage = _Stage.saving);
    final tm = await fetchTriModel();
    if (!mounted) return;
    if (tm == null) return setState(() => _fail("Couldn't read the model from the dashboard."));
    final pts = sampler!.points(at!, tm);
    final f = fitCalibration(pts, tm.n);
    if (f == null) {
      return setState(
        () => _fail(
          sampler!.readings == 0
              ? 'No node reported this device during the $calibrateS s. Make sure it is transmitting and try again.'
              : 'The nodes that heard it have no place on the map. Place them (Fleet) and try again.',
        ),
      );
    }
    final devs = {...tm.devices, widget.mac: f.cal};
    final ok = await saveTriParts(
      {
        'devices': {for (final e in devs.entries) e.key: e.value.toJson()},
      },
      reset: [widget.mac],
    );
    if (!mounted) return;
    if (!ok) return setState(() => _fail("The calibration didn't save."));
    setState(() {
      cal = f.cal;
      stage = _Stage.done;
      result =
          '${f.cal.p0.toStringAsFixed(1)} dBm at 1 m, exponent ${f.cal.n.toStringAsFixed(2)}${f.fitted ? '' : ' (generic: the distances were too similar to fit it)'}, fit error ${f.cal.rms.toStringAsFixed(1)} dB over ${f.cal.nodes} node${f.cal.nodes == 1 ? '' : 's'}.';
    });
  }

  void _fail(String why) {
    stage = _Stage.failed;
    result = why;
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final center = widget.near ?? fleetCenter() ?? const LatLng(0, 0);
    final (String head, String body) = switch (stage) {
      _Stage.place => ('Where is it?', 'Tap the map exactly where the device is right now.'),
      _Stage.listen => ('Listening  ${left}s', 'Keep the device still. ${sampler?.readings ?? 0} readings from ${sampler?.per.length ?? 0} nodes so far.'),
      _Stage.saving => ('Fitting', 'Working out its power at 1 m and path loss.'),
      _Stage.done => ('Calibrated', result),
      _Stage.failed => ('Calibration failed', result),
    };
    final hc = switch (stage) {
      _Stage.done => C.green,
      _Stage.failed => C.red,
      _Stage.listen => C.amber,
      _ => C.text,
    };
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('CALIBRATE  ·  ${deviceTitle(widget.mac).toUpperCase()}', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.label),
            const SizedBox(height: 10),
            Text(head, style: T.h2.copyWith(color: hc)),
            const SizedBox(height: 4),
            Text(body, style: T.meta.copyWith(fontSize: 13)),
            if (stage == _Stage.listen) ...[const SizedBox(height: 10), LinearProgressIndicator(value: 1 - left / calibrateS, minHeight: 2, color: C.amber)],
            const SizedBox(height: 12),
            Container(
              height: 260,
              decoration: BoxDecoration(border: Border.all(color: C.silkLine)),
              child: FlutterMap(
                options: MapOptions(
                  initialCenter: center,
                  initialZoom: 18,
                  maxZoom: 21,
                  backgroundColor: const Color(0xFF0A0D12),
                  onTap: stage == _Stage.place ? (_, p) => setState(() => at = p) : null,
                ),
                children: [
                  dashTiles(),
                  nodeMarkers(),
                  if (at != null)
                    CircleLayer(
                      circles: [
                        for (final n in m.nodes.values)
                          if (n.hasPos && (sampler?.per.containsKey(n.id) ?? false))
                            CircleMarker(
                              point: LatLng(n.lat!, n.lon!),
                              radius: distM(n.lat!, n.lon!, at!.latitude, at!.longitude),
                              useRadiusInMeter: true,
                              color: Colors.transparent,
                              borderColor: C.hue(n.hue).withValues(alpha: 0.5),
                              borderStrokeWidth: 1,
                            ),
                        CircleMarker(point: at!, radius: 8, color: C.amber.withValues(alpha: 0.3), borderColor: C.amber, borderStrokeWidth: 2),
                      ],
                    ),
                ],
              ),
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: Btn(
                    stage == _Stage.done || stage == _Stage.failed ? 'Close' : 'Cancel',
                    kind: BtnKind.ghost,
                    expand: true,
                    onTap: stage == _Stage.saving ? null : () => Navigator.pop(context, cal),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: switch (stage) {
                    _Stage.place => Btn('Listen ${calibrateS}s', icon: Ph.waveform, kind: BtnKind.filled, expand: true, onTap: at == null ? null : _listen),
                    _Stage.failed => Btn(
                      'Try again',
                      icon: Ph.arrowsClockwise,
                      expand: true,
                      onTap: () => setState(() {
                        stage = _Stage.place;
                        sampler = null;
                      }),
                    ),
                    _ => Btn(stage == _Stage.done ? 'Saved' : 'Listening', icon: stage == _Stage.done ? Ph.check : Ph.waveform, expand: true),
                  },
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
