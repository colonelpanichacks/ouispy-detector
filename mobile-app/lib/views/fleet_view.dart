// FLEET: the home node's link counters and every node, as the Tab5's FLEET view; a node's
// page drives it through the dashboard (/api/nodes/<id>/cmd, the same ops as the Tab5).
import 'package:flutter/material.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

class FleetView extends StatelessWidget {
  const FleetView({super.key});
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final nodes = m.nodes.values.toList()
          ..sort((a, b) => a.isHome != b.isHome ? (a.isHome ? -1 : 1) : (a.online != b.online ? (a.online ? -1 : 1) : a.label.compareTo(b.label)));
        final h = m.home;
        return RefreshIndicator(
          color: C.orange,
          onRefresh: App.I.dash.fetchState,
          child: ListView(padding: const EdgeInsets.only(bottom: 20), children: [
            if (h.isNotEmpty) ...[
              SectionHead('HOME ${h['id'] ?? ''}', color: h['alive'] == false ? C.amber : C.green),
              Panel(
                glow: true,
                color: h['alive'] == false ? C.amber : C.green,
                child: Wrap(spacing: 16, runSpacing: 10, children: [
                  _num('STATE', h['alive'] == false ? 'DOWN' : 'UP', h['alive'] == false ? C.amber : C.green),
                  _num('VIA', '${h['source'] ?? '-'}'.toUpperCase(), C.cyan),
                  _num('CHANNEL', '${h['ch'] ?? '-'}', C.accent),
                  _num('RATE', '${h['rate'] ?? '-'}'.toUpperCase(), C.accent),
                  _num('ENCRYPTED', h['enc'] == 1 ? 'YES' : 'NO', h['enc'] == 1 ? C.green : C.red),
                  _num('RX', '${h['rx'] ?? 0}', C.sky),
                  _num('TX', '${h['tx'] ?? 0}', C.sky),
                  _num('DUP', '${h['dup'] ?? 0}', C.purple),
                  _num('TX FAIL', '${h['txf'] ?? 0}', (h['txf'] ?? 0) == 0 ? C.green : C.red),
                  _num('BAD', '${h['bad'] ?? 0}', (h['bad'] ?? 0) == 0 ? C.green : C.red),
                  _num('PEERS', '${h['n'] ?? '-'}', C.orange),
                  _num('UPTIME', h['up'] is num ? dur(h['up']) : '-', C.accent),
                ]),
              ),
            ],
            SectionHead('NODES  ${m.nodesOnline}/${nodes.length} ONLINE', color: C.orange),
            if (nodes.isEmpty) const Empty('No nodes yet.'),
            for (final (i, n) in nodes.indexed) _NodeCard(n, i),
          ]),
        );
      },
    );
  }
}

Widget _num(String k, String v, Color c) => SizedBox(
      width: 88,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(k, style: const TextStyle(color: C.faint, fontSize: 9.5, fontWeight: FontWeight.w800, letterSpacing: 1)),
        Glow(v, color: c, size: 14),
      ]),
    );

class _NodeCard extends StatelessWidget {
  const _NodeCard(this.n, this.i);
  final FleetNode n;
  final int i;
  @override
  Widget build(BuildContext context) {
    final c = C.hue(n.hue, i);
    final st = n.online ? C.green : (n.silent ? C.amber : C.red);
    final ota = '${n.ota['state'] ?? ''}';
    return Panel(
      glow: n.online,
      color: n.online ? c : st,
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => NodeDetail(id: n.id))),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Dot(st),
          const SizedBox(width: 8),
          Expanded(child: Glow(n.label, color: c, size: 16)),
          if (n.isHome) const Pill('HOME', color: C.green, small: true),
          if (!n.isHome) Pill(n.role.toUpperCase(), color: C.sky, small: true),
          const SizedBox(width: 6),
          Pill(n.kind.toUpperCase(), color: n.kind == 'detector' ? C.accent : (n.kind == 'sky-spy' ? C.hot : C.cyan), small: true),
        ]),
        const SizedBox(height: 6),
        Row(children: [
          Text(n.id, style: const TextStyle(color: C.soft, fontFamily: mono, fontSize: 12)),
          const SizedBox(width: 10),
          Text(n.online ? 'online' : (n.silent ? 'silent ${dur(n.age)}' : 'offline ${dur(n.age)}'), style: TextStyle(color: st, fontSize: 12, fontWeight: FontWeight.w700)),
          const Spacer(),
          if (n.rssi != null) ...[Text('${n.rssi} dBm ', style: TextStyle(color: C.rssi(n.rssi), fontSize: 12, fontFamily: mono)), RssiBar(n.rssi)],
        ]),
        const SizedBox(height: 6),
        Wrap(spacing: 12, children: [
          if (n.fw.isNotEmpty) Text('fw ${n.fw}', style: const TextStyle(color: C.muted, fontSize: 11)),
          if (n.filters != null) Text('${n.filters} filters', style: TextStyle(color: n.filtersFresh == false ? C.amber : C.muted, fontSize: 11)),
          if (n.peers != null) Text('${n.peers} peers', style: const TextStyle(color: C.muted, fontSize: 11)),
          if (n.up != null) Text('up ${dur(n.up!)}', style: const TextStyle(color: C.muted, fontSize: 11)),
          if (ota.isNotEmpty && ota != 'done') Text('OTA $ota ${n.ota['pct'] ?? ''}%', style: const TextStyle(color: C.hot, fontSize: 11, fontWeight: FontWeight.w700)),
        ]),
      ]),
    );
  }
}

const presets = ['axon', 'meta', 'ipro', 'verkada'];
const nodeHues = [0, 25, 45, 90, 140, 175, 195, 220, 260, 285, 310, 335];

class NodeDetail extends StatefulWidget {
  const NodeDetail({super.key, required this.id});
  final String id;
  @override
  State<NodeDetail> createState() => _NodeDetailState();
}

class _NodeDetailState extends State<NodeDetail> {
  bool locating = false;

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final dash = App.I.dash;
    return Scaffold(
      appBar: AppBar(title: Glow(widget.id, color: C.orange, size: 15, font: mono)),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final n = m.nodes[widget.id];
          if (n == null) return const Empty('This node is not in the fleet any more.');
          final c = C.hue(n.hue);
          final cfg = n.cfg;
          return ListView(padding: const EdgeInsets.only(bottom: 30), children: [
            Panel(
              glow: true,
              color: n.online ? c : (n.silent ? C.amber : C.red),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Glow(n.label, color: c, size: 21),
                Text(n.online ? 'online, ${n.isHome ? 'home' : n.role}, ${n.kind}' : '${n.silent ? 'SILENT' : 'OFFLINE'} for ${dur(n.age)}',
                    style: TextStyle(color: n.online ? C.green : (n.silent ? C.amber : C.red), fontWeight: FontWeight.w700)),
                const SizedBox(height: 10),
                Wrap(spacing: 7, runSpacing: 7, children: [
                  Pill('RENAME', color: C.accent, onTap: () async {
                    final v = await askText(context, 'NAME THIS NODE', n.alias);
                    if (v != null) dash.nodeSet(n.id, {'alias': v});
                  }),
                  Pill('COLOUR', color: c, onTap: () => _colour(n)),
                ]),
              ]),
            ),
            const SectionHead('CONTROL', color: C.hot),
            Panel(
              child: Column(children: [
                _toggle('Buzzer', 'beeps on a detection', n.buzzer, (v) => dash.nodeCmd(n.id, 'audio', {'buzzer': v ? 1 : 0, 'led': n.led == 0 ? 0 : 1})),
                _toggle('Alert LED', 'flashes on a detection', n.led, (v) => dash.nodeCmd(n.id, 'audio', {'buzzer': n.buzzer == 1 ? 1 : 0, 'led': v ? 1 : 0})),
                const SizedBox(height: 8),
                Wrap(spacing: 7, runSpacing: 8, children: [
                  Pill('IDENTIFY', color: C.cyan, onTap: () => dash.nodeCmd(n.id, 'identify', {'secs': 10})),
                  Pill('BEEP 3x', color: C.amber, onTap: () => dash.nodeCmd(n.id, 'locate', {'mode': 1})),
                  Pill(locating ? 'LOCATE: ON' : 'LOCATE', color: C.orange, on: locating, onTap: () {
                    setState(() => locating = !locating);
                    dash.nodeCmd(n.id, 'locate', {'mode': locating ? 2 : 0});
                  }),
                  Pill('READ STATUS', color: C.green, onTap: () => dash.nodeCmd(n.id, 'status')),
                  Pill('PRESET', color: C.purple, onTap: () => _preset(n)),
                  Pill('REBOOT', color: C.red, onTap: () async {
                    if (await confirm(context, 'REBOOT ${n.label}?', 'The node restarts and drops off the mesh for a few seconds.', yes: 'REBOOT')) {
                      dash.nodeCmd(n.id, 'reboot');
                    }
                  }),
                ]),
              ]),
            ),
            const SectionHead('STATUS'),
            Panel(
              child: Column(children: [
                KV('Node ID', n.id, monoValue: true),
                if (n.fw.isNotEmpty) KV('Firmware', n.fw),
                if (n.ch != null) KV('Mesh channel', '${n.ch}'),
                if (n.filters != null) KV('Filters', '${n.filters}${n.filtersFresh == false ? '  (stale: not the list the dashboard has)' : ''}', color: n.filtersFresh == false ? C.amber : C.text),
                if (cfg['fcrc'] != null) KV('Filter CRC', '${cfg['fcrc']}', monoValue: true),
                if (n.rssi != null) KV('Link to home', '${n.rssi} dBm', color: C.rssi(n.rssi)),
                if (n.lrssi != null) KV('Best neighbour', '${n.lrssi} dBm', color: C.rssi(n.lrssi)),
                if (n.peers != null) KV('Peers', '${n.peers}'),
                if (n.up != null) KV('Uptime', dur(n.up!)),
                if (n.heap != null) KV('Free heap', '${(n.heap! / 1024).round()} KB'),
                if (cfg['mode'] != null) KV('Mode', '${cfg['mode']}${cfg['locked'] == 1 ? ', burned in' : ''}'),
                if (cfg['key'] != null) KV('Fleet key', cfg['key'] == 1 ? 'set (fp ${cfg['kfp'] ?? '?'})' : 'none', color: cfg['key'] == 1 ? C.green : C.red),
                if (n.hasPos) KV('Placed at', '${n.lat!.toStringAsFixed(5)}, ${n.lon!.toStringAsFixed(5)}', monoValue: true),
                if (n.ota.isNotEmpty) KV('Last OTA', '${n.ota['state'] ?? '-'} ${n.ota['pct'] ?? ''}%${n.ota['err'] != null && '${n.ota['err']}'.isNotEmpty ? ' (${n.ota['err']})' : ''}', color: n.ota['state'] == 'done' ? C.green : C.amber),
              ]),
            ),
            if (n.nbr.isNotEmpty) ...[
              const SectionHead('MESH NEIGHBOURS', color: C.lime),
              Panel(
                child: Column(children: [
                  for (final s in n.nbr)
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 4),
                      child: Row(children: [
                        Expanded(child: Text(m.nodeName(s.node), style: TextStyle(color: C.hue(m.nodes[s.node]?.hue), fontWeight: FontWeight.w700))),
                        if (s.age != 0) const Pill('DIRECT', color: C.green, small: true),
                        const SizedBox(width: 8),
                        Text('${s.rssi} dBm ', style: TextStyle(color: C.rssi(s.rssi), fontFamily: mono)),
                        RssiBar(s.rssi),
                      ]),
                    ),
                ]),
              ),
            ],
          ]);
        },
      ),
    );
  }

  Widget _toggle(String t, String sub, int? v, ValueChanged<bool> f) => SwitchListTile(
        contentPadding: EdgeInsets.zero,
        title: Text(t, style: const TextStyle(color: C.text, fontWeight: FontWeight.w600)),
        subtitle: Text(v == null ? '$sub (state not reported yet)' : sub, style: const TextStyle(color: C.muted, fontSize: 12)),
        value: v == 1,
        onChanged: f,
      );

  void _colour(FleetNode n) => showModalBottomSheet(
        context: context,
        builder: (c) => SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(18),
            child: Wrap(spacing: 14, runSpacing: 14, children: [
              for (final h in nodeHues)
                GestureDetector(
                  onTap: () {
                    App.I.dash.nodeSet(n.id, {'color': h});
                    Navigator.pop(c);
                  },
                  child: Container(width: 40, height: 40, decoration: BoxDecoration(color: C.hue(h), shape: BoxShape.circle, boxShadow: [BoxShadow(color: C.hue(h), blurRadius: 10)])),
                ),
            ]),
          ),
        ),
      );

  Future<void> _preset(FleetNode n) async {
    final p = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(shrinkWrap: true, children: [
          const SectionHead('INSTALL A FILTER PRESET', color: C.purple),
          for (final p in presets)
            ListTile(title: Text(p.toUpperCase(), style: const TextStyle(color: C.purple, fontWeight: FontWeight.w800)), onTap: () => Navigator.pop(c, p)),
        ]),
      ),
    );
    if (p == null || !mounted) return;
    if (await confirm(context, 'INSTALL $p ON ${n.label}?', 'Installs the $p preset filters on the node.', yes: 'INSTALL', color: C.purple)) {
      App.I.dash.nodeCmd(n.id, 'preset', {'name': p});
    }
  }
}
