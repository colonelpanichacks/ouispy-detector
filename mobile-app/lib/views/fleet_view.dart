// FLEET: the home node's link counters and every node, as the Tab5's FLEET view; a node's
// page drives it through the dashboard (/api/nodes/<id>/cmd, the same ops as the Tab5).
import 'package:flutter/material.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'feed_view.dart';

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
        final up = h['alive'] != false;
        Widget cell(String k, String v, {Color c = C.text}) => SizedBox(
          width: 96,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(k, style: T.meta.copyWith(fontSize: 11.5)),
              const SizedBox(height: 2),
              Text(
                v,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: T.code.copyWith(color: c, fontSize: 14, fontWeight: FontWeight.w600),
              ),
            ],
          ),
        );
        return RefreshIndicator(
          color: C.accent,
          onRefresh: App.I.link.fetchState,
          child: ListView(
            padding: const EdgeInsets.only(bottom: 20),
            children: [
              if (h.isNotEmpty) ...[
                SectionHead(
                  'Home link',
                  trailing: Row(
                    children: [
                      Dot(up ? C.green : C.amber, size: 7, halo: up),
                      const SizedBox(width: 6),
                      Text(up ? 'Up via ${'${h['source'] ?? '-'}'}' : 'Down', style: T.meta.copyWith(color: up ? C.green : C.amber)),
                    ],
                  ),
                ),
                Panel(
                  margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
                  child: Wrap(
                    spacing: 10,
                    runSpacing: 14,
                    children: [
                      cell('Node', '${h['id'] ?? '-'}'),
                      cell('Channel', '${h['ch'] ?? '-'}'),
                      cell('Rate', '${h['rate'] ?? '-'}'.toUpperCase()),
                      cell('Encrypted', h['enc'] == 1 ? 'Yes' : 'No', c: h['enc'] == 1 ? C.green : C.red),
                      cell('Peers', '${h['n'] ?? '-'}'),
                      cell('Uptime', h['up'] is num ? dur(h['up']) : '-'),
                      cell('RX', '${h['rx'] ?? 0}'),
                      cell('TX', '${h['tx'] ?? 0}'),
                      cell('Dupes', '${h['dup'] ?? 0}'),
                      cell('TX fail', '${h['txf'] ?? 0}', c: (h['txf'] ?? 0) == 0 ? C.text : C.red),
                      cell('Bad', '${h['bad'] ?? 0}', c: (h['bad'] ?? 0) == 0 ? C.text : C.red),
                    ],
                  ),
                ),
              ],
              SectionHead('Nodes', trailing: Text('${m.nodesOnline} of ${nodes.length} online', style: T.meta)),
              if (nodes.isEmpty) linkEmpty(context, 'No nodes yet', 'Nodes appear once the home node hears them.', icon: Ph.graph),
              for (final (i, n) in nodes.indexed) _NodeCard(n, i),
            ],
          ),
        );
      },
    );
  }
}

String _cap(String s) => s.isEmpty ? s : s[0].toUpperCase() + s.substring(1);

class _NodeCard extends StatelessWidget {
  const _NodeCard(this.n, this.i);
  final FleetNode n;
  final int i;
  @override
  Widget build(BuildContext context) {
    final c = C.hue(n.hue, i);
    final st = n.online ? C.green : (n.silent ? C.amber : C.red);
    final ota = '${n.ota['state'] ?? ''}';
    final icon = switch (n.kind) {
      'sky-spy' => Ph.drone,
      'wardriver' => Ph.mapPin,
      _ => n.isHome ? Ph.house : Ph.cpu,
    };
    return Panel(
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => NodeDetail(id: n.id))),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              IconTile(icon, c),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Flexible(
                          child: Text(n.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                        ),
                        const SizedBox(width: 8),
                        Tag(n.isHome ? 'Home' : _cap(n.role), color: n.isHome ? C.accent : C.muted),
                      ],
                    ),
                    const SizedBox(height: 3),
                    Row(
                      children: [
                        Text(n.id, style: T.code),
                        const SizedBox(width: 8),
                        Text(_cap(n.kind), style: T.meta),
                      ],
                    ),
                  ],
                ),
              ),
              Column(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Row(
                    children: [
                      Dot(st, size: 7, halo: n.online),
                      const SizedBox(width: 6),
                      Text(
                        n.online ? 'Online' : (n.silent ? 'Silent' : 'Offline'),
                        style: T.meta.copyWith(color: st, fontWeight: FontWeight.w600),
                      ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  if (n.online && n.rssi != null)
                    Row(
                      children: [
                        Text('${n.rssi}', style: T.code.copyWith(fontSize: 11.5, color: C.rssi(n.rssi))),
                        const SizedBox(width: 4),
                        RssiBar(n.rssi),
                      ],
                    )
                  else if (!n.online)
                    Text(dur(n.age), style: T.code.copyWith(fontSize: 11.5)),
                ],
              ),
            ],
          ),
          const SizedBox(height: 10),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              if (n.fw.isNotEmpty) Tag(n.fw, icon: Ph.cpu),
              if (n.filters != null) Tag('${n.filters} filters', icon: Ph.funnel, color: n.filtersFresh == false ? C.amber : C.muted),
              if (n.peers != null) Tag('${n.peers} peers', icon: Ph.graph),
              if (n.up != null) Tag('up ${dur(n.up!)}', icon: Ph.clock),
              if (ota.isNotEmpty && ota != 'done') Tag('Updating ${n.ota['pct'] ?? ''}%', icon: Ph.arrowsClockwise, color: C.accent),
            ],
          ),
        ],
      ),
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
    final dash = App.I.link;
    return Scaffold(
      appBar: AppBar(title: const Text('Node')),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final n = m.nodes[widget.id];
          if (n == null) return const Empty('Not in the fleet', body: 'This node is not in the fleet any more.', icon: Ph.graph);
          final c = C.hue(n.hue);
          final st = n.online ? C.green : (n.silent ? C.amber : C.red);
          final cfg = n.cfg;
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 4, 20, 4),
                child: Row(
                  children: [
                    IconTile(n.isHome ? Ph.house : Ph.cpu, c, size: 48),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(n.label, style: T.title),
                          const SizedBox(height: 4),
                          Row(
                            children: [
                              Dot(st, size: 7, halo: n.online),
                              const SizedBox(width: 6),
                              Text(
                                n.online ? 'Online · ${n.isHome ? 'home' : n.role} · ${n.kind}' : '${n.silent ? 'Silent' : 'Offline'} for ${dur(n.age)}',
                                style: T.meta.copyWith(color: st),
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 14, 16, 0),
                child: Row(
                  children: [
                    Expanded(
                      child: Btn(
                        'Rename',
                        icon: Ph.pencilSimple,
                        expand: true,
                        onTap: () async {
                          final v = await askText(context, 'Name this node', n.alias);
                          if (v != null) dash.nodeSet(n.id, {'alias': v});
                        },
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Btn('Colour', icon: Ph.palette, color: c, expand: true, onTap: () => _colour(n)),
                    ),
                  ],
                ),
              ),
              Group(
                title: 'On a detection',
                children: [
                  _toggle(
                    Ph.speakerHigh,
                    'Buzzer',
                    'Beeps when it hears something',
                    n.buzzer,
                    (v) => dash.nodeCmd(n.id, 'audio', {'buzzer': v ? 1 : 0, 'led': n.led == 0 ? 0 : 1}),
                  ),
                  _toggle(
                    Ph.lightbulb,
                    'Alert LED',
                    'Flashes when it hears something',
                    n.led,
                    (v) => dash.nodeCmd(n.id, 'audio', {'buzzer': n.buzzer == 1 ? 1 : 0, 'led': v ? 1 : 0}),
                  ),
                ],
              ),
              Group(
                title: 'Actions',
                children: [
                  Tile(
                    icon: Ph.lightbulb,
                    color: C.cyan,
                    title: 'Identify',
                    subtitle: Text('Blinks its LED for 10 seconds', style: T.meta),
                    onTap: () => dash.nodeCmd(n.id, 'identify', {'secs': 10}),
                  ),
                  Tile(icon: Ph.speakerHigh, color: C.amber, title: 'Beep three times', onTap: () => dash.nodeCmd(n.id, 'locate', {'mode': 1})),
                  Tile(
                    icon: Ph.gpsFix,
                    color: C.orange,
                    title: locating ? 'Stop locating' : 'Locate',
                    subtitle: Text(
                      locating ? 'Beeping until you stop it' : 'Beeps until you stop it, to find it',
                      style: T.meta.copyWith(color: locating ? C.orange : null),
                    ),
                    trailing: locating ? const Dot(C.orange, halo: true) : null,
                    onTap: () {
                      setState(() => locating = !locating);
                      dash.nodeCmd(n.id, 'locate', {'mode': locating ? 2 : 0});
                    },
                  ),
                  Tile(icon: Ph.arrowsClockwise, color: C.green, title: 'Read status', onTap: () => dash.nodeCmd(n.id, 'status')),
                  Tile(icon: Ph.funnel, color: C.violet, title: 'Install a filter preset', onTap: () => _preset(n)),
                  Tile(
                    icon: Ph.power,
                    color: C.red,
                    title: 'Reboot',
                    titleColor: C.red,
                    onTap: () async {
                      if (await confirm(context, 'Reboot ${n.label}?', 'It restarts and drops off the mesh for a few seconds.', yes: 'Reboot')) {
                        dash.nodeCmd(n.id, 'reboot');
                      }
                    },
                  ),
                ],
              ),
              Group(
                title: 'Status',
                children: [
                  KV('Node ID', n.id, monoValue: true),
                  if (n.fw.isNotEmpty) KV('Firmware', n.fw, monoValue: true),
                  if (n.ch != null) KV('Mesh channel', '${n.ch}'),
                  if (n.filters != null)
                    KV('Filters', '${n.filters}${n.filtersFresh == false ? '\nNot the list the dashboard has' : ''}', color: n.filtersFresh == false ? C.amber : C.text),
                  if (cfg['fcrc'] != null) KV('Filter CRC', '${cfg['fcrc']}', monoValue: true),
                  if (n.rssi != null) KV('Link to home', '${n.rssi} dBm', color: C.rssi(n.rssi)),
                  if (n.lrssi != null) KV('Best neighbour', '${n.lrssi} dBm', color: C.rssi(n.lrssi)),
                  if (n.peers != null) KV('Peers', '${n.peers}'),
                  if (n.up != null) KV('Uptime', dur(n.up!)),
                  if (n.heap != null) KV('Free heap', '${(n.heap! / 1024).round()} KB'),
                  if (cfg['mode'] != null) KV('Mode', '${cfg['mode']}${cfg['locked'] == 1 ? ', burned in' : ''}'),
                  if (cfg['key'] != null) KV('Fleet key', cfg['key'] == 1 ? 'Set (fp ${cfg['kfp'] ?? '?'})' : 'None', color: cfg['key'] == 1 ? C.green : C.red),
                  if (n.hasPos) KV('Placed at', '${n.lat!.toStringAsFixed(5)}, ${n.lon!.toStringAsFixed(5)}', monoValue: true),
                  if (n.ota.isNotEmpty)
                    KV(
                      'Last update',
                      '${n.ota['state'] ?? '-'} ${n.ota['pct'] ?? ''}%${n.ota['err'] != null && '${n.ota['err']}'.isNotEmpty ? ' (${n.ota['err']})' : ''}',
                      color: n.ota['state'] == 'done' ? C.green : C.amber,
                    ),
                ],
              ),
              if (n.nbr.isNotEmpty)
                Group(
                  title: 'Mesh neighbours',
                  children: [
                    for (final s in n.nbr)
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                        child: Row(
                          children: [
                            Dot(C.hue(m.nodes[s.node]?.hue), size: 9),
                            const SizedBox(width: 10),
                            Expanded(child: Text(m.nodeName(s.node), style: T.row)),
                            if (s.age != 0) ...[const Tag('Direct', color: C.green), const SizedBox(width: 8)],
                            Text('${s.rssi} dBm', style: T.code.copyWith(color: C.rssi(s.rssi), fontSize: 12.5)),
                            const SizedBox(width: 6),
                            RssiBar(s.rssi),
                          ],
                        ),
                      ),
                  ],
                ),
            ],
          );
        },
      ),
    );
  }

  Widget _toggle(IconData i, String t, String sub, int? v, ValueChanged<bool> f) => Padding(
    padding: const EdgeInsets.fromLTRB(16, 6, 10, 6),
    child: Row(
      children: [
        IconTile(i, v == 1 ? C.accent : C.muted),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(t, style: T.row),
              const SizedBox(height: 2),
              Text(v == null ? '$sub. State not reported yet.' : sub, style: T.meta),
            ],
          ),
        ),
        Switch(value: v == 1, onChanged: f),
      ],
    ),
  );
  void _colour(FleetNode n) => showModalBottomSheet(
    context: context,
    builder: (c) => SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Wrap(
          spacing: 14,
          runSpacing: 14,
          children: [
            for (final h in nodeHues)
              GestureDetector(
                onTap: () {
                  App.I.link.nodeSet(n.id, {'color': h});
                  Navigator.pop(c);
                },
                child: Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: C.hue(h),
                    shape: BoxShape.circle,
                    border: Border.all(color: C.bg, width: 3),
                  ),
                ),
              ),
          ],
        ),
      ),
    ),
  );

  Future<void> _preset(FleetNode n) async {
    final p = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const SectionHead('Install a filter preset'),
            for (final p in presets)
              ListTile(
                leading: const Icon(Ph.funnel, color: C.violet),
                title: Text(_presetName(p)),
                onTap: () => Navigator.pop(c, p),
              ),
          ],
        ),
      ),
    );
    if (p == null || !mounted) return;
    if (await confirm(
      context,
      'Install ${_presetName(p)} on ${n.label}?',
      'Replaces the filters on the node with the ${_presetName(p)} preset.',
      yes: 'Install',
      color: C.violet,
    )) {
      App.I.link.nodeCmd(n.id, 'preset', {'name': p});
    }
  }
}

String _presetName(String p) => switch (p) {
  'axon' => 'Axon',
  'meta' => 'Meta',
  'ipro' => 'iPro',
  'verkada' => 'Verkada',
  _ => p,
};
