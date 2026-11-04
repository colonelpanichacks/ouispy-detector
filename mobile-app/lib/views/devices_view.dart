// DEVICES: the Tab5's TARGETS (heard in the last 10 min, who hears them) plus the
// dashboard's whole history, searchable. A tap opens the full detail page.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'feed_view.dart';

class DevicesView extends StatefulWidget {
  const DevicesView({super.key});
  @override
  State<DevicesView> createState() => _DevicesViewState();
}

class _DevicesViewState extends State<DevicesView> {
  bool history = false;
  String dom = '';
  String q = '';
  List<Map<String, dynamic>> rows = [];
  int total = 0;
  bool loading = false;
  Timer? _deb;

  Future<void> _load() async {
    setState(() => loading = true);
    final j = await App.I.link.history(q: q, dom: dom);
    if (!mounted) return;
    setState(() {
      loading = false;
      rows = [for (final r in (j?['rows'] as List? ?? const [])) (r as Map).cast<String, dynamic>()];
      total = (j?['total'] as num?)?.toInt() ?? rows.length;
    });
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
          child: Seg<bool>(
            items: const [(false, 'Heard now'), (true, 'History')],
            value: history,
            onChanged: (v) {
              setState(() => history = v);
              if (v) _load();
            },
          ),
        ),
        ChipRow(
          children: [
            for (final (k, l, ic) in [('', 'Everything', null), ('ble', 'Bluetooth', Ph.bluetooth), ('wifi', 'Wi-Fi', Ph.wifiHigh)])
              Chip2(
                l,
                icon: ic,
                on: dom == k,
                onTap: () {
                  setState(() => dom = k);
                  if (history) _load();
                },
              ),
          ],
        ),
        Expanded(
          child: history
              ? RefreshIndicator(
                  color: C.accent,
                  onRefresh: _load,
                  child: loading && rows.isEmpty
                      ? const Center(child: CircularProgressIndicator())
                      : rows.isEmpty
                      ? ListView(children: [linkEmpty(context, 'No matches', 'Nothing in the history matches that.', icon: Ph.magnifyingGlass)])
                      : ListView.separated(
                          itemCount: rows.length + 1,
                          separatorBuilder: (_, i) => i == 0 ? const SizedBox() : const Divider(indent: 64),
                          itemBuilder: (_, i) => i == 0
                              ? Padding(
                                  padding: const EdgeInsets.fromLTRB(20, 2, 16, 6),
                                  child: Text('$total devices', style: T.meta),
                                )
                              : _HistRow(rows[i - 1]),
                        ),
                )
              : ListenableBuilder(
                  listenable: m,
                  builder: (_, _) {
                    final ql = q.toLowerCase();
                    final list = m.liveTargets.where((t) {
                      if (dom.isNotEmpty && t.dom != dom) return false;
                      if (ql.isEmpty) return true;
                      return '${t.mac} ${t.name} ${t.descr} ${m.aliasOf(t.mac)}'.toLowerCase().contains(ql);
                    }).toList();
                    if (list.isEmpty) {
                      return linkEmpty(
                        context,
                        'Nothing nearby',
                        'No device heard in the last 10 minutes. History has everything the mesh has ever heard.',
                        icon: Ph.broadcast,
                      );
                    }
                    return ListView.builder(padding: const EdgeInsets.only(bottom: 16), itemCount: list.length, itemBuilder: (_, i) => _TargetRow(list[i]));
                  },
                ),
        ),
      ],
    );
  }
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
                    Text(m.labelOf(t.mac, t.descr.isNotEmpty ? t.descr : t.mac), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
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
    return InkWell(
      onTap: () => openDevice(context, mac),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
        child: Row(
          children: [
            IconTile(domIcon(d), C.dom(d)),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    m.labelOf(mac, '${r['name'] ?? ''}'.isNotEmpty ? '${r['name']}' : '${r['descr'] ?? mac}'),
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
