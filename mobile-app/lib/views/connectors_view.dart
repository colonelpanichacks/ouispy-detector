// CONNECTORS: where the dashboard sends the fleet's news (MQTT in, TAK and Reticulum out),
// each one's state with its switch, and the dashboard's data: how long history is kept,
// and clearing it. Set-up (hosts, certificates, passwords) stays on the dashboard: the
// phone never shows a secret, and never sends one back (it only ever posts the switch).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/local_mesh.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'meshtastic_view.dart';

class ConnectorsView extends StatefulWidget {
  const ConnectorsView({super.key});
  @override
  State<ConnectorsView> createState() => _ConnectorsViewState();
}

class _ConnectorsViewState extends State<ConnectorsView> {
  Map<String, dynamic>? cfg; // GET /api/settings (secrets come back as ********; never shown)
  Map<String, dynamic>? tak, rns, mtx;
  String? err;
  Timer? _poll;
  final retention = TextEditingController();
  bool retentionDirty = false;

  bool get onUsb => App.I.link is LocalMesh;

  @override
  void initState() {
    super.initState();
    if (!onUsb) {
      _load();
      _poll = Timer.periodic(const Duration(seconds: 6), (_) => _status());
    }
  }

  @override
  void dispose() {
    _poll?.cancel();
    retention.dispose();
    super.dispose();
  }

  Map<String, dynamic>? _map(Object? j) => j is Map ? j.cast<String, dynamic>() : null;

  Future<void> _load() async {
    final j = _map(await App.I.dash.getJson('/api/settings'));
    if (!mounted) return;
    setState(() {
      if (j != null) {
        cfg = j;
        err = null;
        if (!retentionDirty) retention.text = '${j['retention_days'] ?? 0}';
      } else {
        err = App.I.dash.reachable ? 'The dashboard did not answer /api/settings.' : 'Not connected to the dashboard.';
      }
    });
    await _status();
  }

  Future<void> _status() async {
    final r = await Future.wait([App.I.dash.getJson('/api/tak'), App.I.dash.getJson('/api/rns'), App.I.dash.getJson('/api/meshtastic')]);
    if (!mounted) return;
    setState(() {
      tak = _map(r[0]);
      rns = _map(r[1]);
      mtx = _map(r[2]);
    });
  }

  /// Flip one connector: only {enabled}, so the dashboard keeps every other field (and its secrets).
  Future<void> _enable(String key, bool v) async {
    final (r, why) = await App.I.dash.postChecked('/api/settings', {
      key: {'enabled': v},
    });
    if (!mounted) return;
    if (r == null) {
      App.I.m.toast('Not changed: $why');
    } else {
      setState(() => cfg = r);
      App.I.m.toast('${_name(key)} ${v ? 'on' : 'off'}');
    }
    Future.delayed(const Duration(milliseconds: 800), _status);
  }

  String _name(String k) => switch (k) {
    'mqtt' => 'MQTT',
    'tak' => 'TAK',
    'rns' => 'Reticulum',
    _ => k,
  };

  Future<void> _saveRetention() async {
    final n = int.tryParse(retention.text.trim());
    if (n == null || n < 0) {
      App.I.m.toast('Days: a whole number, 0 keeps everything');
      return;
    }
    final (r, why) = await App.I.dash.postChecked('/api/settings', {'retention_days': n});
    if (!mounted) return;
    if (r == null) {
      App.I.m.toast('Not saved: $why');
      return;
    }
    setState(() {
      cfg = r;
      retentionDirty = false;
      retention.text = '${r['retention_days'] ?? n}';
    });
    App.I.m.toast(n == 0 ? 'History kept forever' : 'History kept $n days');
  }

  Future<void> _clear() async {
    if (!await confirm(
      context,
      'Clear detection history?',
      'Every detection the dashboard has stored, from every node, is deleted. Devices, names, groups and drones stay.',
      yes: 'Continue',
    )) {
      return;
    }
    if (!mounted) return;
    if (!await confirm(context, 'Delete it all?', 'This cannot be undone. Every open dashboard drops them too.', yes: 'Delete history')) return;
    final (r, why) = await App.I.dash.postChecked('/api/detections/clear', {});
    if (r == null) {
      App.I.m.toast('Not cleared: $why');
    } else {
      HapticFeedback.mediumImpact();
      App.I.m.toast('Cleared ${r['n'] ?? 0} detections');
    }
  }

  @override
  Widget build(BuildContext context) => ReconPage(
    title: 'Connectors',
    actions: [
      if (!onUsb)
        IconButton(
          icon: const Icon(Ph.arrowsClockwise, size: 19, color: C.muted),
          onPressed: _load,
        ),
    ],
    body: onUsb
        ? const Empty(
            'Needs the dashboard',
            body:
                'MQTT, TAK, Reticulum and Meshtastic run on the home dashboard. With a node on this phone\'s USB the app is the dashboard, and has none of them. Switch Source to Dashboard in Settings to see them.',
            icon: Ph.plugs,
          )
        : cfg == null
        ? (err == null ? const Center(child: CircularProgressIndicator()) : Empty('No settings from the dashboard', body: err, icon: Ph.plugs, color: C.amber))
        : ListenableBuilder(listenable: App.I.m, builder: (context, _) => _body()),
  );

  Map<String, dynamic> _c(String k) => _map(cfg?[k]) ?? const {};

  Widget _body() {
    final mq = _c('mqtt'), tk = _c('tak'), rn = _c('rns');
    final mqSrc = App.I.m.sources['mqtt'];
    return ListView(
      padding: const EdgeInsets.only(bottom: 40),
      children: [
        // ---- MQTT: detections in from nodes that publish over Wi-Fi
        _connector(
          'MQTT',
          Ph.wifiHigh,
          'Detections in from nodes on Wi-Fi',
          mq['enabled'] == true,
          (v) => _enable('mqtt', v),
          state: mq['enabled'] != true
              ? null
              : mqSrc == null
              ? ('Unknown', C.muted)
              : (mqSrc['connected'] == true ? ('Connected', C.green) : ('Down', C.red)),
          rows: [
            if ('${mq['host'] ?? ''}'.isNotEmpty) KV('Broker', '${mq['host']}:${mq['port'] ?? 1883}', monoValue: true),
            if ('${mq['topic'] ?? ''}'.isNotEmpty) KV('Topic', '${mq['topic']}', monoValue: true),
            if (mqSrc != null && '${mqSrc['detail'] ?? ''}'.isNotEmpty) KV('Detail', '${mqSrc['detail']}'),
          ],
        ),
        // ---- TAK: the fleet on a TAK server's map
        _connector(
          'TAK',
          Ph.mapPin,
          'Nodes, drones and pilots to a TAK server',
          tk['enabled'] == true,
          (v) => _enable('tak', v),
          state: tk['enabled'] != true ? null : _st('${tak?['state'] ?? ''}'),
          rows: [
            if ('${tk['host'] ?? ''}'.isNotEmpty) KV('Server', '${tk['proto'] ?? 'tcp'}://${tk['host']}:${tk['port'] ?? 8087}', monoValue: true),
            if ('${tk['callsign'] ?? ''}'.isNotEmpty) KV('Callsign', '${tk['callsign']}'),
            if (tak != null) KV('Sent', '${tak!['sent'] ?? 0} events${tak!['last'] is num ? ', last ${agoText((tak!['last'] as num).toDouble())}' : ''}'),
            if ('${tak?['error'] ?? ''}'.isNotEmpty) KV('Last error', '${tak!['error']}', color: C.red),
          ],
        ),
        // ---- Reticulum: alerts as LXMF messages
        _connector(
          'Reticulum',
          Ph.chatsCircle,
          'Alerts as LXMF messages',
          rn['enabled'] == true,
          (v) => _enable('rns', v),
          state: rn['enabled'] != true ? null : _st('${rns?['state'] ?? ''}'),
          rows: [
            if ('${rns?['address'] ?? ''}'.isNotEmpty) KV('Our address', '${rns!['address']}', monoValue: true),
            if (rns != null) KV('Messages', '${rns!['sent'] ?? 0} sent, ${rns!['failed'] ?? 0} failed, ${rns!['queued'] ?? 0} waiting'),
            if ('${rns?['error'] ?? ''}'.isNotEmpty) KV('Last error', '${rns!['error']}', color: C.red),
          ],
        ),
        // ---- Meshtastic: its own page
        Group(
          children: [
            Builder(
              builder: (context) {
                final on = (_map(mtx?['config'])?['enabled']) == true;
                final (w, c) = linkState('${mtx?['state'] ?? ''}', on);
                return Tile(
                  icon: Ph.broadcast,
                  color: on ? c : C.muted,
                  title: 'Meshtastic',
                  subtitle: Text(
                    mtx == null ? 'Status, inbox and settings' : '$w  ·  ${mtx!['sent'] ?? 0} sent, ${mtx!['heard'] ?? 0} heard',
                    style: T.meta.copyWith(color: on ? c : null),
                  ),
                  onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const MeshtasticView())).then((_) => _status()),
                );
              },
            ),
          ],
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 2, 20, 4),
          child: Text(
            'Hosts, certificates and passwords are set on the dashboard\'s Settings tab. This page only turns each connector on or off.',
            style: T.meta.copyWith(height: 1.4),
          ),
        ),
        // ---- data
        Group(
          title: 'Data',
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
              child: Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: retention,
                      keyboardType: TextInputType.number,
                      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                      style: T.row,
                      onChanged: (_) => setState(() => retentionDirty = true),
                      decoration: const InputDecoration(labelText: 'Keep history (days)', hintText: '0 keeps everything'),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Btn('Save', icon: Ph.floppyDisk, onTap: retentionDirty ? _saveRetention : null),
                ],
              ),
            ),
            Tile(
              icon: Ph.trash,
              color: C.red,
              title: 'Clear detection history',
              titleColor: C.red,
              subtitle: Text('On the dashboard, for every node', style: T.meta),
              onTap: _clear,
            ),
          ],
        ),
      ],
    );
  }

  (String, Color) _st(String s) => switch (s) {
    'connected' || 'ready' => ('Connected', C.green),
    'connecting' => ('Connecting', C.amber),
    'error' => ('Error', C.red),
    'off' => ('Off', C.muted),
    '' => ('Unknown', C.muted),
    _ => (s, C.muted),
  };

  Widget _connector(String name, IconData icon, String what, bool on, ValueChanged<bool> flip, {(String, Color)? state, List<Widget> rows = const []}) {
    final (sw, sc) = state ?? ('Off', C.muted);
    return Group(
      title: name,
      trailing: Row(
        children: [
          Dot(sc, size: 7, halo: sc == C.green),
          const SizedBox(width: 6),
          Text(sw, style: T.meta.copyWith(color: sc)),
        ],
      ),
      edge: on && sc == C.red ? C.red : null,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 6, 10, 6),
          child: Row(
            children: [
              IconTile(icon, on ? C.accent : C.muted),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('Enabled', style: T.row),
                    const SizedBox(height: 2),
                    Text(what, style: T.meta),
                  ],
                ),
              ),
              Switch(value: on, onChanged: flip),
            ],
          ),
        ),
        ...rows,
      ],
    );
  }
}
