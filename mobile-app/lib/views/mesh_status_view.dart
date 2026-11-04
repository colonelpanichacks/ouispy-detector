// MESH STATUS: both meshes at a glance (meshstatus.js). The ESP-NOW fleet (who is up,
// channel, rate, encryption, wrong-key frames, replays), who hears whom from each node's
// reported neighbours (cfg.nbr), the Meshtastic link and the dashboard's USB feeds
// (/api/meshstatus), the fleet key's status (present and fingerprint only: a key is never
// shown or made here), and the commands the dashboard ran (/api/jobs, polled while open).
// On a node's USB the dashboard-only parts give way to what the phone knows itself.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/dashboard.dart';
import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'fleet_control_view.dart';

class MeshStatusView extends StatefulWidget {
  const MeshStatusView({super.key});
  @override
  State<MeshStatusView> createState() => _MeshStatusViewState();
}

class _MeshStatusViewState extends State<MeshStatusView> {
  Map<String, dynamic>? ms, key;
  List<Map<String, dynamic>> jobs = [];
  Timer? _t;
  int _n = 0;

  Dashboard? get dash => App.I.link is Dashboard ? App.I.link as Dashboard : null;

  @override
  void initState() {
    super.initState();
    _poll();
    _t = Timer.periodic(const Duration(seconds: 3), (_) => _poll());
  }

  @override
  void dispose() {
    _t?.cancel();
    super.dispose();
  }

  Future<void> _poll() async {
    final d = dash;
    if (d == null) return;
    final n = _n++;
    final j = await d.getJson('/api/jobs');
    final s = n % 2 == 0 ? await d.getJson('/api/meshstatus') : null;
    final k = n % 10 == 0 ? await d.getJson('/api/fleetkey/status') : null;
    if (!mounted) return;
    setState(() {
      if (j is List) jobs = [for (final x in j.take(40)) (x as Map).cast<String, dynamic>()];
      if (s is Map) ms = s.cast<String, dynamic>();
      if (k is Map) key = k.cast<String, dynamic>();
    });
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ReconPage(
      title: 'MESH STATUS',
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final nodes = m.nodes.values.toList()..sort((a, b) => a.isHome != b.isHome ? (a.isHome ? -1 : 1) : a.label.compareTo(b.label));
          return RefreshIndicator(
            color: C.accent,
            onRefresh: () async {
              _n = 0;
              await _poll();
            },
            child: ListView(
              padding: const EdgeInsets.only(bottom: 30),
              children: [
                _espnow(m, nodes),
                if (dash != null) ...[_meshtastic(), _usb()],
                _key(m),
                SectionHead('Who hears whom', trailing: Text('rows hear columns', style: T.meta)),
                if (nodes.isEmpty) Panel(child: Text('No nodes yet.', style: T.meta)) else _matrix(m, nodes),
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 2, 20, 0),
                  child: Text('What each node last reported hearing. Strong >= -60 dBm, good >= -72, weak >= -85, poor below.', style: T.meta.copyWith(fontSize: 11.5)),
                ),
                ..._jobs(),
              ],
            ),
          );
        },
      ),
    );
  }

  // ---- tiles

  Widget _tile(String title, Color c, String big, List<String> lines, {List<Color?> colors = const []}) => Panel(
    edge: c == C.muted ? null : c,
    padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Dot(c, size: 7, halo: c == C.green),
            const SizedBox(width: 8),
            Text(title.toUpperCase(), style: T.label),
            const Spacer(),
            if (big.isNotEmpty) Text(big, style: T.num.copyWith(fontSize: 20, color: c == C.muted ? C.text : c)),
          ],
        ),
        for (final (i, l) in lines.indexed)
          if (l.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 6),
              child: Text(l, style: T.meta.copyWith(color: i < colors.length ? colors[i] : null)),
            ),
      ],
    ),
  );

  Widget _espnow(Model m, List<FleetNode> nodes) {
    final e = (ms?['espnow'] as Map?)?.cast<String, dynamic>();
    final h = m.home;
    final on = nodes.where((n) => n.online).length;
    final total = nodes.length;
    final ch = e?['ch'] ?? h['ch'];
    final rate = '${e?['rate'] ?? h['rate'] ?? '?'}';
    final rateName = const {'lr250k': 'LR 250 kbps', 'lr500k': 'LR 500 kbps'}[rate] ?? rate;
    final enc = e != null ? e['enc'] == true : h['enc'] == 1 || h['enc'] == true;
    int n(String k, String hk) => ((e?[k] ?? h[hk]) as num?)?.toInt() ?? 0;
    final auth = n('auth_fail', 'eauth'), rep = n('replays', 'ereplay'), lost = n('lost', 'dlost');
    final homeUp = h['alive'] != false && h.isNotEmpty;
    final c = !homeUp ? C.red : (auth > 0 ? C.red : (on == total ? C.green : C.amber));
    return _tile(
      'ESP-NOW mesh',
      c,
      '$on / $total up',
      [
        'channel ${ch ?? '?'}, $rateName, ${enc ? 'encrypted with the fleet key' : 'NOT encrypted'}',
        'wrong key $auth, replays $rep, undelivered $lost',
        homeUp ? 'home up ${h['up'] is num ? dur(h['up']) : '-'}' : 'home silent',
      ],
      colors: [enc ? null : C.red, auth > 0 ? C.red : null, homeUp ? null : C.red],
    );
  }

  Widget _meshtastic() {
    final t = (ms?['meshtastic'] as Map?)?.cast<String, dynamic>();
    if (t == null) return _tile('Meshtastic', C.muted, '', ['reading the dashboard']);
    final enabled = t['enabled'] == true;
    final link = (t['link'] as Map?)?.cast<String, dynamic>();
    final ready = t['state'] == 'ready';
    final linked = link != null && ((link['age'] as num?) ?? 1e9) < 900;
    final c = !enabled ? C.muted : (!ready ? C.red : (linked ? C.green : C.amber));
    final linkLine = link == null
        ? 'field Tab5: not heard since the dashboard started'
        : 'field Tab5${link['node'] != null ? ' (on ${App.I.m.nodeName('${link['node']}')})' : ''}, radio ${link['from'] ?? '?'}: ${dur((link['age'] as num?) ?? 0)} ago'
              '${link['rssi'] != null ? ', ${link['rssi']} dBm' : ''}${link['snr'] != null ? ', SNR ${link['snr']}' : ''}${link['wifi'] == true ? ', on Wi-Fi' : ', over LoRa'}';
    return _tile(
      'Meshtastic',
      c,
      (enabled ? '${t['state'] ?? '?'}' : 'off').toUpperCase(),
      [
        enabled ? '${t['radio'] ?? 'radio ?'} on ${t['channel'] ?? '?'}' : 'turn it on in the dashboard\'s Settings',
        if (enabled) 'sent ${t['sent'] ?? 0}, failed ${t['failed'] ?? 0}, waiting ${t['queued'] ?? 0}, nodes heard ${t['heard'] ?? 0}',
        if ('${t['error'] ?? ''}'.isNotEmpty) '${t['error']}' else if (enabled) linkLine,
      ],
      colors: [null, null, '${t['error'] ?? ''}'.isNotEmpty ? C.red : null],
    );
  }

  Widget _usb() {
    final u = (ms?['usb'] as Map?)?.cast<String, dynamic>() ?? const {};
    const names = {'serial': 'Home node', 'bridge': 'Tab5 bridge', 'wardrive-usb': 'Wardriver'};
    final t = (ms?['meshtastic'] as Map?)?.cast<String, dynamic>();
    final rows = <(String, bool, String)>[
      for (final e in u.entries)
        if (e.value is Map) (names[e.key] ?? e.key, (e.value as Map)['connected'] == true, '${(e.value as Map)['port'] ?? (e.value as Map)['detail'] ?? ''}'),
      if (t?['port'] != null) ('Meshtastic radio', t?['state'] == 'ready', '${t?['port']}'),
    ];
    return Group(
      title: 'USB on the dashboard',
      children: [
        if (rows.isEmpty)
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text(ms == null ? 'reading the dashboard' : 'nothing reported', style: T.meta),
          ),
        for (final (name, up, detail) in rows)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
            child: Row(
              children: [
                Dot(up ? C.green : (name == 'Wardriver' ? C.muted : C.amber), size: 7, halo: up),
                const SizedBox(width: 10),
                Text(name, style: T.row.copyWith(fontSize: 14)),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(detail, maxLines: 1, overflow: TextOverflow.ellipsis, textAlign: TextAlign.right, style: T.code),
                ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _key(Model m) {
    final home = m.nodes.values.where((n) => n.isHome).firstOrNull;
    final hk = home?.cfg['key'];
    final hfp = '${home?.cfg['kfp'] ?? ''}';
    final present = key != null ? key!['present'] == true : hk == 1;
    final fp = key != null ? '${key!['fp'] ?? ''}' : hfp;
    final others = [
      for (final n in m.nodes.values)
        if (hfp.isNotEmpty && '${n.cfg['kfp'] ?? ''}'.isNotEmpty && '${n.cfg['kfp']}' != hfp) n.label,
    ];
    return Group(
      title: 'Fleet key',
      children: [
        KV('Key', key == null && hk == null ? 'not reported yet' : (present ? 'Present' : 'None'), color: present ? C.green : C.red),
        if (fp.isNotEmpty && fp != 'null') KV('Fingerprint', fp, monoValue: true),
        if (key != null && hfp.isNotEmpty && fp.isNotEmpty && hfp != fp) KV('Home reports', hfp, monoValue: true, color: C.red),
        if (others.isNotEmpty) KV('Other key', others.join(', '), color: C.red),
      ],
    );
  }

  // ---- who hears whom

  Widget _matrix(Model m, List<FleetNode> nodes) {
    const w = 62.0;
    Widget head(String s, {double width = w}) => SizedBox(
      width: width,
      child: Text(s, maxLines: 1, overflow: TextOverflow.ellipsis, textAlign: TextAlign.center, style: T.silk.copyWith(fontSize: 9.5)),
    );
    return Panel(
      padding: const EdgeInsets.fromLTRB(10, 10, 10, 10),
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [head('HEARS', width: 110), for (final n in nodes) head(n.label)]),
            const SizedBox(height: 6),
            for (final r in nodes)
              Opacity(
                opacity: r.online ? 1 : 0.5,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3),
                  child: Row(
                    children: [
                      SizedBox(
                        width: 110,
                        child: Row(
                          children: [
                            Dot(r.online ? C.green : C.red, size: 6),
                            const SizedBox(width: 6),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(r.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row.copyWith(fontSize: 12.5)),
                                  Text(
                                    r.online ? (r.isHome ? 'home' : '${r.peers ?? 0} peers') : 'down ${dur(r.age)}',
                                    maxLines: 1,
                                    style: T.meta.copyWith(fontSize: 10.5),
                                  ),
                                ],
                              ),
                            ),
                          ],
                        ),
                      ),
                      for (final c in nodes) _cell(r, c, w),
                    ],
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _cell(FleetNode r, FleetNode c, double w) {
    if (r.id == c.id) {
      return Container(width: w - 4, height: 30, margin: const EdgeInsets.symmetric(horizontal: 2), color: C.panel2);
    }
    final s = r.nbr.where((x) => x.node == c.id).firstOrNull;
    final col = s == null ? C.faint : C.rssi(s.rssi);
    return Container(
      width: w - 4,
      height: 30,
      margin: const EdgeInsets.symmetric(horizontal: 2),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(2),
        border: Border.all(color: s == null ? C.silkLine : col.withValues(alpha: 0.5)),
        color: s == null ? null : col.withValues(alpha: 0.08),
      ),
      alignment: Alignment.center,
      child: s == null
          ? Text('-', style: T.code.copyWith(color: C.faint))
          : Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                RssiBar(s.rssi),
                const SizedBox(width: 4),
                Text('${s.rssi}', style: T.code.copyWith(fontSize: 11, color: col)),
              ],
            ),
    );
  }

  // ---- commands

  List<Widget> _jobs() {
    if (dash == null) {
      // on USB the phone runs the commands itself: its own log
      return [
        SectionHead('Commands', trailing: Text('this phone', style: T.meta)),
        ValueListenableBuilder(
          valueListenable: cmdLog,
          builder: (context, log, _) => log.isEmpty
              ? Panel(child: Text('Nothing sent yet.', style: T.meta))
              : Group(children: [for (final e in log.take(40)) _jobRow(e.label, e.target, e.ts, e.ok == null ? 'sending' : (e.ok! ? 'done' : 'failed'), e.detail, null)]),
        ),
      ];
    }
    return [
      SectionHead('Dashboard commands', trailing: Text('${jobs.length}', style: T.meta)),
      if (jobs.isEmpty)
        Panel(child: Text('No commands yet.', style: T.meta))
      else
        Group(
          children: [
            for (final j in jobs)
              _jobRow(
                '${j['label'] ?? j['op'] ?? ''}',
                '${j['target'] ?? ''}',
                (j['created'] as num?)?.toDouble() ?? 0,
                '${j['state'] ?? ''}',
                [
                  if ('${j['error'] ?? ''}'.isNotEmpty) '${j['error']}',
                  if (j['replies'] is Map && (j['replies'] as Map).isNotEmpty)
                    (j['replies'] as Map).entries.map((e) => '${App.I.m.nodeName('${e.key}')}: ${e.value is Map ? (e.value as Map)['text'] : e.value}').join(', '),
                ].join('  ·  '),
                (j['progress'] as num?)?.toInt(),
              ),
          ],
        ),
    ];
  }

  Widget _jobRow(String label, String target, double ts, String state, String detail, int? pct) {
    final c = switch (state) {
      'done' => C.green,
      'failed' => C.red,
      'partial' => C.amber,
      _ => C.accent,
    };
    final live = !const ['done', 'failed', 'partial'].contains(state);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(ts > 0 ? clock(ts) : '--:--', style: T.code.copyWith(fontSize: 11.5)),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(label, style: T.row.copyWith(fontSize: 14)),
                    const SizedBox(height: 2),
                    Text(
                      '${target == 'all' ? 'fleet' : App.I.m.nodeName(target)}${detail.isNotEmpty ? '  ·  $detail' : ''}',
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: T.meta,
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              Tag(state.isEmpty ? '?' : state, color: c),
            ],
          ),
          if (live && pct != null) ...[const SizedBox(height: 6), LinearProgressIndicator(value: pct.clamp(0, 100) / 100, minHeight: 3)],
        ],
      ),
    );
  }
}
