// METRICS: the dashboard's Metrics tab. A range (1H to 30D), a node and a domain filter,
// and three modes: Detections (over time by vendor with the unique-device line, by vendor,
// node and type, signal histogram, weekday x hour heat), Wardrive (discovered over time,
// security, channels, makers, nodes) and Drones (reports over time, by node, each drone).
//
// With the home dashboard: GET /api/metrics?range=&node=&dom= (store.py metrics(), with its
// wd and drones sections). With the app as the dashboard (a node on USB) the same keys are
// worked out on the phone from MeshStore and Wardrive (localMetrics).
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:intl/intl.dart' show DateFormat;

import '../data/dashboard.dart';
import '../data/model.dart';
import '../data/store.dart';
import '../data/wardrive.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'wardrive_view.dart' show secColor;

int _i(Object? v) => v is num ? v.toInt() : 0;
double? _d(Object? v) => v is num ? v.toDouble() : null;
List<dynamic> _l(Object? v) => v is List ? v : const [];
Map<String, dynamic> _m(Object? v) => v is Map ? v.cast<String, dynamic>() : const {};
List<num> _nums(Object? v) => [for (final x in _l(v)) x is num ? x : 0];

const _ranges = [(3600, '1H'), (21600, '6H'), (86400, '24H'), (604800, '7D'), (2592000, '30D')];
const _palette = [C.accent, C.violet, C.amber, C.hot, C.sky, C.lime];

enum MetricsMode { detections, wardrive, drones }

// ------------------------------------------------------------------ the phone's own numbers

String _vendorOf(String descr) {
  final v = descr.trim().replaceFirst(RegExp(r'\s*\(.*?\)\s*$'), '');
  return v.isEmpty ? 'Unknown' : v;
}

/// /api/metrics worked out from what the app keeps when it is the dashboard: the latest
/// detections, every device's first sighting, drone reports and the wardrive devices.
Map<String, dynamic> localMetrics(int range, {String? node, String? dom, double? now}) {
  final st = MeshStore.I;
  final until = now ?? DateTime.now().millisecondsSinceEpoch / 1000;
  final since = until - range;
  final span = math.max(60.0, until - since);
  const steps = [60, 300, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400];
  final bucket = steps.firstWhere((b) => span / b <= 120, orElse: () => 86400);
  final start = since - since % bucket;
  final nb = ((until - start) ~/ bucket) + 1;
  bool keep(Map<String, dynamic> r) => (node == null || r['node'] == node) && (dom == null || r['dom'] == dom);

  final rows = [
    for (final r in st.dets)
      if (((r['ts'] as num?) ?? 0) >= since && ((r['ts'] as num?) ?? 0) <= until && keep(r)) r,
  ];
  final prevTotal = st.dets.where((r) {
    final t = (r['ts'] as num?) ?? 0;
    return t >= since - span && t < since && keep(r);
  }).length;

  final perVendor = <String, (int, Set<String>)>{}, perNode = <String, (int, Set<String>)>{};
  final perType = <String, int>{}, perDom = <String, int>{'ble': 0, 'wifi': 0};
  final bucketV = <String, List<int>>{};
  final bucketMacs = List.generate(nb, (_) => <String>{});
  final heat = List.generate(7, (_) => List.filled(24, 0));
  final hist = List.filled(17, 0); // -100 .. -20 in 5s
  for (final r in rows) {
    final mac = '${r['mac']}', v = _vendorOf('${r['descr'] ?? ''}'), n = '${r['node'] ?? ''}', ts = (r['ts'] as num).toDouble();
    final pv = perVendor[v] ?? (0, <String>{});
    perVendor[v] = (pv.$1 + 1, pv.$2..add(mac));
    final pn = perNode[n] ?? (0, <String>{});
    perNode[n] = (pn.$1 + 1, pn.$2..add(mac));
    final t = '${r['type'] ?? ''}'.isEmpty ? '?' : '${r['type']}';
    perType[t] = (perType[t] ?? 0) + 1;
    if (perDom.containsKey(r['dom'])) perDom[r['dom']] = perDom[r['dom']]! + 1;
    final i = math.min(nb - 1, math.max(0, (ts - start) ~/ bucket));
    bucketV.putIfAbsent(v, () => List.filled(nb, 0))[i]++;
    bucketMacs[i].add(mac);
    final lt = DateTime.fromMillisecondsSinceEpoch((ts * 1000).round());
    heat[lt.weekday - 1][lt.hour]++;
    final rs = (r['rssi'] as num?)?.toInt();
    if (rs != null) hist[math.min(hist.length - 1, math.max(0, (rs + 100) ~/ 5))]++;
  }
  final ranked = perVendor.entries.toList()..sort((a, b) => b.value.$1.compareTo(a.value.$1));
  final series = <Map<String, dynamic>>[
    for (final e in ranked.take(5)) {'name': e.key, 'values': bucketV[e.key]},
  ];
  if (ranked.length > 5) {
    final rest = ranked.skip(5).map((e) => e.key).toList();
    series.add({
      'name': 'Other',
      'values': [for (var i = 0; i < nb; i++) rest.fold<int>(0, (a, v) => a + bucketV[v]![i])],
    });
  }
  final totals = [for (var i = 0; i < nb; i++) series.fold<int>(0, (a, s) => a + (s['values'] as List<int>)[i])];
  var peakI = 0;
  for (var i = 0; i < nb; i++) {
    if (totals[i] > totals[peakI]) peakI = i;
  }
  final macs = {for (final r in rows) '${r['mac']}'};
  final nodes = perNode.entries.toList()..sort((a, b) => b.value.$1.compareTo(a.value.$1));
  final types = perType.entries.toList()..sort((a, b) => b.value.compareTo(a.value));

  // drones (drones.py metrics), on the same buckets
  final droneEnd = start + bucket * nb;
  final dSeries = List.filled(nb, 0);
  final dSeen = List.generate(nb, (_) => <String>{});
  final dByNode = <String, Set<String>>{};
  var dTotal = 0;
  double? maxAlt, maxHagl, maxSpd;
  final drones = <Map<String, dynamic>>[];
  for (final e in st.droneReports.entries) {
    var n = 0;
    double first = double.infinity, last = 0;
    double? dAlt, dSpd;
    Map<String, dynamic>? lastR;
    for (final r in e.value) {
      final ts = ((r['ts'] as num?) ?? 0).toDouble();
      if (ts < start || ts >= droneEnd) continue;
      if (node != null && r['node'] != node) continue;
      n++;
      final i = math.min(nb - 1, math.max(0, (ts - start) ~/ bucket));
      dSeries[i]++;
      dSeen[i].add(e.key);
      dByNode.putIfAbsent('${r['node'] ?? '?'}', () => {}).add(e.key);
      first = math.min(first, ts);
      if (ts >= last) {
        last = ts;
        lastR = r;
      }
      double? mx(double? a, Object? b) => b is num ? math.max(a ?? b.toDouble(), b.toDouble()) : a;
      dAlt = mx(dAlt, r['alt']);
      dSpd = mx(dSpd, r['spd']);
      maxHagl = mx(maxHagl, r['hagl']);
    }
    if (n == 0) continue;
    dTotal += n;
    if (dAlt != null) maxAlt = math.max(maxAlt ?? dAlt, dAlt);
    if (dSpd != null) maxSpd = math.max(maxSpd ?? dSpd, dSpd);
    final meta = st.droneMeta[e.key] ?? const {};
    final dl = st.droneLast[e.key] ?? const {};
    drones.add({
      'mac': e.key,
      'n': n,
      'first': first,
      'last': last,
      'max_alt': dAlt,
      'max_spd': dSpd,
      'alias': meta['alias'] ?? '',
      'tag': meta['tag'] ?? 'unknown',
      'live': until - last < droneLiveS,
      'node': lastR?['node'],
      'alt': lastR?['alt'],
      'spd': lastR?['spd'],
      'rssi': dl['rssi'],
      'id': dl['id'] ?? '',
    });
  }
  drones.sort((a, b) => (b['last'] as double).compareTo(a['last'] as double));
  final dNodes = dByNode.entries.toList()..sort((a, b) => b.value.length.compareTo(a.value.length));
  final rssis = [
    for (final d in drones)
      if (d['rssi'] is num) (d['rssi'] as num).toInt(),
  ];

  return {
    'since': since,
    'until': until,
    'bucket': bucket,
    'start': start,
    'n': nb,
    'total': rows.length,
    'prev_total': prevTotal,
    'unique': macs.length,
    'new_devices': macs.where((m) => (((st.devices[m]?['first']) as num?) ?? 0) >= since).length,
    'nodes_active': perNode.length,
    'peak': {'count': nb > 0 ? totals[peakI] : 0, 'ts': start + peakI * bucket},
    'series': series,
    'unique_series': [for (final b in bucketMacs) b.length],
    'by_vendor': [
      for (final e in ranked.take(10)) {'name': e.key, 'count': e.value.$1, 'devices': e.value.$2.length},
    ],
    'by_node': [
      for (final e in nodes) {'id': e.key, 'count': e.value.$1, 'devices': e.value.$2.length},
    ],
    'by_type': [
      for (final e in types) {'type': e.key, 'count': e.value},
    ],
    'by_dom': perDom,
    'heat': heat,
    'rssi': {'from': -100, 'step': 5, 'counts': hist},
    'wd': Wardrive.I.metrics(since, until, node: node, dom: dom, bucket: bucket, start: start.toDouble(), nb: nb),
    'drones': {
      'total': dTotal,
      'unique': drones.length,
      'live': drones.where((d) => d['live'] == true).length,
      'max_alt': maxAlt,
      'max_hagl': maxHagl,
      'max_spd': maxSpd,
      'rssi_best': rssis.isEmpty ? null : rssis.reduce(math.max),
      'series': dSeries,
      'unique_series': [for (final s in dSeen) s.length],
      'by_node': [
        for (final e in dNodes.take(8)) [e.key, e.value.length],
      ],
      'drones': drones.take(12).toList(),
    },
  };
}

// ------------------------------------------------------------------ the view

class MetricsView extends StatefulWidget {
  const MetricsView({super.key, this.mode = MetricsMode.detections});
  final MetricsMode mode;
  @override
  State<MetricsView> createState() => _MetricsViewState();
}

class _MetricsViewState extends State<MetricsView> {
  int range = 86400;
  String? node, dom;
  late MetricsMode mode = widget.mode;
  Map<String, dynamic>? data;
  bool loading = false;
  String error = '';
  Timer? _t;

  @override
  void initState() {
    super.initState();
    _load();
    _t = Timer.periodic(const Duration(seconds: 30), (_) => _load(quiet: true));
  }

  @override
  void dispose() {
    _t?.cancel();
    super.dispose();
  }

  Future<void> _load({bool quiet = false}) async {
    if (loading) return;
    if (!quiet) setState(() => loading = true);
    Map<String, dynamic>? out;
    var err = '';
    final link = App.I.link;
    if (link is Dashboard) {
      final p = StringBuffer('/api/metrics?range=$range');
      if (node != null) p.write('&node=${Uri.encodeQueryComponent(node!)}');
      if (dom != null) p.write('&dom=$dom');
      final j = await link.getJson(p.toString(), timeout: const Duration(seconds: 30));
      if (j is Map) {
        out = j.cast<String, dynamic>();
      } else {
        err = "the dashboard didn't answer /api/metrics";
      }
    } else {
      try {
        out = localMetrics(range, node: node, dom: dom);
      } catch (e) {
        err = 'could not work out the metrics';
      }
    }
    if (!mounted) return;
    setState(() {
      loading = false;
      if (out != null) data = out;
      error = err;
    });
  }

  void _set(VoidCallback f) {
    setState(f);
    _load();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final d = data;
    final nodeIds = <String>{...m.nodes.keys, for (final n in _l(d?['by_node'])) '${_m(n)['id']}'}..removeWhere((e) => e.isEmpty);
    return Scaffold(
      appBar: AppBar(
        title: Text('METRICS', style: T.label.copyWith(fontSize: 13, color: C.text)),
        actions: [
          if (loading)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)),
            ),
          IconButton(icon: const Icon(Ph.arrowClockwise, size: 20), onPressed: _load),
        ],
      ),
      body: RefreshIndicator(
        color: C.accent,
        onRefresh: _load,
        child: ListView(
          padding: const EdgeInsets.only(bottom: 28),
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
              child: Seg<MetricsMode>(
                items: const [(MetricsMode.detections, 'Detections'), (MetricsMode.wardrive, 'Wardrive'), (MetricsMode.drones, 'Drones')],
                value: mode,
                onChanged: (v) => setState(() => mode = v),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 0),
              child: Seg<int>(items: _ranges, value: range, onChanged: (v) => _set(() => range = v)),
            ),
            ChipRow(
              children: [
                Chip2('All nodes', on: node == null, onTap: () => _set(() => node = null)),
                for (final id in nodeIds.toList()..sort((a, b) => m.nodeName(a).compareTo(m.nodeName(b))))
                  Chip2(m.nodeName(id), on: node == id, color: C.hue(m.nodes[id]?.hue), onTap: () => _set(() => node = node == id ? null : id)),
              ],
            ),
            if (mode != MetricsMode.drones)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
                child: Seg<String?>(items: const [(null, 'All'), ('ble', 'BLE'), ('wifi', 'WiFi')], value: dom, onChanged: (v) => _set(() => dom = v)),
              ),
            if (error.isNotEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(18, 8, 18, 0),
                child: Text(error, style: T.meta.copyWith(color: C.amber)),
              ),
            if (d == null)
              Empty(loading ? 'Working it out' : 'No metrics', icon: Ph.chartBar)
            else
              ...switch (mode) {
                MetricsMode.detections => _detections(d),
                MetricsMode.wardrive => _wardrive(d, _m(d['wd'])),
                MetricsMode.drones => _drones(d, _m(d['drones'])),
              },
          ],
        ),
      ),
    );
  }

  // ---- detections

  List<Widget> _detections(Map<String, dynamic> d) {
    final m = App.I.m;
    final total = _i(d['total']), prev = _i(d['prev_total']);
    final delta = prev == 0 ? null : ((total - prev) * 100 / prev).round();
    final peak = _m(d['peak']);
    final byDom = _m(d['by_dom']);
    final series = [for (final s in _l(d['series'])) _m(s)];
    final rssi = _m(d['rssi']);
    final rFrom = _i(rssi['from']), rStep = math.max(1, _i(rssi['step']));
    final counts = _nums(rssi['counts']);
    return [
      _KpiPanel([
        ('Detections', '$total', C.text, delta == null ? null : '${delta >= 0 ? '+' : ''}$delta% vs before'),
        ('Unique', '${_i(d['unique'])}', C.accent, null),
        ('New', '${_i(d['new_devices'])}', C.green, 'first seen'),
        ('Nodes', '${_i(d['nodes_active'])}', C.text, 'active'),
        ('Peak', '${_i(peak['count'])}', C.amber, _d(peak['ts']) == null ? null : _when(_d(peak['ts'])!, _i(d['bucket']))),
        ('BLE / WiFi', '${_i(byDom['ble'])} / ${_i(byDom['wifi'])}', C.text, null),
      ]),
      SectionHead('Over time', trailing: Text('per ${_bucketLabel(_i(d['bucket']))}', style: T.code)),
      _ChartPanel(
        chart: _TimeChart(
          stacks: [for (final (i, s) in series.indexed) (s['name'] == 'Other' ? C.faint : _palette[i % _palette.length], _nums(s['values']))],
          line: _nums(d['unique_series']),
          lineColor: C.text,
        ),
        footer: _TimeAxis(start: _d(d['start']) ?? 0, bucket: _i(d['bucket']), n: _i(d['n'])),
        legend: [for (final (i, s) in series.indexed) ('${s['name']}', s['name'] == 'Other' ? C.faint : _palette[i % _palette.length]), ('unique devices', C.text)],
      ),
      SectionHead('By vendor'),
      _HBars([for (final v in _l(d['by_vendor'])) ('${_m(v)['name']}', _i(_m(v)['count']), '${_i(_m(v)['devices'])} dev', C.accent)]),
      SectionHead('By node'),
      _HBars([
        for (final v in _l(d['by_node']))
          (m.nodeName('${_m(v)['id']}'), _i(_m(v)['count']), '${_i(_m(v)['devices'])} dev', C.hue(m.nodes['${_m(v)['id']}']?.hue, _l(d['by_node']).indexOf(v))),
      ]),
      SectionHead('By type'),
      _HBars([for (final v in _l(d['by_type'])) ('${_m(v)['type']}', _i(_m(v)['count']), null, C.violet)]),
      SectionHead('Signal', trailing: Text('dBm', style: T.code)),
      _ChartPanel(
        chart: _Bars(values: counts, colors: [for (var i = 0; i < counts.length; i++) C.rssi(rFrom + i * rStep + rStep ~/ 2)]),
        footer: _Labels([for (var i = 0; i < counts.length; i += 4) '${rFrom + i * rStep}']),
      ),
      SectionHead('When', trailing: Text('weekday × hour', style: T.code)),
      _ChartPanel(chart: _Heat([for (final r in _l(d['heat'])) _nums(r)]), height: 170),
    ];
  }

  // ---- wardrive

  List<Widget> _wardrive(Map<String, dynamic> d, Map<String, dynamic> wd) {
    final m = App.I.m;
    final ns = _m(wd['new_series']);
    Color kc(String k) => k == 'ble' ? C.hot : (k == 'wifi' ? C.sky : C.lime);
    final chs = [for (final c in _l(wd['by_ch'])) _m(c)];
    if (_i(wd['total']) == 0) {
      return [const Empty('No wardriving in this range', body: 'Devices a wardriver node heard show up here.', icon: Ph.carProfile)];
    }
    return [
      _KpiPanel([
        ('Devices', '${_i(wd['total'])}', C.text, null),
        ('New', '${_i(wd['new'])}', C.green, 'first seen'),
        ('Open', '${_i(wd['open'])}', C.green, 'networks'),
        ('WiFi', '${_i(wd['wifi'])}', C.sky, null),
        ('BLE', '${_i(wd['ble'])}', C.hot, null),
        ('Placed', '${_i(wd['placed'])}', C.text, '${_i(wd['nodes'])} nodes'),
      ]),
      SectionHead('Discovered over time', trailing: Text('per ${_bucketLabel(_i(d['bucket']))}', style: T.code)),
      _ChartPanel(
        chart: _TimeChart(stacks: [for (final e in ns.entries) (kc(e.key), _nums(e.value))]),
        footer: _TimeAxis(start: _d(d['start']) ?? 0, bucket: _i(d['bucket']), n: _i(d['n'])),
        legend: [for (final k in ns.keys) (k.toUpperCase(), kc(k))],
      ),
      SectionHead('Security'),
      _HBars([for (final v in _l(wd['by_sec'])) ('${_m(v)['name']}', _i(_m(v)['count']), null, secColor('${_m(v)['name']}'))]),
      if (chs.isNotEmpty) ...[
        SectionHead('Channels'),
        _ChartPanel(
          chart: _Bars(values: [for (final c in chs) _i(c['count'])], colors: [for (final c in chs) _i(c['ch']) > 14 ? C.violet : C.sky]),
          footer: _Labels([for (final c in chs) '${_i(c['ch'])}'], spread: true),
        ),
      ],
      SectionHead('Makers'),
      _HBars([for (final v in _l(wd['by_maker'])) ('${_m(v)['name']}', _i(_m(v)['count']), null, C.accent)]),
      if (_l(wd['by_node']).isNotEmpty) ...[
        SectionHead('By node'),
        _HBars([for (final (i, v) in _l(wd['by_node']).indexed) (m.nodeName('${_m(v)['id']}'), _i(_m(v)['count']), null, C.hue(m.nodes['${_m(v)['id']}']?.hue, i))]),
      ],
    ];
  }

  // ---- drones

  List<Widget> _drones(Map<String, dynamic> d, Map<String, dynamic> dr) {
    final m = App.I.m;
    if (_i(dr['total']) == 0) {
      return [const Empty('No drones in this range', body: 'Remote ID drones a node heard show up here.', icon: Ph.drone)];
    }
    String f(Object? v, String unit) => _d(v) == null ? '-' : '${_d(v)!.round()} $unit';
    return [
      _KpiPanel([
        ('Reports', '${_i(dr['total'])}', C.text, null),
        ('Drones', '${_i(dr['unique'])}', C.hot, null),
        ('Live', '${_i(dr['live'])}', _i(dr['live']) > 0 ? C.hot : C.text, null),
        ('Max alt', f(dr['max_alt'], 'm'), C.text, dr['max_hagl'] == null ? null : 'AGL ${f(dr['max_hagl'], 'm')}'),
        ('Max speed', f(dr['max_spd'], 'm/s'), C.text, null),
        ('Best RSSI', dr['rssi_best'] == null ? '-' : '${_i(dr['rssi_best'])}', C.rssi(dr['rssi_best'] == null ? null : _i(dr['rssi_best'])), null),
      ]),
      SectionHead('Over time', trailing: Text('per ${_bucketLabel(_i(d['bucket']))}', style: T.code)),
      _ChartPanel(
        chart: _TimeChart(stacks: [(C.hot, _nums(dr['series']))], line: _nums(dr['unique_series']), lineColor: C.text),
        footer: _TimeAxis(start: _d(d['start']) ?? 0, bucket: _i(d['bucket']), n: _i(d['n'])),
        legend: const [('reports', C.hot), ('drones', C.text)],
      ),
      SectionHead('By node'),
      _HBars([
        for (final (i, v) in _l(dr['by_node']).indexed)
          if (v is List && v.length >= 2) (m.nodeName('${v[0]}'), _i(v[1]), 'drones', C.hue(m.nodes['${v[0]}']?.hue, i)),
      ]),
      SectionHead('Drones'),
      Group(
        children: [
          for (final x in _l(dr['drones']).map(_m))
            Tile(
              icon: x['live'] == true ? PhF.drone : Ph.drone,
              color: x['live'] == true ? C.hot : C.droneTag('${x['tag']}'),
              title: '${x['alias'] ?? ''}'.isNotEmpty ? '${x['alias']}' : ('${x['id'] ?? ''}'.isNotEmpty ? '${x['id']}' : '${x['mac']}'),
              subtitle: Text(
                [
                  '${_i(x['n'])} reports',
                  if (_d(x['max_alt']) != null) 'max ${_d(x['max_alt'])!.round()} m',
                  if (_d(x['max_spd']) != null) '${_d(x['max_spd'])!.round()} m/s',
                  if (x['node'] != null) m.nodeName('${x['node']}'),
                ].join(' · '),
                style: T.meta,
              ),
              trailing: Text(_d(x['last']) == null ? '' : ago(_d(x['last'])!), style: T.code),
            ),
        ],
      ),
    ];
  }
}

String _bucketLabel(int b) {
  if (b < 3600) return '${b ~/ 60} min';
  if (b < 86400) return b == 3600 ? 'hour' : '${b ~/ 3600} h';
  return 'day';
}

String _when(double ts, int bucket) {
  final t = DateTime.fromMillisecondsSinceEpoch((ts * 1000).round());
  return DateFormat(bucket >= 86400 ? 'MMM d' : (bucket >= 3600 ? 'EEE HH:mm' : 'HH:mm')).format(t);
}

// ------------------------------------------------------------------ pieces

class _KpiPanel extends StatelessWidget {
  const _KpiPanel(this.items);
  final List<(String, String, Color, String?)> items;
  @override
  Widget build(BuildContext context) {
    final rows = <List<(String, String, Color, String?)>>[];
    for (var i = 0; i < items.length; i += 3) {
      rows.add(items.sublist(i, math.min(items.length, i + 3)));
    }
    return Panel(
      padding: EdgeInsets.zero,
      margin: const EdgeInsets.fromLTRB(16, 10, 16, 4),
      child: Column(
        children: [
          for (final (i, r) in rows.indexed) ...[
            if (i > 0) const Divider(color: C.silkLine),
            Row(
              children: [
                for (final (l, v, c, s) in r)
                  Expanded(
                    child: Stat(l, v, color: c, sub: s),
                  ),
                for (var k = r.length; k < 3; k++) const Expanded(child: SizedBox()),
              ],
            ),
          ],
        ],
      ),
    );
  }
}

class _ChartPanel extends StatelessWidget {
  const _ChartPanel({required this.chart, this.footer, this.legend = const [], this.height = 140});
  final Widget chart;
  final Widget? footer;
  final List<(String, Color)> legend;
  final double height;
  @override
  Widget build(BuildContext context) => Panel(
    padding: const EdgeInsets.fromLTRB(12, 14, 12, 10),
    margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SizedBox(height: height, child: chart),
        if (footer != null) ...[const SizedBox(height: 6), footer!],
        if (legend.isNotEmpty) ...[
          const SizedBox(height: 10),
          Wrap(
            spacing: 12,
            runSpacing: 6,
            children: [
              for (final (l, c) in legend)
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Dot(c, size: 7),
                    const SizedBox(width: 5),
                    ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 140),
                      child: Text(l, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.silk.copyWith(fontSize: 10)),
                    ),
                  ],
                ),
            ],
          ),
        ],
      ],
    ),
  );
}

class _TimeAxis extends StatelessWidget {
  const _TimeAxis({required this.start, required this.bucket, required this.n});
  final double start;
  final int bucket, n;
  @override
  Widget build(BuildContext context) {
    if (n <= 0) return const SizedBox.shrink();
    final end = start + bucket * n;
    return _Labels([_when(start, bucket), _when((start + end) / 2, bucket), _when(end, bucket)]);
  }
}

class _Labels extends StatelessWidget {
  const _Labels(this.labels, {this.spread = false});
  final List<String> labels;
  final bool spread; // one per bar, evenly
  @override
  Widget build(BuildContext context) => Row(
    mainAxisAlignment: MainAxisAlignment.spaceBetween,
    children: [
      for (final l in labels)
        spread
            ? Expanded(
                child: Text(l, textAlign: TextAlign.center, maxLines: 1, overflow: TextOverflow.clip, style: T.silk.copyWith(fontSize: 9)),
              )
            : Text(l, style: T.silk.copyWith(fontSize: 9.5)),
    ],
  );
}

/// Horizontal bars: label, bar, count.
class _HBars extends StatelessWidget {
  const _HBars(this.items);
  final List<(String, int, String?, Color)> items;
  @override
  Widget build(BuildContext context) {
    if (items.isEmpty) {
      return Padding(
        padding: const EdgeInsets.fromLTRB(18, 0, 18, 8),
        child: Text('nothing', style: T.meta),
      );
    }
    final mx = items.map((e) => e.$2).fold<int>(1, math.max);
    return Panel(
      padding: const EdgeInsets.fromLTRB(14, 10, 14, 10),
      margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
      child: Column(
        children: [
          for (final (l, n, sub, c) in items)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Row(
                children: [
                  SizedBox(
                    width: 112,
                    child: Text(
                      l,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: T.meta.copyWith(color: C.soft),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: LayoutBuilder(
                      builder: (c2, bc) => Align(
                        alignment: Alignment.centerLeft,
                        child: Container(
                          width: math.max(2, bc.maxWidth * n / mx),
                          height: 9,
                          decoration: BoxDecoration(color: c.withValues(alpha: 0.75), borderRadius: BorderRadius.circular(1)),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  SizedBox(
                    width: 74,
                    child: Text.rich(
                      TextSpan(
                        children: [
                          TextSpan(
                            text: '$n',
                            style: T.code.copyWith(color: C.text, fontWeight: FontWeight.w600),
                          ),
                          if (sub != null) TextSpan(text: ' $sub', style: T.code.copyWith(fontSize: 10)),
                        ],
                      ),
                      textAlign: TextAlign.right,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

/// Stacked bars over time, with an optional line on its own scale.
class _TimeChart extends StatelessWidget {
  const _TimeChart({required this.stacks, this.line, this.lineColor = C.text});
  final List<(Color, List<num>)> stacks;
  final List<num>? line;
  final Color lineColor;
  @override
  Widget build(BuildContext context) => CustomPaint(painter: _TimePainter(stacks, line, lineColor), size: Size.infinite);
}

class _TimePainter extends CustomPainter {
  _TimePainter(this.stacks, this.line, this.lineColor);
  final List<(Color, List<num>)> stacks;
  final List<num>? line;
  final Color lineColor;

  @override
  void paint(Canvas canvas, Size s) {
    final n = stacks.fold<int>(line?.length ?? 0, (a, st) => math.max(a, st.$2.length));
    _grid(canvas, s);
    if (n == 0) return;
    final tot = List<num>.generate(n, (i) => stacks.fold<num>(0, (a, st) => a + (i < st.$2.length ? st.$2[i] : 0)));
    final mx = tot.fold<num>(0, math.max);
    final bw = s.width / n;
    final gap = bw > 4 ? 1.0 : 0.0;
    if (mx > 0) {
      for (var i = 0; i < n; i++) {
        var y = s.height;
        for (final (c, v) in stacks) {
          final x = i < v.length ? v[i] : 0;
          if (x <= 0) continue;
          final h = s.height * x / mx;
          canvas.drawRect(Rect.fromLTWH(i * bw + gap / 2, y - h, math.max(1, bw - gap), h), Paint()..color = c.withValues(alpha: 0.85));
          y -= h;
        }
      }
      _txt(canvas, '$mx', Offset(2, 0));
    }
    final l = line;
    if (l != null && l.isNotEmpty) {
      final lm = l.fold<num>(0, math.max);
      if (lm > 0) {
        final p = Path();
        for (var i = 0; i < l.length; i++) {
          final pt = Offset(i * bw + bw / 2, s.height - s.height * l[i] / lm * 0.95);
          i == 0 ? p.moveTo(pt.dx, pt.dy) : p.lineTo(pt.dx, pt.dy);
        }
        canvas.drawPath(
          p,
          Paint()
            ..color = lineColor.withValues(alpha: 0.85)
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1.5
            ..strokeJoin = StrokeJoin.round,
        );
      }
    }
  }

  @override
  bool shouldRepaint(_TimePainter o) => true;
}

/// Plain bars, one colour each.
class _Bars extends StatelessWidget {
  const _Bars({required this.values, required this.colors});
  final List<num> values;
  final List<Color> colors;
  @override
  Widget build(BuildContext context) => CustomPaint(painter: _BarsPainter(values, colors), size: Size.infinite);
}

class _BarsPainter extends CustomPainter {
  _BarsPainter(this.values, this.colors);
  final List<num> values;
  final List<Color> colors;
  @override
  void paint(Canvas canvas, Size s) {
    _grid(canvas, s);
    if (values.isEmpty) return;
    final mx = values.fold<num>(0, math.max);
    if (mx <= 0) return;
    final bw = s.width / values.length;
    final gap = math.min(4.0, bw * 0.2);
    for (var i = 0; i < values.length; i++) {
      final h = s.height * values[i] / mx;
      canvas.drawRect(Rect.fromLTWH(i * bw + gap / 2, s.height - h, bw - gap, h), Paint()..color = colors[i % colors.length].withValues(alpha: 0.85));
    }
    _txt(canvas, '$mx', const Offset(2, 0));
  }

  @override
  bool shouldRepaint(_BarsPainter o) => true;
}

/// Weekday (Mon first) x hour.
class _Heat extends StatelessWidget {
  const _Heat(this.rows);
  final List<List<num>> rows;
  @override
  Widget build(BuildContext context) => CustomPaint(painter: _HeatPainter(rows), size: Size.infinite);
}

class _HeatPainter extends CustomPainter {
  _HeatPainter(this.rows);
  final List<List<num>> rows;
  static const _days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
  @override
  void paint(Canvas canvas, Size s) {
    const lw = 30.0, fh = 14.0;
    final cw = (s.width - lw) / 24, ch = (s.height - fh) / 7;
    final mx = rows.fold<num>(0, (a, r) => r.fold<num>(a, math.max));
    for (var d = 0; d < 7; d++) {
      _txt(canvas, _days[d], Offset(0, d * ch + ch / 2 - 6));
      for (var h = 0; h < 24; h++) {
        final v = d < rows.length && h < rows[d].length ? rows[d][h] : 0;
        final a = mx <= 0 || v <= 0 ? 0.0 : 0.15 + 0.85 * (v / mx);
        final r = Rect.fromLTWH(lw + h * cw + 0.5, d * ch + 0.5, cw - 1, ch - 1);
        canvas.drawRect(r, Paint()..color = a == 0 ? C.panel2.withValues(alpha: 0.6) : C.accent.withValues(alpha: a));
      }
    }
    for (final h in const [0, 6, 12, 18, 23]) {
      _txt(canvas, '$h', Offset(lw + h * cw, s.height - fh + 2));
    }
  }

  @override
  bool shouldRepaint(_HeatPainter o) => true;
}

void _grid(Canvas canvas, Size s) {
  final p = Paint()
    ..color = C.silkLine
    ..strokeWidth = 1;
  for (var i = 0; i <= 3; i++) {
    final y = s.height * i / 3;
    canvas.drawLine(Offset(0, y), Offset(s.width, y), p);
  }
}

void _txt(Canvas canvas, String t, Offset at) {
  final tp = TextPainter(
    text: TextSpan(text: t, style: T.silk.copyWith(fontSize: 9)),
    textDirection: TextDirection.ltr,
  )..layout();
  tp.paint(canvas, at);
}
