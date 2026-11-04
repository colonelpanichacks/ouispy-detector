// DRONES: every Remote ID drone heard in the window (1H/4H/8H/24H/ALL or a span), live
// ones on top; a drone's page has its track, pilot, tag and past flights.
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'feed_view.dart';
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
      builder: (c, w) => Theme(data: buildTheme(), child: w!),
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
          color: C.accent,
          onRefresh: App.I.dash.fetchDroneWindow,
          child: ListView(
            padding: const EdgeInsets.only(bottom: 20),
            children: [
              ChipRow(
                children: [
                  for (final h in [1, 4, 8, 24])
                    Chip2(
                      '$h h',
                      on: w.hours == h,
                      onTap: () => _set(context, DroneWindow.hours(h), hours: h),
                    ),
                  Chip2('All time', on: w.hours < 0, onTap: () => _set(context, const DroneWindow.all(), hours: 0)),
                  Chip2(
                    w.hours == 0 ? '${clock(w.since, date: true).split(' ').take(2).join(' ')} to ${clock(w.until, date: true).split(' ').take(2).join(' ')}' : 'Dates',
                    icon: Ph.calendarBlank,
                    on: w.hours == 0,
                    onTap: () => _span(context),
                  ),
                ],
              ),
              if (live.isNotEmpty) ...[
                SectionHead('In the air now', trailing: Tag('${live.length} live', color: C.hot)),
                for (final d in live) _DroneCard(d, live: true),
              ],
              if (list.isEmpty)
                linkEmpty(context, 'No drones', 'No Remote ID drone was heard in this window.', icon: Ph.drone)
              else ...[
                SectionHead('Heard in this window', trailing: Text('${past.length}', style: T.code)),
                for (final d in past) _DroneCard(d, live: false),
              ],
            ],
          ),
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
    final h = d.hagl ?? d.alt;
    final what = [if (d.maker.isNotEmpty) d.maker, if (d.ua.isNotEmpty) d.ua, if (d.vendor.isNotEmpty && d.maker.isEmpty) d.vendor].join(' · ');
    return Panel(
      edge: live ? C.hot : null,
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => DroneDetail(mac: d.mac))),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              IconTile(live ? PhF.drone : Ph.drone, live ? C.hot : C.muted),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(d.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                    if (what.isNotEmpty) ...[const SizedBox(height: 3), Text(what, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta)],
                  ],
                ),
              ),
              if (d.tag.isNotEmpty && d.tag != 'unknown') Tag(_cap(d.tag), color: C.droneTag(d.tag)),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              _stat('Last', live ? agoText(d.last) : clock(d.last, date: true), live ? C.green : C.soft),
              if (h != null) _stat(d.hagl != null ? 'Height' : 'Alt', '${h.round()} m', C.text),
              if (d.spd != null) _stat('Speed', '${d.spd!.toStringAsFixed(1)} m/s', C.text),
              if (d.pilotDist != null) _stat('Pilot', '${d.pilotDist} m', C.orange),
              if (d.rssi != null) _stat('Signal', '${d.rssi}', C.rssi(d.rssi)),
            ],
          ),
          if (d.closest.isNotEmpty) ...[
            const SizedBox(height: 8),
            Row(
              children: [
                Dot(C.hue(m.nodes[d.closest]?.hue), size: 6),
                const SizedBox(width: 6),
                Text('Closest node: ${m.nodeName(d.closest)}', style: T.meta),
              ],
            ),
          ],
        ],
      ),
    );
  }

  Widget _stat(String k, String v, Color c) => Expanded(
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(k, style: T.meta.copyWith(fontSize: 11.5)),
        const SizedBox(height: 2),
        Text(
          v,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: T.code.copyWith(color: c, fontSize: 13, fontWeight: FontWeight.w600),
        ),
      ],
    ),
  );
}

String _cap(String s) => s.isEmpty ? s : s[0].toUpperCase() + s.substring(1);

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
      appBar: AppBar(title: const Text('Drone')),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final d = m.drones[widget.mac];
          if (d == null) return const Empty('Not in this window', body: 'Widen the time window on the Drones tab to see it.', icon: Ph.drone);
          final live = m.now - d.last < droneLiveS;
          final f = shown == null || flights == null || shown! >= flights!.length ? null : flights![shown!];
          final centre = d.hasPos ? LatLng(d.lat!, d.lon!) : (d.hasPilot ? LatLng(d.plat!, d.plon!) : fleetCenter());
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 4, 20, 4),
                child: Row(
                  children: [
                    IconTile(live ? PhF.drone : Ph.drone, live ? C.hot : C.muted, size: 48),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(d.label, style: T.title),
                          const SizedBox(height: 3),
                          Text(live ? 'In the air now' : 'Last heard ${clock(d.last, date: true)}', style: T.meta.copyWith(color: live ? C.hot : C.muted)),
                          if (d.maker.isNotEmpty) Text(d.maker, style: T.meta),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 12, 16, 0),
                child: Seg<String>(
                  items: [for (final t in m.droneTags) (t, _cap(t))],
                  value: d.tag,
                  color: C.droneTag(d.tag),
                  onChanged: (t) => App.I.dash.droneMeta(d.mac, {'tag': t}),
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 6),
                child: Row(
                  children: [
                    Expanded(
                      child: Btn(
                        'Rename',
                        icon: Ph.pencilSimple,
                        expand: true,
                        onTap: () async {
                          final v = await askText(context, 'Name this drone', d.alias);
                          if (v != null) App.I.dash.droneMeta(d.mac, {'alias': v});
                        },
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Btn(
                        'Notes',
                        icon: Ph.notePencil,
                        color: C.violet,
                        expand: true,
                        onTap: () async {
                          final v = await askText(context, 'Notes', d.notes, maxLines: 4);
                          if (v != null) App.I.dash.droneMeta(d.mac, {'notes': v});
                        },
                      ),
                    ),
                  ],
                ),
              ),
              if (centre != null)
                Panel(
                  padding: EdgeInsets.zero,
                  margin: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                  child: SizedBox(
                    height: 260,
                    child: MiniMap(
                      center: centre,
                      zoom: 16,
                      layers: [
                        nodeMarkers(),
                        PolylineLayer(
                          polylines: [
                            if (f == null) ...droneLines(d, hot: live),
                            if (f != null && f.ptrack.length > 1)
                              Polyline(
                                points: [for (final p in f.ptrack) LatLng(p[0], p[1])],
                                color: C.orange,
                                strokeWidth: 2.5,
                                pattern: StrokePattern.dashed(segments: const [6, 5]),
                              ),
                            if (f != null && f.track.length > 1) Polyline(points: [for (final p in f.track) LatLng(p[0], p[1])], color: C.accent, strokeWidth: 3.5),
                          ],
                        ),
                        MarkerLayer(
                          markers: f == null ? droneMarkers(d) : [if (f.pilot != null) Marker(point: LatLng(f.pilot![0], f.pilot![1]), child: const PilotPin())],
                        ),
                      ],
                    ),
                  ),
                ),
              Group(
                title: live ? 'In the air' : 'Last report',
                children: [
                  KV('Last heard', live ? agoText(d.last) : clock(d.last, date: true), color: live ? C.green : C.text),
                  KV('First heard', clock(d.first, date: true)),
                  if (d.status.isNotEmpty) KV('Status', d.status),
                  if (d.serial.isNotEmpty) KV('Remote ID', d.serial, monoValue: true, color: C.accent),
                  if (d.op.isNotEmpty) KV('Operator', d.op, monoValue: true),
                  if (d.desc.isNotEmpty) KV('Description', d.desc),
                  if (d.ua.isNotEmpty) KV('Aircraft', d.ua),
                  if (d.vendor.isNotEmpty) KV('Radio maker', d.vendor, color: C.amber),
                  if (d.hasPos) KV('Position', '${d.lat!.toStringAsFixed(6)}, ${d.lon!.toStringAsFixed(6)}', monoValue: true),
                  if (d.alt != null) KV('Altitude', '${d.alt!.round()} m MSL'),
                  if (d.hagl != null) KV('Height', '${d.hagl!.round()} m above ground'),
                  if (d.spd != null) KV('Speed', '${d.spd!.toStringAsFixed(1)} m/s${d.vspd != null ? ', ${d.vspd!.toStringAsFixed(1)} m/s vertical' : ''}'),
                  if (d.hdg != null) KV('Heading', '${d.hdg!.round()}°'),
                  if (d.hasPilot)
                    KV(
                      'Pilot',
                      '${d.plat!.toStringAsFixed(6)}, ${d.plon!.toStringAsFixed(6)}${d.pilotDist != null ? '\n${d.pilotDist} m from the drone' : ''}',
                      color: C.orange,
                      monoValue: true,
                    ),
                  if (d.rssi != null) KV('Signal', '${d.rssi} dBm', color: C.rssi(d.rssi)),
                  if (d.closest.isNotEmpty) KV('Closest node', m.nodeName(d.closest)),
                  KV('Source', d.src.isEmpty ? '-' : d.src.toUpperCase()),
                  KV('Reports', '${d.reports}'),
                  if (d.notes.isNotEmpty) KV('Notes', d.notes),
                ],
              ),
              SectionHead('Past flights', trailing: flights == null || flights!.isEmpty ? null : Text('Tap one to draw it', style: T.meta.copyWith(fontSize: 11.5))),
              if (flights == null)
                const Padding(
                  padding: EdgeInsets.all(20),
                  child: Center(child: CircularProgressIndicator()),
                )
              else if (flights!.isEmpty)
                const Empty('No stored flights', icon: Ph.path)
              else
                for (final (i, fl) in flights!.indexed)
                  Panel(
                    edge: shown == i ? C.accent : null,
                    onTap: () => setState(() => shown = shown == i ? null : i),
                    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
                    child: Row(
                      children: [
                        Icon(shown == i ? PhF.path : Ph.path, color: shown == i ? C.accent : C.muted, size: 20),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text('${clock(fl.start, date: true)}  ·  ${dur(fl.end - fl.start)}', style: T.row.copyWith(fontSize: 14)),
                              const SizedBox(height: 3),
                              Text(
                                [
                                  if (fl.maxH != null) 'max ${fl.maxH!.round()} m ${fl.agl ? 'AGL' : 'MSL'}',
                                  if (fl.maxSpd != null) '${fl.maxSpd!.toStringAsFixed(1)} m/s',
                                  if (fl.dist != null) '${fl.dist} m flown',
                                  if (fl.pilotMax != null) 'pilot to ${fl.pilotMax} m',
                                ].join(' · '),
                                style: T.meta,
                              ),
                            ],
                          ),
                        ),
                        Text(shown == i ? 'On map' : '${fl.reports}', style: T.code.copyWith(color: shown == i ? C.accent : C.muted)),
                      ],
                    ),
                  ),
            ],
          );
        },
      ),
    );
  }
}
