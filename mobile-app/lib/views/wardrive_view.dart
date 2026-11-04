// WARDRIVE: what a wardriver node heard, WiGLE style. The session (recording, a new one),
// the counts, the map of everything placed (coloured by security, as the Tab5 does), the
// network list (search, sort, security and band filters), a network's sheet, and the WiGLE
// CSV export and upload to the user's own WiGLE account.
//
// WardriveView is a body: put it in a Scaffold or a tab (it scrolls itself).
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../data/local_mesh.dart';
import '../data/lookup.dart';
import '../data/wardrive.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'map_view.dart';

/// The Tab5's security colours: open green, WEP red, WPA amber, WPA2 cyan, WPA3 violet;
/// BLE hot, Zigbee lime.
Color wdColor(WdDevice d) {
  if (d.kind == 'ble') return C.hot;
  if (d.kind == 'zigbee') return C.lime;
  if (!d.isWifi) return C.orange;
  return secColor(d.sec);
}

Color secColor(String sec) => switch (sec) {
  'open' => C.green,
  'WEP' => C.red,
  'WPA' => C.amber,
  'WPA2' => C.cyan,
  'WPA3' || 'WPA2/WPA3' => C.violet,
  _ => C.muted,
};

IconData wdIcon(WdDevice d) => switch (d.kind) {
  'ble' => Ph.bluetooth,
  'zigbee' => Ph.hexagon,
  'wifi' => d.open ? Ph.wifiHigh : Ph.lockSimple,
  _ => Ph.broadcast,
};

String bandLabel(String b) => switch (b) {
  '2.4' => '2.4 GHz',
  '5' => '5 GHz',
  '6' => '6 GHz',
  'ble' => 'BLE',
  'zigbee' => 'Zigbee',
  _ => b.toUpperCase(),
};

String _secLabel(WdDevice d) => d.isWifi ? (d.sec.isEmpty ? '?' : d.sec) : bandLabel(d.band);

enum _Sort { recent, signal, name, channel }

class WardriveView extends StatefulWidget {
  const WardriveView({super.key});
  @override
  State<WardriveView> createState() => _WardriveViewState();
}

class _WardriveViewState extends State<WardriveView> {
  final w = Wardrive.I;
  bool all = false; // false: this session
  String sec = ''; // '' all, or open / WEP / WPA / WPA2 / WPA3
  String band = ''; // '' all, or a band
  String q = '';
  _Sort sort = _Sort.recent;
  int show = 150;
  final _q = TextEditingController();

  @override
  void dispose() {
    _q.dispose();
    super.dispose();
  }

  String? get _scope => all ? null : w.currentId;

  List<WdDevice> _list() {
    final ql = q.toLowerCase();
    final l = [
      for (final d in w.inScope(session: _scope))
        if ((band.isEmpty || d.band == band) &&
            (sec.isEmpty || (sec == 'WPA3' ? (d.sec == 'WPA3' || d.sec == 'WPA2/WPA3') : d.sec == sec)) &&
            (ql.isEmpty || '${d.mac} ${d.name} ${d.maker}'.toLowerCase().contains(ql)))
          d,
    ];
    switch (sort) {
      case _Sort.recent:
        l.sort((a, b) => b.last.compareTo(a.last));
      case _Sort.signal:
        l.sort((a, b) => (b.rssi ?? -999).compareTo(a.rssi ?? -999));
      case _Sort.name:
        l.sort((a, b) => a.label.toLowerCase().compareTo(b.label.toLowerCase()));
      case _Sort.channel:
        l.sort((a, b) => (a.ch ?? 999).compareTo(b.ch ?? 999));
    }
    return l;
  }

  Future<void> _refresh() async {
    if (w.onDashboard) await w.refreshDashboard(history: all);
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: w,
    builder: (context, _) {
      final st = w.stats(session: _scope);
      final list = _list();
      return RefreshIndicator(
        color: C.accent,
        onRefresh: _refresh,
        child: ListView(
          padding: const EdgeInsets.only(bottom: 24),
          children: [
            _SessionBar(w: w, all: all),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
              child: Seg<bool>(
                items: const [(false, 'This session'), (true, 'All sessions')],
                value: all,
                onChanged: (v) {
                  setState(() => all = v);
                  if (v && w.onDashboard) w.refreshDashboard(history: true);
                },
              ),
            ),
            _Kpis(st),
            _Map(devices: list, key: ValueKey('map-$all-${w.currentId}')),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
              child: ActionStrip([
                ('Export CSV', Ph.fileCsv, C.accent, () => exportWigle(context, session: _scope)),
                ('WiGLE upload', Ph.cloudArrowUp, C.cyan, () => exportWigle(context, session: _scope, upload: true)),
                ('WiGLE account', Ph.userGear, C.text, () => wigleAccountSheet(context)),
                ('Sessions', Ph.listBullets, C.text, () => _sessions(context)),
              ]),
            ),
            SectionHead(all ? 'Everything heard' : 'This session', trailing: Text('${list.length}', style: T.code)),
            ChipRow(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              children: [
                Chip2('All', on: sec.isEmpty, onTap: () => setState(() => sec = '')),
                for (final s in const ['open', 'WEP', 'WPA', 'WPA2', 'WPA3'])
                  Chip2(
                    s,
                    on: sec == s,
                    color: secColor(s),
                    count: s == 'WPA3' ? (st.bySec['WPA3'] ?? 0) + (st.bySec['WPA2/WPA3'] ?? 0) : st.bySec[s] ?? 0,
                    onTap: () => setState(() => sec = sec == s ? '' : s),
                  ),
              ],
            ),
            if (st.byBand.length > 1)
              ChipRow(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                children: [
                  Chip2('Any band', on: band.isEmpty, onTap: () => setState(() => band = '')),
                  for (final b in st.byBand.keys.toList()..sort())
                    Chip2(
                      bandLabel(b),
                      on: band == b,
                      count: st.byBand[b],
                      color: b == 'ble' ? C.hot : (b == 'zigbee' ? C.lime : C.sky),
                      onTap: () => setState(() => band = band == b ? '' : b),
                    ),
                ],
              ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              child: TextField(
                controller: _q,
                style: T.row,
                onChanged: (v) => setState(() => q = v.trim()),
                decoration: InputDecoration(
                  hintText: 'Search SSID, name, MAC, maker',
                  prefixIcon: const Icon(Ph.magnifyingGlass, size: 18, color: C.silk),
                  suffixIcon: q.isEmpty
                      ? null
                      : IconButton(
                          icon: const Icon(Ph.x, size: 16, color: C.silk),
                          onPressed: () => setState(() {
                            _q.clear();
                            q = '';
                          }),
                        ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              child: Seg<_Sort>(
                items: const [(_Sort.recent, 'Recent'), (_Sort.signal, 'Signal'), (_Sort.name, 'Name'), (_Sort.channel, 'Ch')],
                value: sort,
                onChanged: (v) => setState(() => sort = v),
              ),
            ),
            if (list.isEmpty)
              Empty(
                w.devices.isEmpty ? 'Nothing wardriven yet' : 'No match',
                body: w.devices.isEmpty
                    ? 'Plug in a node running the wardrive build. Every network and BLE device it hears lands here, placed with its own GPS or the phone\'s.'
                    : 'Nothing in this scope matches the filters.',
                icon: Ph.carProfile,
              )
            else
              Panel(
                padding: EdgeInsets.zero,
                margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: Column(
                  children: [
                    for (final (i, d) in list.take(show).indexed) ...[if (i > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), _Row(d)],
                    if (list.length > show)
                      Padding(
                        padding: const EdgeInsets.all(10),
                        child: Btn('Show more (${list.length - show} left)', kind: BtnKind.ghost, onTap: () => setState(() => show += 300)),
                      ),
                  ],
                ),
              ),
          ],
        ),
      );
    },
  );

  Future<void> _sessions(BuildContext context) async {
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      builder: (c) => DraggableScrollableSheet(
        expand: false,
        initialChildSize: 0.6,
        builder: (c, sc) => ListenableBuilder(
          listenable: w,
          builder: (c, _) {
            final ss = w.sessionList;
            return ListView(
              controller: sc,
              children: [
                SectionHead('Sessions', trailing: Text('${ss.length}', style: T.code)),
                if (ss.isEmpty) const Empty('No sessions yet', icon: Ph.listBullets),
                for (final s in ss)
                  Tile(
                    icon: s.remote ? Ph.desktopTower : Ph.deviceMobile,
                    color: s.id == w.currentId ? C.green : C.muted,
                    title: s.note.isNotEmpty ? '${s.label} · ${s.note}' : s.label,
                    subtitle: Text('${clock(s.start, date: true)} to ${clock(s.last, date: true)} · ${w.inScope(session: s.id).length} devices', style: T.meta),
                    trailing: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        if (s.id == w.currentId) const Tag('current', color: C.green),
                        IconButton(
                          tooltip: 'Export this session',
                          icon: const Icon(Ph.fileCsv, size: 18, color: C.accent),
                          onPressed: () => exportWigle(context, session: s.id),
                        ),
                        if (!s.remote)
                          IconButton(
                            tooltip: 'Forget this session',
                            icon: const Icon(Ph.trash, size: 18, color: C.red),
                            onPressed: () async {
                              if (await confirm(c, 'Forget ${s.label}?', 'Its sightings go; devices seen in other sessions stay.', yes: 'Forget')) {
                                w.clear(session: s.id);
                              }
                            },
                          ),
                      ],
                    ),
                  ),
                const SizedBox(height: 20),
              ],
            );
          },
        ),
      ),
    );
  }
}

class _SessionBar extends StatelessWidget {
  const _SessionBar({required this.w, required this.all});
  final Wardrive w;
  final bool all;
  @override
  Widget build(BuildContext context) {
    final s = w.current;
    final rec = w.recording;
    final (c, state) = w.paused ? (C.amber, 'PAUSED') : (rec ? (C.green, 'RECORDING') : (C.muted, 'IDLE'));
    final n = s == null ? 0 : w.inScope(session: s.id).length;
    final gps = w.onDashboard
        ? 'dashboard'
        : w.fix == null
        ? 'no phone fix'
        : 'phone fix ±${(w.fix!.acc ?? 0).round()} m';
    return Panel(
      edge: rec ? C.green : null,
      padding: const EdgeInsets.fromLTRB(14, 12, 10, 12),
      child: Row(
        children: [
          Dot(c, size: 10, halo: rec),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Text(state, style: T.label.copyWith(color: c)),
                    const SizedBox(width: 8),
                    Flexible(
                      child: Text(s?.label ?? 'no session', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                    ),
                  ],
                ),
                const SizedBox(height: 3),
                Text(
                  [if (s != null) 'since ${clock(s.start, date: true)}', '$n devices', if (w.lastLine > 0) 'last ${agoText(w.lastLine)}', gps].join(' · '),
                  maxLines: 2,
                  style: T.meta,
                ),
              ],
            ),
          ),
          IconButton(
            tooltip: w.paused ? 'Resume' : 'Pause',
            icon: Icon(w.paused ? Ph.play : Ph.pause, color: w.paused ? C.green : C.amber, size: 20),
            onPressed: () => w.setPaused(!w.paused),
          ),
          IconButton(
            tooltip: 'New session',
            icon: const Icon(Ph.plusCircle, color: C.accent, size: 22),
            onPressed: () async {
              final note = await askText(context, 'New wardrive session', '', hint: 'A note (optional)');
              if (note == null) return;
              final ns = await w.startSession(note: note);
              if (context.mounted) {
                ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('${ns.label} started')));
              }
            },
          ),
        ],
      ),
    );
  }
}

class _Kpis extends StatelessWidget {
  const _Kpis(this.st);
  final WdStats st;
  @override
  Widget build(BuildContext context) {
    final wifi = st.byKind['wifi'] ?? 0, ble = st.byKind['ble'] ?? 0;
    Widget cell(String l, String v, Color c, [String? sub]) => Expanded(
      child: Stat(l, v, color: c, sub: sub),
    );
    return Panel(
      padding: EdgeInsets.zero,
      margin: const EdgeInsets.fromLTRB(16, 10, 16, 4),
      child: Column(
        children: [
          Row(
            children: [
              cell('Total', '${st.total}', C.text, '${st.newHour} new this hour'),
              cell('WiFi', '$wifi', C.sky, st.byBand['5'] != null ? '${st.byBand['5']} on 5 GHz' : null),
              cell('BLE', '$ble', C.hot),
            ],
          ),
          const Divider(color: C.silkLine),
          Row(
            children: [
              cell('Open', '${st.open}', st.open > 0 ? C.green : C.text),
              cell('Placed', '${st.placed}', C.text, st.total == 0 ? null : '${(100 * st.placed / st.total).round()}%'),
              cell('Hidden / WPS', '${st.hidden} / ${st.wps}', C.text),
            ],
          ),
        ],
      ),
    );
  }
}

class _Map extends StatelessWidget {
  const _Map({super.key, required this.devices});
  final List<WdDevice> devices;
  @override
  Widget build(BuildContext context) {
    final placed = [
      for (final d in devices)
        if (d.placed) d,
    ];
    final w = Wardrive.I;
    if (placed.isEmpty && w.fix == null) return const SizedBox.shrink();
    // the most recent ones, so a long history stays drawable
    placed.sort((a, b) => b.last.compareTo(a.last));
    final shown = placed.take(2000).toList();
    final center = shown.isNotEmpty
        ? LatLng(shown.map((d) => d.lat!).reduce((a, b) => a + b) / shown.length, shown.map((d) => d.lon!).reduce((a, b) => a + b) / shown.length)
        : LatLng(w.fix!.lat, w.fix!.lon);
    return Panel(
      padding: EdgeInsets.zero,
      margin: const EdgeInsets.fromLTRB(16, 6, 16, 4),
      child: SizedBox(
        height: 240,
        child: MiniMap(
          center: center,
          zoom: 15,
          layers: [
            CircleLayer(
              circles: [
                for (final d in shown.reversed)
                  CircleMarker(point: LatLng(d.lat!, d.lon!), radius: 4, color: wdColor(d).withValues(alpha: 0.85), borderColor: C.bg, borderStrokeWidth: 1),
              ],
            ),
            MarkerLayer(
              markers: [
                for (final d in shown.take(400))
                  Marker(
                    point: LatLng(d.lat!, d.lon!),
                    width: 18,
                    height: 18,
                    child: GestureDetector(behavior: HitTestBehavior.opaque, onTap: () => wardriveDeviceSheet(context, d)),
                  ),
                if (w.fix != null && !w.onDashboard) Marker(point: LatLng(w.fix!.lat, w.fix!.lon), width: 16, height: 16, child: const Dot(C.me, size: 12, halo: true)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _Row extends StatelessWidget {
  const _Row(this.d);
  final WdDevice d;
  @override
  Widget build(BuildContext context) {
    final c = wdColor(d);
    final sub = [if (d.ch != null && d.ch! > 0) 'ch ${d.ch}', bandLabel(d.band), if (d.maker.isNotEmpty) d.maker].join(' · ');
    return InkWell(
      onTap: () => wardriveDeviceSheet(context, d),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
        child: Row(
          children: [
            IconTile(wdIcon(d), c),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Flexible(
                        child: Text(
                          d.label,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: T.row.copyWith(color: d.name.isEmpty ? C.muted : null),
                        ),
                      ),
                      if (d.hidden) ...[const SizedBox(width: 6), const Tag('hidden', color: C.silk)],
                      if (d.wps) ...[const SizedBox(width: 6), const Tag('WPS', color: C.orange)],
                    ],
                  ),
                  const SizedBox(height: 3),
                  Text(d.mac, style: T.code),
                  if (sub.isNotEmpty) Text(sub, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta.copyWith(fontSize: 11.5)),
                ],
              ),
            ),
            const SizedBox(width: 8),
            Column(
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                Tag(_secLabel(d), color: c),
                const SizedBox(height: 6),
                Row(
                  children: [
                    Text(
                      '${d.rssi ?? '-'}',
                      style: T.code.copyWith(color: C.rssi(d.rssi), fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(width: 4),
                    RssiBar(d.rssi),
                  ],
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

/// A wardriven device's sheet: every field, and where it was heard.
Future<void> wardriveDeviceSheet(BuildContext context, WdDevice d) => showModalBottomSheet<void>(
  context: context,
  isScrollControlled: true,
  builder: (c) => DraggableScrollableSheet(
    expand: false,
    initialChildSize: 0.8,
    maxChildSize: 0.95,
    builder: (c, sc) => _DeviceSheet(d: d, sc: sc),
  ),
);

class _DeviceSheet extends StatelessWidget {
  const _DeviceSheet({required this.d, required this.sc});
  final WdDevice d;
  final ScrollController sc;

  static const _flags = [
    (0x01, 'LE Limited Discoverable'),
    (0x02, 'LE General Discoverable'),
    (0x04, 'BR/EDR not supported'),
    (0x08, 'LE + BR/EDR (controller)'),
    (0x10, 'LE + BR/EDR (host)'),
  ];

  @override
  Widget build(BuildContext context) {
    final c = wdColor(d);
    final w = Wardrive.I;
    final rows = <Widget>[
      KV('MAC', d.mac, monoValue: true),
      KV('Kind', '${d.kind.toUpperCase()} · ${bandLabel(d.band)}'),
      KV(d.isWifi ? 'SSID' : 'Name', d.name.isEmpty ? (d.isWifi ? '(hidden)' : '-') : d.name),
      if (d.maker.isNotEmpty) KV('Maker', d.maker),
      KV('Address', d.rnd || Lookup.randomMac(d.mac) ? 'random (locally administered)' : 'public (globally unique)'),
      if (d.isWifi) KV('Security', d.sec.isEmpty ? 'unknown' : d.sec, color: secColor(d.sec)),
      if (d.ch != null) KV('Channel', '${d.ch}'),
      if (d.isWifi && d.std.isNotEmpty)
        KV('Standard', switch (d.std) {
          'ax' => 'WiFi 6 (802.11ax)',
          'ac' => 'WiFi 5 (802.11ac)',
          'n' => 'WiFi 4 (802.11n)',
          _ => d.std,
        }),
      if (d.rate != null) KV('Top legacy rate', '${d.rate} Mb/s'),
      if (d.cc.isNotEmpty) KV('Country', d.cc),
      if (d.isWifi) KV('Hidden / WPS', '${d.hidden ? 'hidden' : 'broadcast'} · ${d.wps ? 'WPS on' : 'no WPS'}'),
      if (d.vend.isNotEmpty) KV('Vendor tags', d.vend.join(', '), monoValue: true),
      if (d.mfr != null) KV('Company', '0x${d.mfr!.toRadixString(16).padLeft(4, '0').toUpperCase()} ${Lookup.I.companyName(d.mfr!) ?? ''}'.trim()),
      if (d.app != null && d.app != 0) KV('Appearance', Lookup.I.appearanceName(d.app!) ?? '${d.app}'),
      if (d.tx != null) KV('TX power', '${d.tx} dBm'),
      if (d.fl != null && d.fl != 0)
        KV(
          'Flags',
          [
            for (final (b, n) in _flags)
              if (d.fl! & b != 0) n,
          ].join(', '),
        ),
      if (d.svc.isNotEmpty) KV('Services', [for (final u in d.svc) '$u ${Lookup.I.uuidName(int.tryParse(u, radix: 16) ?? -1) ?? ''}'.trim()].join('\n')),
      KV('Best signal', d.rssi == null ? '-' : '${d.rssi} dBm', color: C.rssi(d.rssi)),
      KV(
        'Position',
        d.placed ? '${d.lat!.toStringAsFixed(6)}, ${d.lon!.toStringAsFixed(6)}${d.acc != null ? ' ±${d.acc!.round()} m' : ''}' : 'not placed (no fix)',
        monoValue: d.placed,
      ),
      KV('First seen', clock(d.first, date: true)),
      KV('Last seen', '${clock(d.last, date: true)} (${agoText(d.last)})'),
      KV('Sightings', '${d.seen} in ${d.sessions.length} session${d.sessions.length == 1 ? '' : 's'}'),
      if (d.node.isNotEmpty) KV('Heard by', d.node, monoValue: true),
    ];
    return ListView(
      controller: sc,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Row(
            children: [
              IconTile(wdIcon(d), c, size: 44),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(d.label, maxLines: 2, overflow: TextOverflow.ellipsis, style: T.h2),
                    const SizedBox(height: 4),
                    Row(
                      children: [
                        Tag(_secLabel(d), color: c),
                        const SizedBox(width: 6),
                        Text(d.mac, style: T.code),
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
        _WdActions(d: d),
        Group(children: rows),
        FutureBuilder<List<WdObs>>(
          future: w.sightings(d),
          builder: (bc, snap) {
            final ob = snap.data ?? const <WdObs>[];
            if (ob.isEmpty && !d.placed) return const SizedBox.shrink();
            final pts = [for (final o in ob.take(300)) LatLng(o.lat, o.lon)];
            final center = d.placed ? LatLng(d.lat!, d.lon!) : pts.first;
            return Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                SectionHead('Where it was heard', trailing: Text('${ob.length}', style: T.code)),
                Panel(
                  padding: EdgeInsets.zero,
                  margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                  child: SizedBox(
                    height: 220,
                    child: MiniMap(
                      center: center,
                      zoom: 17,
                      layers: [
                        CircleLayer(
                          circles: [
                            for (final o in ob.take(300).toList().reversed)
                              CircleMarker(point: LatLng(o.lat, o.lon), radius: 4, color: C.rssi(o.rssi).withValues(alpha: 0.75)),
                            if (d.placed) CircleMarker(point: center, radius: 7, color: c, borderColor: C.bg, borderStrokeWidth: 2),
                          ],
                        ),
                      ],
                    ),
                  ),
                ),
                if (ob.isNotEmpty)
                  Group(
                    children: [
                      for (final o in ob.take(20))
                        Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 9),
                          child: Row(
                            children: [
                              Text(clock(o.ts, date: true), style: T.code),
                              const SizedBox(width: 10),
                              Expanded(
                                child: Text('${o.lat.toStringAsFixed(5)}, ${o.lon.toStringAsFixed(5)}', style: T.code.copyWith(color: C.soft)),
                              ),
                              Text(
                                '${o.rssi ?? '-'}',
                                style: T.code.copyWith(color: C.rssi(o.rssi), fontWeight: FontWeight.w600),
                              ),
                            ],
                          ),
                        ),
                    ],
                  ),
              ],
            );
          },
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
          child: Btn('Open device page', icon: Ph.arrowSquareOut, kind: BtnKind.ghost, expand: true, onTap: () => openDevice(context, d.mac)),
        ),
      ],
    );
  }
}

/// The Tab5's action bar on a wardriven device: NAME (an alias, every screen shows it) and
/// ADD AS DETECTION, a filter on one node or all of them so they alert on it. The
/// detectors match BLE by full MAC, Wi-Fi only by maker prefix (beacon OUI), so a Wi-Fi
/// device's filter catches every AP of that maker; the bar says so.
class _WdActions extends StatefulWidget {
  const _WdActions({required this.d});
  final WdDevice d;
  @override
  State<_WdActions> createState() => _WdActionsState();
}

class _WdActionsState extends State<_WdActions> {
  String node = 'all';

  Future<void> _name() async {
    final m = App.I.m;
    final v = await askText(context, 'Name it', m.aliasOf(widget.d.mac), hint: 'blank clears it');
    if (v == null) return;
    await App.I.link.setAlias(widget.d.mac, v);
  }

  Future<void> _add() async {
    final m = App.I.m;
    final d = widget.d;
    final ble = d.kind == 'ble';
    final hex = d.mac.replaceAll(RegExp('[^0-9A-Fa-f]'), '').toUpperCase();
    if (hex.length != 12) return;
    final al = m.aliasOf(d.mac);
    var desc = al.isNotEmpty ? al : (d.name.isNotEmpty ? d.name : d.mac);
    if (desc.length > 40) desc = desc.substring(0, 40);
    final args = {
      'filters': [
        {'t': ble ? 'MAC' : 'BEACON', 'id': ble ? hex : hex.substring(0, 6), 'd': desc},
      ],
    };
    final b = App.I.link;
    // a node on USB takes a filter for one node at a time: "all" is each detector in turn
    final ids = node == 'all' && b is LocalMesh
        ? [
            for (final n in m.nodes.values)
              if (n.kind == 'detector' && n.online) n.id,
          ]
        : [node];
    if (ids.isEmpty) return m.toast('no detector online to add it to');
    var ok = 0;
    for (final id in ids) {
      if (await b.nodeCmd(id, 'filters_add', args) != null) ok++;
    }
    if (ok > 0) m.toast('$desc: ${ble ? 'MAC' : 'Wi-Fi maker'} ${ble ? hex : hex.substring(0, 6)} added on ${node == 'all' ? 'all nodes' : m.nodeName(node)}');
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final d = widget.d;
    final can = d.kind == 'ble' || d.isWifi;
    return ListenableBuilder(
      listenable: m,
      builder: (_, _) {
        final al = m.aliasOf(d.mac);
        final ns = m.nodes.values.toList()..sort((a, b) => a.label.toLowerCase().compareTo(b.label.toLowerCase()));
        if (node != 'all' && !m.nodes.containsKey(node)) node = 'all';
        return Group(
          children: [
            Tile(
              icon: Ph.pencilSimple,
              color: al.isEmpty ? C.muted : C.accent,
              title: al.isEmpty ? 'Name it' : al,
              titleColor: al.isEmpty ? C.soft : null,
              subtitle: Text(al.isEmpty ? 'a name every screen shows for it' : 'its name: tap to change', style: T.meta),
              onTap: _name,
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    !can
                        ? 'THE DETECTORS CAN ONLY ALERT ON BLE AND WI-FI'
                        : d.kind == 'ble'
                        ? 'ALERT ON THIS DEVICE (BLE MAC)'
                        : "ALERT ON THIS MAKER'S APS (WI-FI OUI)",
                    style: T.silk,
                  ),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      PickChip<String>(
                        label: 'On',
                        icon: Ph.graph,
                        value: node,
                        lit: node != 'all',
                        items: [('all', 'All nodes'), for (final n in ns) (n.id, n.label)],
                        dots: (v) => v == 'all' ? null : C.hue(m.nodes[v]?.hue),
                        onChanged: (v) => setState(() => node = v),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn('Add as detection', icon: Ph.plusCircle, expand: true, onTap: can ? _add : null),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}

/// Writes the WiGLE CSV (one session, or all) and shows where; with [upload], sends it to
/// the user's WiGLE account after a confirm.
Future<void> exportWigle(BuildContext context, {String? session, bool upload = false}) async {
  final w = Wardrive.I;
  if (upload && !await w.hasWigle()) {
    if (!context.mounted) return;
    await wigleAccountSheet(context);
    if (!await w.hasWigle()) return;
  }
  final String path;
  try {
    path = await w.exportWigle(session: session);
  } catch (e) {
    if (context.mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text("couldn't write the CSV")));
    return;
  }
  if (!context.mounted) return;
  if (!upload) {
    await showDialog<void>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('WiGLE CSV written'),
        content: SelectableText(path, style: T.code.copyWith(color: C.soft)),
        actions: [
          TextButton(
            onPressed: () {
              Clipboard.setData(ClipboardData(text: path));
              Navigator.pop(c);
              ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('path copied')));
            },
            child: const Text('Copy path', style: TextStyle(color: C.muted)),
          ),
          TextButton(
            onPressed: () {
              Navigator.pop(c);
              exportWigleUpload(context, path);
            },
            child: const Text(
              'Upload to WiGLE',
              style: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
            ),
          ),
        ],
      ),
    );
    return;
  }
  await exportWigleUpload(context, path);
}

Future<void> exportWigleUpload(BuildContext context, String path) async {
  final w = Wardrive.I;
  if (!await w.hasWigle()) {
    if (!context.mounted) return;
    await wigleAccountSheet(context);
    if (!await w.hasWigle()) return;
  }
  final name = await w.wigleName();
  if (!context.mounted) return;
  if (!await confirm(context, 'Upload to WiGLE?', '${path.split('/').last} goes to the WiGLE account "$name".', yes: 'Upload', color: C.accent)) return;
  if (!context.mounted) return;
  ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('uploading to WiGLE…')));
  final r = await w.uploadWigle(path);
  if (!context.mounted) return;
  await showDialog<void>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(r.ok ? 'Uploaded' : 'Upload failed'),
      content: Text(r.message, style: T.body),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(c),
          child: const Text('OK', style: TextStyle(color: C.accent)),
        ),
      ],
    ),
  );
}

/// The user's own WiGLE API name and token, kept on the phone only.
Future<void> wigleAccountSheet(BuildContext context) async {
  final w = Wardrive.I;
  final name = TextEditingController(text: await w.wigleName());
  final token = TextEditingController();
  final has = await w.hasWigle();
  if (!context.mounted) return;
  await showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    builder: (c) => Padding(
      padding: EdgeInsets.fromLTRB(16, 0, 16, MediaQuery.viewInsetsOf(c).bottom + 20),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text('WIGLE ACCOUNT', style: T.label),
          const SizedBox(height: 8),
          Text(
            'Your own API name and token, from wigle.net: Account, then API Token ("Encoded for use" is not needed). They stay on this phone and are only sent to api.wigle.net.',
            style: T.meta.copyWith(fontSize: 13),
          ),
          const SizedBox(height: 14),
          TextField(
            controller: name,
            style: T.row,
            autocorrect: false,
            decoration: const InputDecoration(labelText: 'API name', hintText: 'AID…'),
          ),
          const SizedBox(height: 10),
          TextField(
            controller: token,
            style: T.row,
            obscureText: true,
            autocorrect: false,
            enableSuggestions: false,
            decoration: InputDecoration(labelText: 'API token', hintText: has ? 'saved (type to replace)' : ''),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              if (has)
                Expanded(
                  child: Btn(
                    'Forget',
                    icon: Ph.trash,
                    color: C.red,
                    kind: BtnKind.ghost,
                    expand: true,
                    onTap: () async {
                      await w.setWigle('', '');
                      if (c.mounted) Navigator.pop(c);
                    },
                  ),
                ),
              if (has) const SizedBox(width: 10),
              Expanded(
                child: Btn(
                  'Save',
                  icon: Ph.check,
                  kind: BtnKind.filled,
                  expand: true,
                  onTap: () async {
                    if (name.text.trim().isEmpty || (token.text.trim().isEmpty && !has)) return;
                    if (token.text.trim().isNotEmpty) {
                      await w.setWigle(name.text, token.text);
                    } else {
                      await w.renameWigle(name.text);
                    }
                    if (c.mounted) Navigator.pop(c);
                  },
                ),
              ),
            ],
          ),
        ],
      ),
    ),
  );
}
