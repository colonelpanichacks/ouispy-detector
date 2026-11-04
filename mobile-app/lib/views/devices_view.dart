// DEVICES: the Tab5's TARGETS (heard in the last 10 min, who hears them) plus the
// dashboard's whole history, searchable. A tap opens the full detail page.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/dashboard.dart';
import '../data/export.dart';
import '../data/lookup.dart';
import '../data/model.dart';
import '../data/wardrive.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'feed_view.dart';
import 'wardrive_view.dart';
import 'watch_view.dart';

class DevicesView extends StatefulWidget {
  const DevicesView({super.key});
  @override
  State<DevicesView> createState() => _DevicesViewState();
}

class _DevicesViewState extends State<DevicesView> {
  bool history = false;
  bool watch = false;
  bool wardrive = false;
  String dom = '';
  String q = '';
  String sort = 'last'; // last | signal | hits | name
  String node = ''; // '', kind:<k>, or a node id
  String group = ''; // '', none, or a group name
  String src = ''; // history: '' both, det, wd
  List<Map<String, dynamic>> rows = [];
  int total = 0;
  bool loading = false;
  Timer? _deb;

  Future<void> _load() async {
    setState(() => loading = true);
    final j = await _fetch();
    if (!mounted) return;
    setState(() {
      loading = false;
      rows = [for (final r in (j?['rows'] as List? ?? const [])) (r as Map).cast<String, dynamic>()];
      total = (j?['total'] as num?)?.toInt() ?? rows.length;
    });
  }

  /// The history: the dashboard's /api/devices with every filter it takes; on USB the
  /// phone's store, plus what the wardriver heard, filtered here.
  Future<Map<String, dynamic>?> _fetch() async {
    final b = App.I.link;
    if (b is Dashboard) {
      final p = StringBuffer('/api/devices?limit=1000');
      if (src.isNotEmpty) p.write('&src=$src');
      if (q.isNotEmpty) p.write('&q=${Uri.encodeQueryComponent(q)}');
      if (dom.isNotEmpty) p.write('&dom=$dom');
      if (node.isNotEmpty) p.write('&node=${Uri.encodeQueryComponent(node)}');
      final j = await b.getJson(p.toString());
      return j is Map ? j.cast<String, dynamic>() : null;
    }
    final out = <Map<String, dynamic>>[];
    if (src != 'wd') {
      final j = await b.history(q: q, dom: dom, limit: 1000);
      out.addAll([for (final r in (j?['rows'] as List? ?? const [])) (r as Map).cast<String, dynamic>()]);
    }
    if (src != 'det') {
      final ql = q.toLowerCase();
      for (final d in Wardrive.I.devices.values) {
        final dd = d.isWifi ? 'wifi' : 'ble';
        if (dom.isNotEmpty && dd != dom) continue;
        if (ql.isNotEmpty && !'${d.mac} ${d.label} ${d.maker} ${App.I.m.aliasOf(d.mac)}'.toLowerCase().contains(ql)) continue;
        out.add({
          'mac': d.mac,
          'dom': dd,
          'descr': d.label,
          'name': '',
          'last': d.last,
          'first': d.first,
          'hits': d.seen,
          'best': d.rssi,
          'nodes': [if (d.node.isNotEmpty) d.node],
          'wd': d.kind,
          'sec': d.sec,
          'ch': d.ch,
        });
      }
    }
    final l = out.where((r) => nodesPass([for (final n in (r['nodes'] as List? ?? const [])) '$n'], node)).toList();
    return {'rows': l, 'total': l.length};
  }

  void _query(String v) {
    q = v.trim();
    _deb?.cancel();
    if (history) {
      _deb = Timer(const Duration(milliseconds: 350), _load);
    } else {
      setState(() {});
    }
  }

  @override
  void dispose() {
    _deb?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
          child: TextField(
            onChanged: _query,
            style: T.row,
            decoration: const InputDecoration(
              prefixIcon: Icon(Ph.magnifyingGlass, color: C.muted, size: 19),
              hintText: 'Search MAC, name, vendor',
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
          child: Seg<int>(
            items: const [(0, 'Now'), (1, 'History'), (2, 'Watch'), (3, 'Wardrive')],
            value: wardrive ? 3 : (watch ? 2 : (history ? 1 : 0)),
            onChanged: (v) {
              setState(() {
                wardrive = v == 3;
                watch = v == 2;
                history = v == 1;
              });
              if (v == 1) _load();
            },
          ),
        ),
        if (wardrive) const Expanded(child: WardriveView()),
        if (watch) const Expanded(child: WatchView()),
        if (!watch && !wardrive) ..._lists(m),
      ],
    );
  }

  /// One compact row of drop-downs: radio, sort, node, group (and on History the source
  /// and the tools).
  Widget _controls() {
    void reload() {
      if (history) _load();
    }

    return ListenableBuilder(
      listenable: App.I.m,
      builder: (_, _) => ChipRow(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 6),
        children: [
          PickChip<String>(
            label: 'Radio',
            value: dom,
            items: const [('', 'All'), ('ble', 'Bluetooth'), ('wifi', 'Wi-Fi')],
            onChanged: (v) {
              setState(() => dom = v);
              reload();
            },
          ),
          PickChip<String>(
            label: 'Sort',
            icon: Ph.sortDescending,
            value: sort,
            lit: false,
            items: const [('last', 'Last heard'), ('signal', 'Signal'), ('hits', 'Hits'), ('name', 'Name')],
            onChanged: (v) => setState(() => sort = v),
          ),
          NodePick(
            value: node,
            onChanged: (v) {
              setState(() => node = v);
              reload();
            },
          ),
          PickChip<String>(
            label: 'Group',
            icon: Ph.folderSimple,
            value: groupFilterItems().any((g) => g.$1 == group) ? group : '',
            items: groupFilterItems(),
            onChanged: (v) => setState(() => group = v),
          ),
          if (history)
            PickChip<String>(
              label: 'Source',
              value: src,
              items: const [('', 'All'), ('det', 'Detected'), ('wd', 'Wardriven')],
              onChanged: (v) {
                setState(() => src = v);
                _load();
              },
            ),
          if (history)
            Chip2(
              'Export',
              icon: Ph.export,
              onTap: () => historyTools(
                context,
                filter: ExportFilter(q: q, node: node, dom: dom),
                node: node,
              ),
            ),
        ],
      ),
    );
  }

  int Function(R, R) _cmp<R>(double Function(R) last, int? Function(R) best, int Function(R) hits, String Function(R) name) => switch (sort) {
    'signal' => (a, b) => (best(b) ?? -200).compareTo(best(a) ?? -200),
    'hits' => (a, b) => hits(b).compareTo(hits(a)),
    'name' => (a, b) => name(a).toLowerCase().compareTo(name(b).toLowerCase()),
    _ => (a, b) => last(b).compareTo(last(a)),
  };

  List<Widget> _lists(Model m) => [
    _controls(),
    Expanded(
      child: history
          ? RefreshIndicator(
              color: C.accent,
              onRefresh: _load,
              child: Builder(
                builder: (context) {
                  final shown = rows.where((r) => groupPasses('${r['mac']}', group)).toList()
                    ..sort(
                      _cmp<Map<String, dynamic>>(
                        (r) => (r['last'] as num?)?.toDouble() ?? 0,
                        (r) => (r['best'] as num?)?.toInt(),
                        (r) => (r['hits'] as num?)?.toInt() ?? 0,
                        (r) => deviceTitle('${r['mac']}', '${r['name'] ?? ''}'.isNotEmpty ? '${r['name']}' : '${r['descr'] ?? ''}'),
                      ),
                    );
                  if (loading && rows.isEmpty) return const Center(child: CircularProgressIndicator());
                  if (shown.isEmpty) {
                    return ListView(children: [linkEmpty(context, 'No matches', 'Nothing in the history matches that.', icon: Ph.magnifyingGlass)]);
                  }
                  return ListView.separated(
                    itemCount: shown.length + 1,
                    separatorBuilder: (_, i) => i == 0 ? const SizedBox() : const Divider(indent: 64),
                    itemBuilder: (_, i) => i == 0
                        ? Padding(
                            padding: const EdgeInsets.fromLTRB(20, 2, 16, 6),
                            child: Text(shown.length == total ? '$total devices' : '${shown.length} of $total devices', style: T.meta),
                          )
                        : _HistRow(shown[i - 1]),
                  );
                },
              ),
            )
          : ListenableBuilder(
              listenable: m,
              builder: (_, _) {
                final ql = q.toLowerCase();
                final list = m.liveTargets.where((t) {
                  if (dom.isNotEmpty && t.dom != dom) return false;
                  if (!nodesPass(t.seen.map((s) => s.node), node)) return false;
                  if (!groupPasses(t.mac, group)) return false;
                  if (ql.isEmpty) return true;
                  return '${t.mac} ${t.name} ${t.descr} ${m.aliasOf(t.mac)} ${Lookup.I.vendor(t.mac) ?? ''}'.toLowerCase().contains(ql);
                }).toList()..sort(_cmp<Target>((t) => t.last, (t) => t.best, (t) => t.hits, (t) => deviceTitle(t.mac, t.descr)));
                if (list.isEmpty) {
                  return linkEmpty(
                    context,
                    'Nothing nearby',
                    node.isNotEmpty || group.isNotEmpty || dom.isNotEmpty
                        ? 'Nothing heard in the last 10 minutes matches the filters.'
                        : 'No device heard in the last 10 minutes. History has everything the mesh has ever heard.',
                    icon: Ph.broadcast,
                  );
                }
                return ListView.builder(padding: const EdgeInsets.only(bottom: 16), itemCount: list.length, itemBuilder: (_, i) => _TargetRow(list[i]));
              },
            ),
    ),
  ];
}

/// The History tools: export detections (CSV / JSON, with the list's filters), the
/// sightings CSV, and clearing detections (the node picked, or all). Writes to the app's
/// documents folder and shows the path.
Future<void> historyTools(BuildContext context, {ExportFilter filter = const ExportFilter(), String node = ''}) async {
  final m = App.I.m;
  final dash = App.I.link is Dashboard;
  final one = node.isNotEmpty && !node.startsWith('kind:');
  final pick = await showModalBottomSheet<String>(
    context: context,
    builder: (c) => SafeArea(
      child: ListView(
        shrinkWrap: true,
        children: [
          const SectionHead('Export'),
          Tile(
            icon: Ph.fileCsv,
            color: C.accent,
            title: 'Detections CSV',
            subtitle: Text(_filterText(filter), style: T.meta),
            onTap: () => Navigator.pop(c, 'csv'),
          ),
          Tile(
            icon: Ph.fileCode,
            color: C.accent,
            title: 'Detections JSON',
            subtitle: Text(_filterText(filter), style: T.meta),
            onTap: () => Navigator.pop(c, 'json'),
          ),
          Tile(
            icon: Ph.fileCsv,
            color: C.accent,
            title: dash ? 'Sightings CSV' : 'Devices CSV',
            subtitle: Text(dash ? 'every node join / leave the dashboard logged' : 'every device this phone has heard', style: T.meta),
            onTap: () => Navigator.pop(c, 'sightings'),
          ),
          const SectionHead('Clear'),
          if (one) Tile(icon: Ph.trash, color: C.red, title: "Clear ${m.nodeName(node)}'s detections", titleColor: C.red, onTap: () => Navigator.pop(c, 'clear-node')),
          Tile(icon: Ph.trash, color: C.red, title: 'Clear all detections', titleColor: C.red, onTap: () => Navigator.pop(c, 'clear-all')),
          const SizedBox(height: 8),
        ],
      ),
    ),
  );
  if (pick == null || !context.mounted) return;
  if (pick.startsWith('clear')) {
    final who = pick == 'clear-node' ? "${m.nodeName(node)}'s detections" : 'every detection';
    if (!await confirm(
      context,
      'Clear $who?',
      'They are deleted ${dash ? 'on the dashboard' : 'from this phone'} for good. Names, notes and groups stay.',
      yes: 'Clear',
    )) {
      return;
    }
    final n = await clearDetections(App.I.link, m, node: pick == 'clear-node' ? node : null);
    m.toast(n == null ? "didn't clear" : '$n detection${n == 1 ? '' : 's'} cleared');
    return;
  }
  m.toast('exporting…');
  try {
    final path = pick == 'sightings'
        ? await exportSightings(App.I.link)
        : await exportDetections(App.I.link, fmt: pick, filter: filter, nodeOk: (id) => nodePasses(id, filter.node));
    if (context.mounted) await showPath(context, 'Export written', path);
  } catch (e) {
    m.toast("couldn't export ($e)");
  }
}

String _filterText(ExportFilter f) {
  final p = [if (f.q.isNotEmpty) '"${f.q}"', if (f.node.isNotEmpty) nodeFilterLabel(f.node), if (f.dom.isNotEmpty) f.dom == 'ble' ? 'Bluetooth' : 'Wi-Fi'];
  return p.isEmpty ? 'everything stored' : 'only ${p.join(', ')}';
}

class _TargetRow extends StatelessWidget {
  const _TargetRow(this.t);
  final Target t;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final live = m.now - t.last < targetLiveS;
    final c = C.dom(t.dom);
    return Panel(
      onTap: () => openDevice(context, t.mac),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              IconTile(domIcon(t.dom), c),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(deviceTitle(t.mac, t.descr), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                    const SizedBox(height: 3),
                    Text(t.mac, style: T.code),
                  ],
                ),
              ),
              Column(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Row(
                    children: [
                      Dot(live ? C.green : C.amber, size: 6, halo: live),
                      const SizedBox(width: 5),
                      Text(
                        live ? 'now' : ago(t.last),
                        style: T.meta.copyWith(color: live ? C.green : C.amber, fontWeight: FontWeight.w600),
                      ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text('${t.hits} hits', style: T.meta.copyWith(fontSize: 11.5)),
                ],
              ),
            ],
          ),
          if (t.seen.isNotEmpty || t.triOk) ...[
            const SizedBox(height: 10),
            Wrap(
              spacing: 6,
              runSpacing: 6,
              children: [
                if (t.triOk) Tag(t.triRough ? 'Rough fix' : 'Located', icon: Ph.crosshair, color: t.triRough ? C.amber : C.green),
                for (final (i, s) in t.seen.indexed)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 3),
                    decoration: BoxDecoration(color: C.panel2, borderRadius: BorderRadius.circular(6)),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Dot(C.hue(m.nodes[s.node]?.hue, i), size: 6),
                        const SizedBox(width: 5),
                        Text(m.nodeName(s.node), style: T.meta.copyWith(fontSize: 11.5, color: C.soft)),
                        const SizedBox(width: 5),
                        Text('${s.rssi}', style: T.code.copyWith(fontSize: 11, color: C.rssi(s.rssi))),
                      ],
                    ),
                  ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}

class _HistRow extends StatelessWidget {
  const _HistRow(this.r);
  final Map<String, dynamic> r;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final mac = '${r['mac']}'.toUpperCase();
    final d = '${r['dom']}';
    final best = (r['best'] as num?)?.toInt();
    final nodes = (r['nodes'] as List? ?? const []).map((n) => m.nodeName('$n')).join(', ');
    final wd = '${r['wd'] ?? ''}';
    final wdDev = wd.isEmpty ? null : Wardrive.I.devices['$wd|$mac'];
    return InkWell(
      onTap: () => wdDev != null ? wardriveDeviceSheet(context, wdDev) : openDevice(context, mac),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
        child: Row(
          children: [
            IconTile(domIcon(d), wd.isEmpty ? C.dom(d) : (wdDev != null ? wdColor(wdDev) : C.muted)),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    deviceTitle(mac, '${r['name'] ?? ''}'.isNotEmpty ? '${r['name']}' : '${r['descr'] ?? ''}'),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: T.row,
                  ),
                  const SizedBox(height: 3),
                  Text(nodes.isEmpty ? mac : '$mac  ·  $nodes', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.code),
                ],
              ),
            ),
            const SizedBox(width: 8),
            Column(
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                Text(clock((r['last'] as num?)?.toDouble() ?? 0, date: true), style: T.meta.copyWith(fontSize: 11.5)),
                const SizedBox(height: 4),
                Text('${r['hits'] ?? 0}x  ${best ?? '-'} dBm', style: T.code.copyWith(fontSize: 11, color: C.rssi(best))),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
