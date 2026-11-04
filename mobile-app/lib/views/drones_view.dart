// DRONES: every Remote ID drone heard in the window (1H/4H/8H/24H/ALL or a span), live
// ones on top; a drone's page has its track, pilot, tag and past flights.
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'map_view.dart';

class DronesView extends StatelessWidget {
  const DronesView({super.key});

  Future<void> _set(BuildContext context, DroneWindow w, {int? hours}) async {
    final a = App.I;
    a.m.droneWindow = w;
    if (hours != null) {
      a.s.droneHours = hours;
      a.s.save();
    }
    a.m.changed();
    await a.dash.fetchDroneWindow();
  }

  Future<void> _span(BuildContext context) async {
    final now = DateTime.now();
    final r = await showDateRangePicker(
      context: context,
      firstDate: now.subtract(const Duration(days: 365)),
      lastDate: now,
      initialDateRange: DateTimeRange(start: now.subtract(const Duration(days: 2)), end: now),
      builder: (c, w) => Theme(
          data: buildTheme().copyWith(colorScheme: const ColorScheme.dark(primary: C.hot, onPrimary: C.bg, surface: C.panel, onSurface: C.text)),
          child: w!),
    );
    if (r == null || !context.mounted) return;
    final end = DateTime(r.end.year, r.end.month, r.end.day, 23, 59, 59);
    await _set(context, DroneWindow.span(r.start.millisecondsSinceEpoch / 1000, end.millisecondsSinceEpoch / 1000));
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final w = m.droneWindow;
        final list = m.windowDrones;
        final live = list.where((d) => m.now - d.last < droneLiveS).toList();
        final past = list.where((d) => m.now - d.last >= droneLiveS).toList();
        return RefreshIndicator(
          color: C.hot,
          onRefresh: App.I.dash.fetchDroneWindow,
          child: ListView(padding: const EdgeInsets.only(bottom: 20), children: [
            PillRow(children: [
              for (final h in [1, 4, 8, 24])
                Pill('${h}H', color: C.hot, on: w.hours == h, onTap: () => _set(context, DroneWindow.hours(h), hours: h)),
              Pill('ALL', color: C.hot, on: w.hours < 0, onTap: () => _set(context, const DroneWindow.all(), hours: 0)),
              Pill(w.hours == 0 ? 'SPAN ${clock(w.since, date: true).split(' ').take(2).join(' ')}' : 'SPAN', color: C.violet, on: w.hours == 0, onTap: () => _span(context)),
            ]),
            if (live.isNotEmpty) SectionHead('IN THE AIR NOW  ${live.length}', color: C.hot),
            for (final d in live) _DroneCard(d, live: true),
            SectionHead('HEARD IN THE WINDOW  ${past.length}', color: C.violet),
            if (list.isEmpty) const Empty('No drones in this window.', color: C.violet),
            for (final d in past) _DroneCard(d, live: false),
          ]),
        );
      },
    );
  }
}

class _DroneCard extends StatelessWidget {
  const _DroneCard(this.d, {required this.live});
  final Drone d;
  final bool live;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final c = live ? C.hot : C.violet;
    final tc = C.droneTag(d.tag);
    final h = d.hagl ?? d.alt;
    return Panel(
      glow: live,
      color: c,
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => DroneDetail(mac: d.mac))),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Icon(Icons.flight, color: c, size: 20, shadows: glowOf(c)),
          const SizedBox(width: 8),
          Expanded(child: Glow(d.label, color: live ? C.text : C.pink, size: 16)),
          Pill(d.tag.toUpperCase(), color: tc, small: true),
        ]),
        const SizedBox(height: 4),
        Text([if (d.maker.isNotEmpty) d.maker, if (d.ua.isNotEmpty) d.ua, if (d.vendor.isNotEmpty && d.maker.isEmpty) d.vendor].join('  /  '),
            overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.amber, fontSize: 12)),
        const SizedBox(height: 8),
        Wrap(spacing: 14, runSpacing: 4, children: [
          _stat('LAST', live ? agoText(d.last) : clock(d.last, date: true), live ? C.green : C.amber),
          if (h != null) _stat(d.hagl != null ? 'AGL' : 'ALT', '${h.round()} m', C.cyan),
          if (d.spd != null) _stat('SPEED', '${d.spd!.toStringAsFixed(1)} m/s', C.cyan),
          if (d.pilotDist != null) _stat('PILOT', '${d.pilotDist} m', C.orange),
          if (d.rssi != null) _stat('RSSI', '${d.rssi}', C.rssi(d.rssi)),
          if (d.closest.isNotEmpty) _stat('NODE', m.nodeName(d.closest), C.hue(m.nodes[d.closest]?.hue)),
        ]),
      ]),
    );
  }

  Widget _stat(String k, String v, Color c) => Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(k, style: const TextStyle(color: C.faint, fontSize: 9.5, fontWeight: FontWeight.w800, letterSpacing: 1)),
        Text(v, style: TextStyle(color: c, fontSize: 13, fontWeight: FontWeight.w700)),
      ]);
}

class DroneDetail extends StatefulWidget {
  const DroneDetail({super.key, required this.mac});
  final String mac;
  @override
  State<DroneDetail> createState() => _DroneDetailState();
}

class _DroneDetailState extends State<DroneDetail> {
  List<Flight>? flights;
  int? shown; // flight drawn on the map

  @override
  void initState() {
    super.initState();
    App.I.dash.flights(widget.mac).then((f) {
      if (mounted) setState(() => flights = f);
    });
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Scaffold(
      appBar: AppBar(title: Glow(widget.mac, color: C.pink, size: 15, font: mono)),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final d = m.drones[widget.mac];
          if (d == null) return const Empty('This drone is not in the current window.');
          final live = m.now - d.last < droneLiveS;
          final f = shown == null || flights == null || shown! >= flights!.length ? null : flights![shown!];
          final centre = d.hasPos ? LatLng(d.lat!, d.lon!) : (d.hasPilot ? LatLng(d.plat!, d.plon!) : fleetCenter());
          return ListView(padding: const EdgeInsets.only(bottom: 30), children: [
            Panel(
              glow: true,
              color: live ? C.hot : C.violet,
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Glow(d.label, color: live ? C.hot : C.pink, size: 21),
                if (d.maker.isNotEmpty) Text(d.maker, style: const TextStyle(color: C.amber, fontWeight: FontWeight.w600)),
                const SizedBox(height: 10),
                Wrap(spacing: 7, runSpacing: 7, children: [
                  for (final t in m.droneTags)
                    Pill(t.toUpperCase(), color: C.droneTag(t), on: d.tag == t, small: true, onTap: () => App.I.dash.droneMeta(d.mac, {'tag': t})),
                ]),
                const SizedBox(height: 8),
                Wrap(spacing: 7, children: [
                  Pill('RENAME', color: C.accent, onTap: () async {
                    final v = await askText(context, 'NAME THIS DRONE', d.alias);
                    if (v != null) App.I.dash.droneMeta(d.mac, {'alias': v});
                  }),
                  Pill('NOTES', color: C.purple, onTap: () async {
                    final v = await askText(context, 'NOTES', d.notes, maxLines: 4);
                    if (v != null) App.I.dash.droneMeta(d.mac, {'notes': v});
                  }),
                ]),
              ]),
            ),
            if (centre != null)
              Panel(
                padding: EdgeInsets.zero,
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(14),
                  child: SizedBox(
                    height: 260,
                    child: MiniMap(center: centre, zoom: 16, layers: [
                      nodeMarkers(),
                      PolylineLayer(polylines: [
                        if (f == null) ...droneLines(d, hot: live),
                        if (f != null && f.ptrack.length > 1)
                          Polyline(points: [for (final p in f.ptrack) LatLng(p[0], p[1])], color: C.orange, strokeWidth: 2.5, pattern: StrokePattern.dashed(segments: const [6, 5])),
                        if (f != null && f.track.length > 1) Polyline(points: [for (final p in f.track) LatLng(p[0], p[1])], color: C.cyan, strokeWidth: 3.5),
                      ]),
                      MarkerLayer(markers: f == null ? droneMarkers(d) : [
                        if (f.pilot != null) Marker(point: LatLng(f.pilot![0], f.pilot![1]), child: Icon(Icons.person_pin_circle, color: C.orange, shadows: glowOf(C.orange))),
                      ]),
                    ]),
                  ),
                ),
              ),
            SectionHead(live ? 'IN THE AIR' : 'LAST REPORT', color: live ? C.hot : C.violet),
            Panel(
              child: Column(children: [
                KV('Last heard', live ? agoText(d.last) : clock(d.last, date: true), color: live ? C.green : C.amber),
                KV('First heard', clock(d.first, date: true)),
                if (d.status.isNotEmpty) KV('Status', d.status, color: C.cyan),
                if (d.serial.isNotEmpty) KV('Remote ID', d.serial, monoValue: true, color: C.pink),
                if (d.op.isNotEmpty) KV('Operator', d.op, monoValue: true),
                if (d.desc.isNotEmpty) KV('Description', d.desc),
                if (d.ua.isNotEmpty) KV('Aircraft', d.ua),
                if (d.vendor.isNotEmpty) KV('Radio maker', d.vendor, color: C.amber),
                if (d.hasPos) KV('Position', '${d.lat!.toStringAsFixed(6)}, ${d.lon!.toStringAsFixed(6)}', monoValue: true),
                if (d.alt != null) KV('Altitude', '${d.alt!.round()} m MSL'),
                if (d.hagl != null) KV('Height', '${d.hagl!.round()} m above ground', color: C.cyan),
                if (d.spd != null) KV('Speed', '${d.spd!.toStringAsFixed(1)} m/s${d.vspd != null ? ', ${d.vspd!.toStringAsFixed(1)} m/s vertical' : ''}'),
                if (d.hdg != null) KV('Heading', '${d.hdg!.round()} deg'),
                if (d.hasPilot) KV('Pilot', '${d.plat!.toStringAsFixed(6)}, ${d.plon!.toStringAsFixed(6)}${d.pilotDist != null ? '  (${d.pilotDist} m away)' : ''}', color: C.orange),
                if (d.rssi != null) KV('Signal', '${d.rssi} dBm', color: C.rssi(d.rssi)),
                if (d.closest.isNotEmpty) KV('Closest node', m.nodeName(d.closest)),
                KV('Source', d.src.isEmpty ? '-' : d.src.toUpperCase()),
                KV('Reports', '${d.reports}'),
                if (d.notes.isNotEmpty) KV('Notes', d.notes, color: C.purple),
              ]),
            ),
            const SectionHead('PAST FLIGHTS', color: C.cyan),
            if (flights == null)
              const Padding(padding: EdgeInsets.all(20), child: Center(child: CircularProgressIndicator(color: C.cyan)))
            else if (flights!.isEmpty)
              const Empty('No stored flights.', color: C.cyan)
            else
              for (final (i, fl) in flights!.indexed)
                Panel(
                  glow: shown == i,
                  color: C.cyan,
                  onTap: () => setState(() => shown = shown == i ? null : i),
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                  child: Row(children: [
                    Expanded(
                      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                        Text('${clock(fl.start, date: true)}  for ${dur(fl.end - fl.start)}', style: const TextStyle(color: C.text, fontWeight: FontWeight.w600)),
                        Text(
                            [
                              if (fl.maxH != null) 'max ${fl.maxH!.round()} m ${fl.agl ? 'AGL' : 'MSL'}',
                              if (fl.maxSpd != null) '${fl.maxSpd!.toStringAsFixed(1)} m/s',
                              if (fl.dist != null) '${fl.dist} m flown',
                              if (fl.pilotMax != null) 'pilot to ${fl.pilotMax} m',
                            ].join('  /  '),
                            style: const TextStyle(color: C.cyan, fontSize: 12)),
                      ]),
                    ),
                    Text(shown == i ? 'ON MAP' : '${fl.reports} rpt', style: TextStyle(color: shown == i ? C.green : C.muted, fontSize: 11, fontWeight: FontWeight.w700)),
                  ]),
                ),
          ]);
        },
      ),
    );
  }
}
