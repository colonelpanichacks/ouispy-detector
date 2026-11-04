// FLEET CONTROL: the dashboard's Fleet Control tab (app.js renderFleetCtl) for the whole
// fleet at once: read all, identify all, buzzer / LED for every remote, reboot remotes,
// re-channel, push the home's filters, and the wardrivers' channel plans. Every command
// goes through Backend.nodeCmd (USB: the node's console; dashboard: its REST), and its
// result lands in the command log at the bottom.
import 'package:flutter/material.dart';

import '../data/local_mesh.dart';
import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

/// One command and how it went.
class CmdLogEntry {
  CmdLogEntry(this.ts, this.target, this.label);
  final double ts;
  final String target, label;
  bool? ok; // null while waiting
  String detail = '';
}

/// The command log, kept for the app's lifetime (the screen can close mid-command).
final cmdLog = ValueNotifier<List<CmdLogEntry>>([]);

/// Runs one node op and logs its result: true when the backend took it.
Future<bool> runLogged(String id, String op, [Map<String, dynamic>? args, String? label]) async {
  final e = CmdLogEntry(DateTime.now().millisecondsSinceEpoch / 1000, id, label ?? op);
  cmdLog.value = [e, ...cmdLog.value.take(79)];
  Map<String, dynamic>? r;
  try {
    r = await App.I.link.nodeCmd(id, op, args);
  } catch (err) {
    e.detail = '$err';
  }
  e.ok = r != null && r['ok'] != false;
  if (e.detail.isEmpty) {
    final job = r?['job'];
    e.detail = r == null ? 'no answer' : '${r['error'] ?? r['reply'] ?? (job is Map ? job['label'] ?? job['state'] ?? '' : '')}';
  }
  cmdLog.value = [...cmdLog.value];
  return e.ok!;
}

/// A fleet-wide op (target `all`). The dashboard backend must route `all` to /api/fleet/cmd.
Future<bool> fleetCmd(String op, [Map<String, dynamic>? args, String? label]) => runLogged('all', op, args, label);

/// A wardrive channel plan as commands.py takes it: all, 6, 1-6, 1,6,11, 1-3,11 (1-13).
String? checkPlan(String s) {
  final v = s.replaceAll(' ', '');
  if (v == 'all') return null;
  if (!RegExp(r'^(\d{1,2}(-\d{1,2})?)(,\d{1,2}(-\d{1,2})?)*$').hasMatch(v)) return 'channels: all, 6, 1-6, 1,6,11 or 1-3,11';
  for (final part in v.split(',')) {
    final ab = part.split('-').map(int.parse).toList();
    if (ab.any((c) => c < 1 || c > 13)) return 'channels are 1 to 13';
    if (ab.length == 2 && ab[0] > ab[1]) return '$part runs backwards';
  }
  return null;
}

/// 1-11 cut into n contiguous runs, the first ones a channel longer (3 -> 1-4, 5-8, 9-11).
List<String> splitChannels(int n) {
  n = n.clamp(1, 11);
  final base = 11 ~/ n, rem = 11 % n;
  final out = <String>[];
  var c = 1;
  for (var i = 0; i < n; i++) {
    final len = base + (i < rem ? 1 : 0);
    final end = c + len - 1;
    out.add(len == 1 ? '$c' : '$c-$end');
    c = end + 1;
  }
  return out;
}

const _plans = [
  ('1-11', '1-11 hop (US band)'),
  ('all', '1-13 hop (all)'),
  ('1,6,11', '1, 6, 11'),
  ('1-6', '1-6'),
  ('7-11', '7-11'),
  ('1', '1 only'),
  ('6', '6 only'),
  ('11', '11 only'),
];

bool isWardriverNode(FleetNode n) => n.kind.contains('wardriver');

String _wdch(FleetNode n) => '${n.cfg['wdch'] ?? n.tlm['wdch'] ?? ''}';

class FleetControlView extends StatefulWidget {
  const FleetControlView({super.key});
  @override
  State<FleetControlView> createState() => _FleetControlViewState();
}

class _FleetControlViewState extends State<FleetControlView> {
  bool buzzer = true, led = true;
  int chan = 0; // 0 = not picked yet: follow the home's
  final selWd = <String>{};
  final raw = <String, bool>{};

  FleetNode? get _home {
    for (final n in App.I.m.nodes.values) {
      if (n.isHome) return n;
    }
    return null;
  }

  Future<void> _readAll() async {
    final h = _home;
    await Future.wait([fleetCmd('status', null, 'Read status (all)'), if (h != null) runLogged(h.id, 'status', null, 'Read status (home)')]);
  }

  Future<void> _rebootAll() async {
    if (!await confirm(context, 'Reboot every remote?', 'Every remote node restarts and drops off the mesh for a few seconds. The home stays up.', yes: 'Reboot all')) {
      return;
    }
    await fleetCmd('reboot', null, 'Reboot all remotes');
  }

  Future<void> _rechan(int ch) async {
    if (!await confirm(
      context,
      'Move the fleet to channel $ch?',
      'The home tells every remote to switch to channel $ch, then reboots onto it; the remotes follow and keep the new channel. '
          'A node that misses the order stays behind until its channel auto-heal finds the fleet (about 2 minutes).',
      yes: 'Re-channel',
      color: C.amber,
    )) {
      return;
    }
    await fleetCmd('rechan', {'ch': ch}, 'Re-channel to $ch');
  }

  Future<void> _push() async {
    if (!await confirm(
      context,
      "Push the home's filters to every node?",
      "Every remote's filter list is replaced with the home's, and each remote reboots to apply it.",
      yes: 'Push',
      color: C.violet,
    )) {
      return;
    }
    await fleetCmd('meshpush', null, 'Push home filters');
  }

  /// wardrive {ch} or {raw}. The USB engine's wardrive op has no `raw`: that goes as a console line.
  Future<bool> _wd(FleetNode n, {String? ch, bool? raw}) {
    final link = App.I.link;
    if (raw != null && link is LocalMesh) {
      final w = 'raw ${raw ? 'on' : 'off'}';
      final line = n.id == link.homeId ? 'wd $w' : 'rwd ${n.id} $w';
      return runLogged(n.id, 'raw', {'line': line}, 'Wardriver ${n.label}: $w');
    }
    return runLogged(n.id, 'wardrive', {'ch': ?ch, 'raw': ?raw}, 'Wardriver ${n.label}: ${ch != null ? 'ch $ch' : 'raw ${raw! ? 'on' : 'off'}'}');
  }

  Future<void> _pickPlan(FleetNode n) async {
    final v = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            SectionHead('Channel plan · ${n.label}'),
            for (final (p, l) in _plans)
              ListTile(
                title: Text(l, style: T.row),
                trailing: _wdch(n) == p ? const Icon(Ph.check, color: C.accent, size: 18) : null,
                onTap: () => Navigator.pop(c, p),
              ),
            ListTile(
              leading: const Icon(Ph.pencilSimple, color: C.muted, size: 18),
              title: const Text('Custom...', style: T.row),
              onTap: () => Navigator.pop(c, 'custom'),
            ),
          ],
        ),
      ),
    );
    if (v == null || !mounted) return;
    var plan = v;
    if (v == 'custom') {
      final t = await askText(context, 'Channels for ${n.label}', _wdch(n), hint: 'e.g. 1-3,11');
      if (t == null || t.isEmpty) return;
      final err = checkPlan(t);
      if (err != null) {
        App.I.m.toast(err);
        return;
      }
      plan = t.replaceAll(' ', '');
    }
    await _wd(n, ch: plan);
  }

  Future<void> _split(List<FleetNode> wds) async {
    final pick = wds.where((n) => selWd.contains(n.id)).toList();
    if (pick.isEmpty) return;
    if (pick.length > 11) {
      App.I.m.toast('at most 11 wardrivers: one channel each');
      return;
    }
    final plans = splitChannels(pick.length);
    if (!await confirm(
      context,
      'Split 1-11 across ${pick.length} wardrivers?',
      [for (final (i, n) in pick.indexed) '${n.label}: ${plans[i]}'].join('\n'),
      yes: 'Apply',
      color: C.accent,
    )) {
      return;
    }
    for (final (i, n) in pick.indexed) {
      await _wd(n, ch: plans[i]);
    }
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ReconPage(
      title: 'FLEET CONTROL',
      actions: const [],

      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final nodes = m.nodes.values.toList();
          final remotes = nodes.where((n) => !n.isHome).toList();
          final online = remotes.where((n) => n.online).length;
          final wds = nodes.where(isWardriverNode).toList()..sort((a, b) => a.label.compareTo(b.label));
          final homeCh = (m.home['ch'] as num?)?.toInt() ?? _home?.ch;
          final ch = chan > 0 ? chan : (homeCh != null && homeCh >= 1 && homeCh <= 11 ? homeCh : 1);
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: ActionStrip([
                  ('Read all', Ph.arrowsClockwise, C.green, _readAll),
                  ('Identify', Ph.lightbulb, C.cyan, () => fleetCmd('identify', {'secs': 10}, 'Identify all (10 s)')),
                  (
                    'Filters',
                    Ph.funnel,
                    C.violet,
                    () {
                      final h = _home;
                      fleetCmd('filters_get', null, 'Read filters (all)');
                      if (h != null) runLogged(h.id, 'filters_get', null, 'Read filters (home)');
                    },
                  ),
                ]),
              ),
              SectionHead('Remotes', trailing: Text('$online of ${remotes.length} online', style: T.meta)),
              Group(
                children: [
                  _switch(Ph.speakerHigh, 'Buzzer', 'Every remote beeps on a detection', buzzer, (v) => setState(() => buzzer = v)),
                  _switch(Ph.lightbulb, 'Alert LED', 'Every remote flashes on a detection', led, (v) => setState(() => led = v)),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 8, 16, 12),
                    child: Btn(
                      'Apply to all remotes',
                      icon: Ph.check,
                      expand: true,
                      onTap: () => fleetCmd('audio', {'buzzer': buzzer, 'led': led}, 'Audio all: buzzer ${buzzer ? 'on' : 'off'}, LED ${led ? 'on' : 'off'}'),
                    ),
                  ),
                ],
              ),
              Group(
                title: 'Fleet',
                children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            const IconTile(Ph.broadcast, C.amber),
                            const SizedBox(width: 12),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  const Text('Mesh channel', style: T.row),
                                  const SizedBox(height: 2),
                                  Text('Now ${homeCh ?? '-'}. The home reboots onto the new one; the remotes follow.', style: T.meta),
                                ],
                              ),
                            ),
                          ],
                        ),
                        const SizedBox(height: 10),
                        SingleChildScrollView(
                          scrollDirection: Axis.horizontal,
                          child: Row(
                            children: [
                              for (var c = 1; c <= 11; c++)
                                Padding(
                                  padding: const EdgeInsets.only(right: 6),
                                  child: Chip2('$c', on: c == ch, color: C.amber, onTap: () => setState(() => chan = c)),
                                ),
                            ],
                          ),
                        ),
                        const SizedBox(height: 10),
                        Btn('Re-channel to $ch', icon: Ph.broadcast, color: C.amber, expand: true, onTap: ch == homeCh ? null : () => _rechan(ch)),
                      ],
                    ),
                  ),
                  Tile(
                    icon: Ph.uploadSimple,
                    color: C.violet,
                    title: "Push the home's filters",
                    subtitle: Text('Every node gets the home\'s list (meshpush)', style: T.meta),
                    onTap: _push,
                  ),
                  Tile(icon: Ph.power, color: C.red, title: 'Reboot all remotes', titleColor: C.red, onTap: _rebootAll),
                ],
              ),
              SectionHead('Wardrivers', trailing: Text('${wds.length}', style: T.meta)),
              if (wds.isEmpty)
                Panel(child: Text('No wardriver nodes in the fleet. A node running wardriver firmware (build stamp ending W or Y) shows here.', style: T.meta))
              else ...[
                for (final n in wds) _wdCard(n),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
                  child: Btn(
                    selWd.isEmpty ? 'Tick wardrivers to split 1-11' : 'Split 1-11 across ${selWd.length}: ${splitChannels(selWd.length).join('  ')}',
                    icon: Ph.stack,
                    expand: true,
                    onTap: selWd.isEmpty ? null : () => _split(wds),
                  ),
                ),
              ],
              SectionHead(
                'Command log',
                trailing: GestureDetector(
                  onTap: () => cmdLog.value = [],
                  child: Text('CLEAR', style: T.silk.copyWith(color: C.muted)),
                ),
              ),
              ValueListenableBuilder(
                valueListenable: cmdLog,
                builder: (context, log, _) => log.isEmpty ? Panel(child: Text('Nothing sent yet.', style: T.meta)) : Group(children: [for (final e in log) _logRow(e)]),
              ),
            ],
          );
        },
      ),
    );
  }

  Widget _wdCard(FleetNode n) {
    final plan = _wdch(n);
    final st = n.online ? C.green : (n.silent ? C.amber : C.red);
    final wd = n.tlm['wd'];
    final r = raw[n.id] ?? false;
    return Panel(
      padding: const EdgeInsets.fromLTRB(6, 6, 12, 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Checkbox(value: selWd.contains(n.id), onChanged: (v) => setState(() => v == true ? selWd.add(n.id) : selWd.remove(n.id))),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Dot(st, size: 7, halo: n.online),
                        const SizedBox(width: 6),
                        Flexible(
                          child: Text(n.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                        ),
                        const SizedBox(width: 8),
                        Text(n.id, style: T.code),
                      ],
                    ),
                    const SizedBox(height: 3),
                    Text([plan.isEmpty ? 'plan not reported' : 'ch $plan', if (wd is List && wd.length >= 2) '${wd[0]} APs, ${wd[1]} BLE'].join('  ·  '), style: T.meta),
                  ],
                ),
              ),
            ],
          ),
          Padding(
            padding: const EdgeInsets.only(left: 10, top: 4),
            child: Row(
              children: [
                Expanded(
                  child: Btn('Channel plan', icon: Ph.sliders, expand: true, onTap: () => _pickPlan(n)),
                ),
                const SizedBox(width: 10),
                Text('RAW', style: T.silk.copyWith(color: r ? C.accent : C.silk)),
                Switch(
                  value: r,
                  onChanged: (v) async {
                    setState(() => raw[n.id] = v);
                    if (!await _wd(n, raw: v) && mounted) setState(() => raw[n.id] = !v);
                  },
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _logRow(CmdLogEntry e) {
    final (c, s) = switch (e.ok) {
      null => (C.amber, 'SENDING'),
      true => (C.green, 'OK'),
      false => (C.red, 'FAILED'),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(clock(e.ts), style: T.code.copyWith(fontSize: 11.5)),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(e.label, style: T.row.copyWith(fontSize: 14)),
                const SizedBox(height: 2),
                Text(
                  '${e.target == 'all' ? 'fleet' : App.I.m.nodeName(e.target)}${e.detail.isNotEmpty ? '  ·  ${e.detail}' : ''}',
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: T.meta,
                ),
              ],
            ),
          ),
          const SizedBox(width: 8),
          Tag(s, color: c),
        ],
      ),
    );
  }

  Widget _switch(IconData i, String t, String sub, bool v, ValueChanged<bool> f) => Padding(
    padding: const EdgeInsets.fromLTRB(16, 6, 10, 6),
    child: Row(
      children: [
        IconTile(i, v ? C.accent : C.muted),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(t, style: T.row),
              const SizedBox(height: 2),
              Text(sub, style: T.meta),
            ],
          ),
        ),
        Switch(value: v, onChanged: f),
      ],
    ),
  );
}
