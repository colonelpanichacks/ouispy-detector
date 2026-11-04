// DRONES: every Remote ID drone heard in the window (1H/4H/8H/24H/ALL or a span), live
// ones on top; a drone's page has its track, pilot, tag and past flights. Every drone shows
// who made it at once (rid_lookup.dart: the serial's maker code, then the FAA registry).
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart' hide Path;

import '../data/model.dart';
import '../data/rid_lookup.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'fpv_view.dart';
import '../data/fpv.dart';
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
    await a.link.fetchDroneWindow();
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
      listenable: Listenable.merge([m, RidLookup.I, Fpv.I, droneKind]),
      builder: (context, _) {
        final w = m.droneWindow;
        // one feed: Remote ID drones and the FPV transmitters the Recon's receiver finds
        final kind = droneKind.value;
        final since = w.hours > 0 ? m.now - w.hours * 3600 : (w.hours == 0 ? w.since : 0.0);
        final fpv = kind == 'rid'
            ? <FpvDet>[]
            : [
                for (final d in Fpv.I.dets)
                  if (d.ts >= since && (w.hours != 0 || d.ts <= w.until)) d,
              ];
        final fpvLive = [
          for (final d in fpv)
            if (m.now - d.ts < droneLiveS) d,
        ];
        final list = kind == 'fpv' ? <Drone>[] : m.windowDrones;
        final live = list.where((d) => m.now - d.last < droneLiveS).toList();
        final past = list.where((d) => m.now - d.last >= droneLiveS).toList();
        for (final d in list) {
          RidLookup.I.ensure(d);
        }
        return RefreshIndicator(
          color: C.accent,
          onRefresh: App.I.link.fetchDroneWindow,
          child: ListView(
            padding: const EdgeInsets.only(bottom: 20),
            children: [
              _Kpis(all: list, live: live),
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
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
                child: Seg<String>(items: const [('all', 'Everything'), ('rid', 'Remote ID'), ('fpv', 'FPV video')], value: kind, onChanged: (v) => droneKind.value = v),
              ),
              if (kind != 'rid' && Fpv.I.info != null) const _FpvReceiver(),
              if (live.isNotEmpty || fpvLive.isNotEmpty) ...[
                SectionHead('In the air now', trailing: Tag('${live.length + fpvLive.length} live', color: C.hot)),
                for (final d in live) _DroneCard(d, live: true),
                if (fpvLive.isNotEmpty)
                  Panel(
                    padding: EdgeInsets.zero,
                    edge: C.orange,
                    child: Column(children: [for (final d in fpvLive) FpvDetRow(d)]),
                  ),
              ],
              if (list.isEmpty && fpv.isEmpty)
                linkEmpty(
                  context,
                  'Nothing in the air',
                  kind == 'fpv' ? 'No FPV transmitter was found in this window.' : 'No drone was heard in this window.',
                  icon: Ph.drone,
                )
              else ...[
                SectionHead('Heard in this window', trailing: Text('${past.length + fpv.length - fpvLive.length}', style: T.code)),
                for (final d in past) _DroneCard(d, live: false),
                if (fpv.length > fpvLive.length)
                  Panel(
                    padding: EdgeInsets.zero,
                    child: Column(
                      children: [
                        for (final (k, d) in [
                          for (final d in fpv)
                            if (m.now - d.ts >= droneLiveS) d,
                        ].indexed) ...[if (k > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), FpvDetRow(d)],
                      ],
                    ),
                  ),
              ],
            ],
          ),
        );
      },
    );
  }
}

/// Which kinds the Drones feed shows: all, Remote ID or FPV.
final droneKind = ValueNotifier<String>('all');

/// The receiver itself: a live thumbnail, where it is tuned, and the way to its screen.
class _FpvReceiver extends StatelessWidget {
  const _FpvReceiver();
  @override
  Widget build(BuildContext context) {
    final i = Fpv.I.info!;
    return Panel(
      edge: i.video ? C.green : null,
      padding: const EdgeInsets.all(10),
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const FpvView())),
      child: Row(
        children: [
          const FpvThumb(),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('FPV RECEIVER', style: T.silk),
                const SizedBox(height: 4),
                Text('${i.channelName}  ${i.mhz} MHz', style: T.row),
                const SizedBox(height: 2),
                Text(
                  i.video ? 'Video, ${(i.level * 100).round()}% signal' : (i.scanning ? 'Scanning' : 'No signal'),
                  style: T.meta.copyWith(color: i.video ? C.green : C.muted),
                ),
              ],
            ),
          ),
          const Icon(Ph.caretRight, size: 16, color: C.silkMark),
        ],
      ),
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
    final who = RidLookup.I.describe(d);
    final what = [if (who.known) who.text, if (d.ua.isNotEmpty) d.ua].join(' · ');
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
                    if (what.isNotEmpty) ...[
                      const SizedBox(height: 3),
                      Row(
                        children: [
                          Flexible(
                            child: Text(what, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta),
                          ),
                          if (who.known) ...[const SizedBox(width: 6), SourceMark(who.source)],
                        ],
                      ),
                    ],
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
    App.I.link.flights(widget.mac).then((f) {
      if (mounted) setState(() => flights = f);
    });
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Scaffold(
      appBar: AppBar(title: const Text('Drone')),
      body: ListenableBuilder(
        listenable: Listenable.merge([m, RidLookup.I, followDrone]),
        builder: (context, _) {
          final d = m.drones[widget.mac];
          if (d == null) return const Empty('Not in this window', body: 'Widen the time window on the Drones tab to see it.', icon: Ph.drone);
          RidLookup.I.ensure(d);
          final live = m.now - d.last < droneLiveS;
          final who = RidLookup.I.describe(d);
          final faa = RidLookup.I.state(d);
          final following = followDrone.value == d.mac;
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
                          if (who.known) ...[
                            const SizedBox(height: 3),
                            Row(
                              children: [
                                Flexible(
                                  child: Text(
                                    who.text,
                                    maxLines: 1,
                                    overflow: TextOverflow.ellipsis,
                                    style: T.meta.copyWith(color: C.soft),
                                  ),
                                ),
                                const SizedBox(width: 6),
                                SourceMark(who.source),
                              ],
                            ),
                          ],
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
                  onChanged: (t) => App.I.link.droneMeta(d.mac, {'tag': t}),
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
                          if (v != null) App.I.link.droneMeta(d.mac, {'alias': v});
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
                          if (v != null) App.I.link.droneMeta(d.mac, {'notes': v});
                        },
                      ),
                    ),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 2, 16, 6),
                child: ActionStrip([
                  (
                    'Show on map',
                    Ph.mapPin,
                    C.accent,
                    d.hasPos || d.hasPilot
                        ? () {
                            showDrone.value = null;
                            showDrone.value = d.mac;
                            Navigator.of(context).popUntil((r) => r.isFirst);
                          }
                        : null,
                  ),
                  (
                    following ? 'Following' : 'Follow',
                    following ? PhF.gpsFix : Ph.gpsFix,
                    C.hot,
                    following
                        ? () => followDrone.value = null
                        : (d.hasPos
                              ? () {
                                  followDrone.value = d.mac;
                                  Navigator.of(context).popUntil((r) => r.isFirst);
                                }
                              : null),
                  ),
                  (
                    faa == FaaState.pending ? 'Looking up' : 'FAA lookup',
                    Ph.magnifyingGlass,
                    C.green,
                    d.serial.trim().isEmpty || faa == FaaState.pending
                        ? null
                        : () async {
                            final e = await RidLookup.I.lookup(d, force: true);
                            if (e == null) return;
                            m.toast(
                              !e.ok
                                  ? 'FAA lookup failed: no answer'
                                  : e.items.isEmpty
                                  ? 'no FAA record for ${d.serial}'
                                  : 'FAA: ${RidLookup.I.describe(d).text}',
                            );
                          },
                  ),
                ]),
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
              if (d.hasPos || d.track.isNotEmpty)
                Panel(
                  margin: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                  padding: const EdgeInsets.fromLTRB(12, 10, 12, 12),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(d.hasPilot ? 'RADAR · AROUND THE PILOT' : 'RADAR · TRACK', style: T.silk),
                      const SizedBox(height: 8),
                      AspectRatio(
                        aspectRatio: 1,
                        child: CustomPaint(
                          painter: DroneRadar(d, live: live),
                          size: Size.infinite,
                        ),
                      ),
                    ],
                  ),
                ),
              Group(
                title: 'Who made it',
                children: [
                  KV('Maker', who.known ? '${who.text}  ·  ${who.source.toUpperCase()}' : '-', color: who.known ? sourceColor(who.source) : C.muted),
                  if (who.idt.isNotEmpty) KV('ID type', who.idt),
                  if (who.code.isNotEmpty) KV('Maker code', _codeText(d.serial), monoValue: true),
                  KV('FAA record', _faaText(faa, who, d), color: faa == FaaState.found ? C.green : (faa == FaaState.failed ? C.red : C.muted)),
                ],
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

String _codeText(String serial) {
  final p = RidSerial.parse(serial);
  if (p == null) return '-';
  final mk = p.maker;
  final len = p.declaredLen == null
      ? 'length char ${p.lengthChar} unreadable'
      : (p.lengthOk ? '${p.declaredLen} chars' : 'says ${p.declaredLen} chars, has ${p.body.length}');
  return '${p.code} = ${mk ?? 'not in the table'}  ·  $len';
}

String _faaText(FaaState s, RidInfo who, Drone d) => switch (s) {
  FaaState.pending => 'looking up',
  FaaState.found => [who.source == RidSource.faa ? who.text : 'on record', if (who.faaStatus.isNotEmpty) '(${who.faaStatus})'].join(' '),
  FaaState.missing => 'no record for this serial',
  FaaState.failed => 'no answer, tries again in an hour',
  FaaState.none =>
    d.serial.trim().isEmpty
        ? 'no serial broadcast yet'
        : !faaIdTypes.contains(ridIdType(d.raw['idt']))
        ? 'not a serial the FAA indexes'
        : (RidLookup.I.faaLookup ? 'not looked up yet' : 'automatic lookup is off'),
};

Color sourceColor(String source) => switch (source) {
  RidSource.faa => C.green,
  RidSource.serial => C.accent,
  RidSource.vendor => C.amber,
  _ => C.muted,
};

/// Where a maker came from, as a small printed tag: FAA / SERIAL CODE / RADIO VENDOR.
class SourceMark extends StatelessWidget {
  const SourceMark(this.source, {super.key});
  final String source;
  @override
  Widget build(BuildContext context) {
    if (source.isEmpty) return const SizedBox.shrink();
    final c = sourceColor(source);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1.5),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(2),
        border: Border.all(color: c.withValues(alpha: 0.5)),
      ),
      child: Text(source.toUpperCase(), style: T.silk.copyWith(fontSize: 9, letterSpacing: 0.8, color: c)),
    );
  }
}

/// The drones window's readouts, as the web dashboard's: live, airborne, pilots located,
/// highest AGL, in window.
class _Kpis extends StatelessWidget {
  const _Kpis({required this.all, required this.live});
  final List<Drone> all, live;
  @override
  Widget build(BuildContext context) {
    final airborne = live.where((d) => d.status == 'Airborne').length;
    final pilots = live.where((d) => d.hasPilot).length;
    double hi = -1;
    for (final d in live) {
      if (d.hagl != null) hi = math.max(hi, d.hagl!);
    }
    Widget cell(Widget w, {bool right = true, bool bottom = true}) => Container(
      decoration: BoxDecoration(
        border: Border(
          right: right ? const BorderSide(color: C.silkLine) : BorderSide.none,
          bottom: bottom ? const BorderSide(color: C.silkLine) : BorderSide.none,
        ),
      ),
      child: w,
    );
    return Panel(
      padding: EdgeInsets.zero,
      margin: const EdgeInsets.fromLTRB(16, 18, 16, 4),
      child: Column(
        children: [
          IntrinsicHeight(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(
                  child: cell(Stat('Live', '${live.length}', icon: Ph.drone, color: live.isNotEmpty ? C.hot : C.text)),
                ),
                Expanded(child: cell(Stat('Airborne', '$airborne', color: airborne > 0 ? C.hot : C.text))),
                Expanded(child: cell(Stat('Pilots located', '$pilots', color: pilots > 0 ? C.orange : C.text), right: false)),
              ],
            ),
          ),
          IntrinsicHeight(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(child: cell(Stat('Highest AGL', hi >= 0 ? '${hi.round()} m' : '-'), bottom: false)),
                Expanded(child: cell(Stat('In window', '${all.length}'), right: false, bottom: false)),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// The Tab5's radar box: the drone's track around its pilot (or around itself with no
/// pilot), north up, rings at a round number of metres, labelled.
class DroneRadar extends CustomPainter {
  DroneRadar(this.d, {required this.live});
  final Drone d;
  final bool live;

  static double niceStep(double span) {
    final raw = span / 3;
    final p = math.pow(10, (math.log(raw) / math.ln10).floor()).toDouble();
    for (final k in [1.0, 2.0, 5.0, 10.0]) {
      if (k * p >= raw) return k * p;
    }
    return 10 * p;
  }

  @override
  void paint(Canvas c, Size s) {
    final side = math.min(s.width, s.height);
    final ctr = Offset(s.width / 2, s.height / 2);
    final rad = side / 2 - 10;
    final col = live ? C.hot : C.muted;
    final track = d.track;
    final double ola, olo;
    if (d.hasPilot) {
      ola = d.plat!;
      olo = d.plon!;
    } else if (d.hasPos) {
      ola = d.lat!;
      olo = d.lon!;
    } else if (track.isNotEmpty) {
      ola = track.last[0];
      olo = track.last[1];
    } else {
      return;
    }
    final kx = 111320.0 * math.cos(ola * math.pi / 180);
    Offset m(double la, double lo) => Offset((lo - olo) * kx, (la - ola) * 111320.0);
    var span = 30.0; // metres from the centre to the edge, at least
    for (final p in [
      ...track,
      if (d.hasPos) [d.lat!, d.lon!],
    ]) {
      final o = m(p[0], p[1]);
      span = math.max(span, math.max(o.dx.abs(), o.dy.abs()) * 1.15);
    }
    final step = niceStep(span);
    span = math.max(span, step);
    Offset px(double la, double lo) {
      final o = m(la, lo);
      return ctr + Offset(o.dx, -o.dy) * (rad / span);
    }

    // ground and rings
    c.drawCircle(ctr, rad, Paint()..color = C.panel.withValues(alpha: 0.6));
    final ring = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..color = C.silkLine;
    for (var r = step; r <= span + 0.01; r += step) {
      final rr = r / span * rad;
      c.drawCircle(ctr, rr, ring);
      _text(c, '${r.round()} m', ctr + Offset(rr * 0.7071 + 3, -rr * 0.7071 - 12), C.silk);
    }
    c.drawCircle(ctr, rad, ring..color = C.silkMark);
    c.drawLine(ctr - Offset(rad, 0), ctr + Offset(rad, 0), ring..color = C.silkLine.withValues(alpha: 0.6));
    c.drawLine(ctr - Offset(0, rad), ctr + Offset(0, rad), ring);
    _text(c, 'N', ctr + Offset(-4, -rad - 1), C.silk);

    // track, oldest faint
    for (var i = 1; i < track.length; i++) {
      c.drawLine(
        px(track[i - 1][0], track[i - 1][1]),
        px(track[i][0], track[i][1]),
        Paint()
          ..strokeWidth = 2
          ..strokeCap = StrokeCap.round
          ..color = col.withValues(alpha: 0.3 + 0.7 * i / track.length),
      );
    }
    // pilot at the centre, dashed line to the drone
    if (d.hasPilot) {
      c.drawCircle(ctr, 5, Paint()..color = C.orange);
      c.drawCircle(
        ctr,
        8,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.2
          ..color = C.orange.withValues(alpha: 0.6),
      );
    }
    if (d.hasPos) {
      final p = px(d.lat!, d.lon!);
      if (d.hasPilot) {
        final v = p - ctr, n = v.distance;
        final dash = Paint()
          ..strokeWidth = 1.2
          ..color = C.orange.withValues(alpha: 0.7);
        for (double t = 10; t < n - 8; t += 9) {
          c.drawLine(ctr + v * (t / n), ctr + v * (math.min(t + 5, n - 8) / n), dash);
        }
      }
      // the drone: an arrowhead along its heading, a dot without one
      final fill = Paint()..color = col;
      if (d.hdg != null) {
        final a = d.hdg! * math.pi / 180;
        Offset r(double x, double y) => p + Offset(x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a));
        c.drawPath(
          Path()
            ..moveTo(r(0, -9).dx, r(0, -9).dy)
            ..lineTo(r(6, 6).dx, r(6, 6).dy)
            ..lineTo(r(0, 3).dx, r(0, 3).dy)
            ..lineTo(r(-6, 6).dx, r(-6, 6).dy)
            ..close(),
          fill,
        );
      } else {
        c.drawCircle(p, 5, fill);
      }
      if (live) {
        c.drawCircle(
          p,
          11,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1
            ..color = col.withValues(alpha: 0.5),
        );
      }
    }
    _text(c, 'EDGE ${span.round()} M', Offset(s.width - 4, 0), C.silk, right: true);
  }

  void _text(Canvas c, String t, Offset at, Color col, {bool right = false}) {
    final tp = TextPainter(
      text: TextSpan(
        text: t,
        style: T.silk.copyWith(fontSize: 9.5, color: col),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(c, right ? at - Offset(tp.width, 0) : at);
  }

  @override
  bool shouldRepaint(DroneRadar o) => true;
}
