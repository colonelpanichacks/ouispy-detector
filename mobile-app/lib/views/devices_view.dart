// DEVICES: the Tab5's TARGETS (heard in the last 10 min, who hears them) plus the
// dashboard's whole history, searchable. A tap opens the full detail page.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

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
    final j = await App.I.dash.history(q: q, dom: dom);
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
    return Column(children: [
      Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 0),
        child: TextField(
          onChanged: _query,
          style: const TextStyle(color: C.text),
          decoration: const InputDecoration(prefixIcon: Icon(Icons.search, color: C.accent), hintText: 'MAC, name, vendor, alias'),
        ),
      ),
      PillRow(children: [
        Pill('LIVE', color: C.green, on: !history, onTap: () => setState(() => history = false)),
        Pill('HISTORY', color: C.orange, on: history, onTap: () {
          setState(() => history = true);
          _load();
        }),
        const SizedBox(width: 6),
        for (final (k, l, c) in [('', 'ALL', C.accent), ('ble', 'BLE', C.purple), ('wifi', 'WIFI', C.sky)])
          Pill(l, color: c, on: dom == k, onTap: () {
            setState(() => dom = k);
            if (history) _load();
          }),
      ]),
      Expanded(
        child: history
            ? RefreshIndicator(
                color: C.accent,
                onRefresh: _load,
                child: loading && rows.isEmpty
                    ? const Center(child: CircularProgressIndicator(color: C.accent))
                    : rows.isEmpty
                        ? ListView(children: const [Empty('Nothing in the history matches.')])
                        : ListView.builder(
                            itemCount: rows.length + 1,
                            itemBuilder: (_, i) => i == 0
                                ? Padding(
                                    padding: const EdgeInsets.fromLTRB(16, 2, 16, 4),
                                    child: Text('$total devices', style: const TextStyle(color: C.muted, fontSize: 11.5)))
                                : _HistRow(rows[i - 1])),
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
                  if (list.isEmpty) return const Empty('No devices heard in the last 10 minutes.\nHISTORY has everything the mesh has ever heard.');
                  return ListView.builder(itemCount: list.length, itemBuilder: (_, i) => _TargetRow(list[i]));
                },
              ),
      ),
    ]);
  }
}

class _TargetRow extends StatelessWidget {
  const _TargetRow(this.t);
  final Target t;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final live = m.now - t.last < targetLiveS;
    final c = t.dom == 'ble' ? C.purple : C.cyan;
    return Panel(
      onTap: () => openDevice(context, t.mac),
      color: live ? c : C.line2,
      glow: live,
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Dot(live ? C.green : C.amber, size: 8),
          const SizedBox(width: 8),
          Expanded(
              child: Text(m.labelOf(t.mac, t.descr.isNotEmpty ? t.descr : t.mac),
                  overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.text, fontSize: 15, fontWeight: FontWeight.w700))),
          if (t.triOk) Pill(t.triRough ? 'ROUGH FIX' : 'FIXED', color: t.triRough ? C.amber : C.green, small: true),
          const SizedBox(width: 6),
          Text(ago(t.last), style: TextStyle(color: live ? C.green : C.amber, fontSize: 12, fontFamily: mono)),
        ]),
        const SizedBox(height: 4),
        Row(children: [
          Text(t.mac, style: const TextStyle(color: C.soft, fontSize: 11.5, fontFamily: mono)),
          const SizedBox(width: 8),
          Text((t.type.isEmpty ? t.dom : t.type).toUpperCase(), style: TextStyle(color: c, fontSize: 11, fontWeight: FontWeight.w700)),
          const Spacer(),
          Text('${t.hits} hits', style: const TextStyle(color: C.muted, fontSize: 11)),
        ]),
        if (t.seen.isNotEmpty) ...[
          const SizedBox(height: 7),
          Wrap(spacing: 6, runSpacing: 4, children: [
            for (final (i, s) in t.seen.indexed)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: C.hue(m.nodes[s.node]?.hue, i).withValues(alpha: i == 0 ? 1 : 0.5))),
                child: Text('${m.nodeName(s.node)} ${s.rssi}',
                    style: TextStyle(color: C.hue(m.nodes[s.node]?.hue, i), fontSize: 11, fontWeight: i == 0 ? FontWeight.w800 : FontWeight.w500)),
              ),
          ]),
        ],
      ]),
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
    final c = d == 'ble' ? C.purple : C.cyan;
    final best = (r['best'] as num?)?.toInt();
    return InkWell(
      onTap: () => openDevice(context, mac),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 9),
        decoration: const BoxDecoration(border: Border(bottom: BorderSide(color: C.line))),
        child: Row(children: [
          Container(width: 3, height: 34, color: c),
          const SizedBox(width: 10),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(m.labelOf(mac, '${r['name'] ?? ''}'.isNotEmpty ? '${r['name']}' : '${r['descr'] ?? mac}'),
                  overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.text, fontSize: 14, fontWeight: FontWeight.w600)),
              const SizedBox(height: 2),
              Text('$mac   ${'${r['type'] ?? d}'.toUpperCase()}   ${(r['nodes'] as List? ?? const []).map((n) => m.nodeName('$n')).join(', ')}',
                  overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.soft, fontSize: 11, fontFamily: mono)),
            ]),
          ),
          Column(crossAxisAlignment: CrossAxisAlignment.end, children: [
            Text(clock((r['last'] as num?)?.toDouble() ?? 0, date: true), style: const TextStyle(color: C.muted, fontSize: 11)),
            const SizedBox(height: 3),
            Text('${r['hits'] ?? 0}x  ${best ?? '?'} dBm', style: TextStyle(color: C.rssi(best), fontSize: 11, fontFamily: mono)),
          ]),
        ]),
      ),
    );
  }
}
