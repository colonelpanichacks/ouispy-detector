// MESHTASTIC: the dashboard's Meshtastic link (home-dashboard/mtastic.py). Its state, the
// radio and channel, what it sent, what it heard, a test message, and its settings: the
// radio's port, the channel, who gets direct messages and what it sends. It lives on the
// dashboard; a node on the phone's USB has no Meshtastic radio of its own (yet).
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/local_mesh.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

/// The link's state as a word and a colour.
(String, Color) linkState(String st, bool enabled) {
  if (!enabled) return ('Off', C.muted);
  return switch (st) {
    'ready' || 'connected' => ('Ready', C.green),
    'connecting' => ('Connecting', C.amber),
    'error' => ('Error', C.red),
    'off' => ('Starting', C.amber),
    _ => (st.isEmpty ? 'Unknown' : st, C.muted),
  };
}

class MeshtasticView extends StatefulWidget {
  const MeshtasticView({super.key});
  @override
  State<MeshtasticView> createState() => _MeshtasticViewState();
}

class _MeshtasticViewState extends State<MeshtasticView> {
  Map<String, dynamic>? st;
  String? err;
  Timer? _poll;
  bool saving = false, dirty = false;
  // the editable copy of the settings
  bool enabled = false;
  int channel = 1;
  final port = TextEditingController();
  final recipients = TextEditingController();
  Map<String, bool> send = {'new_devices': true, 'drones': true, 'lost_nodes': true, 'nodes': false};

  bool get onUsb => App.I.link is LocalMesh;

  @override
  void initState() {
    super.initState();
    if (!onUsb) {
      _load(fill: true);
      _poll = Timer.periodic(const Duration(seconds: 5), (_) => _load());
    }
  }

  @override
  void dispose() {
    _poll?.cancel();
    port.dispose();
    recipients.dispose();
    super.dispose();
  }

  Future<void> _load({bool fill = false}) async {
    final j = await App.I.dash.getJson('/api/meshtastic');
    if (!mounted) return;
    setState(() {
      if (j is Map) {
        st = j.cast<String, dynamic>();
        err = null;
        if (fill || !dirty) _fill();
      } else {
        err = App.I.dash.reachable ? 'The dashboard did not answer /api/meshtastic (an older dashboard?).' : 'Not connected to the dashboard.';
      }
    });
  }

  void _fill() {
    final c = (st?['config'] as Map?)?.cast<String, dynamic>() ?? const {};
    enabled = c['enabled'] == true;
    channel = (c['channel'] as num?)?.toInt() ?? 1;
    port.text = '${c['port'] ?? ''}';
    recipients.text = '${c['recipients'] ?? ''}';
    final sd = (c['send'] as Map?) ?? const {};
    for (final k in send.keys.toList()) {
      if (sd[k] is bool) send[k] = sd[k] as bool;
    }
    dirty = false;
  }

  Future<void> _save() async {
    setState(() => saving = true);
    final (r, why) = await App.I.dash.postChecked('/api/settings', {
      'meshtastic': {'enabled': enabled, 'port': port.text.trim(), 'channel': channel, 'recipients': recipients.text.trim(), 'send': send},
    });
    if (!mounted) return;
    setState(() => saving = false);
    if (r == null) {
      App.I.m.toast('Not saved: $why');
      return;
    }
    dirty = false;
    App.I.m.toast('Meshtastic settings saved');
    _load(fill: true);
  }

  Future<void> _test() async {
    if (!await confirm(
      context,
      'Send a test?',
      'The dashboard sends "OUI-SPY Mesh: test message from the dashboard." on channel $channel${recipients.text.trim().isEmpty ? '' : ' and to each recipient'}. Everyone on that channel sees it.',
      yes: 'Send',
      color: C.accent,
    )) {
      return;
    }
    final (r, why) = await App.I.dash.postChecked('/api/meshtastic/test', {});
    App.I.m.toast(r == null ? 'Test not sent: $why' : 'Test queued');
    _load();
  }

  Future<void> _pickPort() async {
    final j = await App.I.dash.getJson('/api/ports');
    if (!mounted || j is! List) return;
    final pick = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const SectionHead('Ports on the dashboard'),
            if (j.isEmpty)
              Padding(
                padding: const EdgeInsets.all(16),
                child: Text('No serial ports.', style: T.meta),
              ),
            for (final p in j)
              if (p is Map)
                Tile(
                  icon: Ph.usb,
                  color: p['espressif'] == true ? C.muted : C.accent,
                  title: '${p['device']}',
                  subtitle: Text(
                    [if (p['espressif'] == true) 'an ESP32 (likely one of our nodes)', '${p['desc'] ?? ''}'].where((x) => x.isNotEmpty).join('  '),
                    style: T.meta,
                  ),
                  onTap: () => Navigator.pop(c, '${p['device']}'),
                ),
          ],
        ),
      ),
    );
    if (pick != null) {
      setState(() {
        port.text = pick;
        dirty = true;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return ReconPage(
      title: 'Meshtastic',
      actions: [
        if (!onUsb)
          IconButton(
            icon: const Icon(Ph.arrowsClockwise, size: 19, color: C.muted),
            onPressed: () => _load(),
          ),
      ],
      body: onUsb
          ? const Empty(
              'Needs the dashboard for now',
              body:
                  'The Meshtastic link runs on the home dashboard, which talks to the radio on its own USB. With a node on this phone\'s USB the app is the dashboard, and talking to a Meshtastic radio straight from the phone is a later piece. Switch Source to Dashboard in Settings to see it.',
              icon: Ph.broadcast,
            )
          : st == null
          ? (err == null ? const Center(child: CircularProgressIndicator()) : Empty('No Meshtastic status', body: err, icon: Ph.broadcast, color: C.amber))
          : _body(),
    );
  }

  Widget _body() {
    final s = st!;
    final cfgOn = ((s['config'] as Map?)?['enabled']) == true;
    final (word, c) = linkState('${s['state'] ?? ''}', cfgOn);
    final error = '${s['error'] ?? ''}';
    final inbox = [
      for (final e in (s['inbox'] as List? ?? const []))
        if (e is Map) e.cast<String, dynamic>(),
    ]..sort((a, b) => ((b['ts'] as num?) ?? 0).compareTo((a['ts'] as num?) ?? 0));
    String n(String k) => s[k] == null ? '-' : '${s[k]}';
    return ListView(
      padding: const EdgeInsets.only(bottom: 40),
      children: [
        SectionHead(
          'Link',
          trailing: Row(
            children: [
              Dot(c, size: 7, halo: c == C.green),
              const SizedBox(width: 6),
              Text(word, style: T.meta.copyWith(color: c)),
            ],
          ),
        ),
        Panel(
          margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          padding: EdgeInsets.zero,
          edge: error.isNotEmpty && cfgOn ? C.red : null,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              KV('Radio', '${s['radio'] ?? ''}'.isEmpty ? '-' : '${s['radio']}', monoValue: true),
              KV('Channel', '${s['channel'] ?? ''}'.isEmpty ? '-' : '${s['channel']}'),
              IntrinsicHeight(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(child: Stat('Sent', n('sent'), color: C.green)),
                    Expanded(child: Stat('Failed', n('failed'), color: (s['failed'] as num? ?? 0) > 0 ? C.red : C.text)),
                    Expanded(child: Stat('Queued', n('queued'), color: (s['queued'] as num? ?? 0) > 0 ? C.amber : C.text)),
                    Expanded(child: Stat('Heard', n('heard'))),
                  ],
                ),
              ),
              if (s['last'] is num) KV('Last sent', '${clock((s['last'] as num).toDouble(), date: true)}  (${agoText((s['last'] as num).toDouble())})'),
              if (error.isNotEmpty) KV('Last error', error, color: C.red),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 14),
                child: Btn('Send a test', icon: Ph.paperPlaneTilt, expand: true, onTap: cfgOn ? _test : null),
              ),
            ],
          ),
        ),
        SectionHead('Inbox', trailing: Text('${inbox.length}', style: T.code)),
        if (inbox.isEmpty)
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 0, 20, 8),
            child: Text('Nothing heard yet. Text messages on the channel land here.', style: T.meta),
          )
        else
          Group(
            children: [
              for (final e in inbox)
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Expanded(
                            child: Text(
                              '${e['name'] ?? ''}'.isNotEmpty ? '${e['name']}' : '${e['from'] ?? '?'}',
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: T.row.copyWith(fontSize: 14),
                            ),
                          ),
                          const SizedBox(width: 8),
                          Text(e['ts'] is num ? agoText((e['ts'] as num).toDouble()) : '', style: T.code.copyWith(fontSize: 11.5)),
                        ],
                      ),
                      if ('${e['name'] ?? ''}'.isNotEmpty) Text('${e['from'] ?? ''}', style: T.code.copyWith(fontSize: 11.5)),
                      const SizedBox(height: 4),
                      SelectableText('${e['text'] ?? ''}', style: T.body),
                    ],
                  ),
                ),
            ],
          ),
        Group(
          title: 'Settings',
          children: [
            _sw(Ph.power, 'Enabled', 'Send alerts over the Meshtastic mesh', enabled, (v) => enabled = v),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 6),
              child: TextField(
                controller: port,
                style: T.code.copyWith(color: C.text, fontSize: 13.5),
                onChanged: (_) => setState(() => dirty = true),
                decoration: InputDecoration(
                  labelText: 'Radio port',
                  hintText: '/dev/ttyUSB0, or tcp:<host>',
                  suffixIcon: IconButton(
                    icon: const Icon(Ph.list, size: 18, color: C.muted),
                    onPressed: _pickPort,
                    tooltip: 'Ports',
                  ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('CHANNEL', style: T.silk),
                  const SizedBox(height: 8),
                  Seg<int>(
                    items: [for (var i = 0; i < 8; i++) (i, '$i')],
                    value: channel,
                    onChanged: (v) => setState(() {
                      channel = v;
                      dirty = true;
                    }),
                  ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 6, 16, 12),
              child: TextField(
                controller: recipients,
                style: T.code.copyWith(color: C.text, fontSize: 13.5),
                onChanged: (_) => setState(() => dirty = true),
                decoration: const InputDecoration(labelText: 'Direct-message recipients', hintText: '!a1b2c3d4 !e5f60718 (blank: the channel only)'),
              ),
            ),
            _sw(Ph.broadcast, 'New devices', 'A batch a minute', send['new_devices']!, (v) => send['new_devices'] = v),
            _sw(Ph.drone, 'Drones', 'When one is first heard', send['drones']!, (v) => send['drones'] = v),
            _sw(Ph.warning, 'Lost nodes', 'A lost node seen again', send['lost_nodes']!, (v) => send['lost_nodes'] = v),
            _sw(Ph.graph, 'Nodes', 'Nodes joining and dropping', send['nodes']!, (v) => send['nodes'] = v),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 6, 16, 14),
              child: Row(
                children: [
                  Expanded(
                    child: Btn(saving ? 'Saving' : 'Save', icon: Ph.floppyDisk, kind: BtnKind.filled, expand: true, onTap: saving || !dirty ? null : _save),
                  ),
                  if (dirty) ...[
                    const SizedBox(width: 8),
                    Btn('Undo', icon: Ph.arrowCounterClockwise, kind: BtnKind.ghost, color: C.muted, onTap: () => setState(_fill)),
                  ],
                ],
              ),
            ),
          ],
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
          child: Text(
            'Meshtastic airtime is shared by every radio in range: the dashboard batches new devices and spaces its messages out. Pick a channel the fleet\'s Meshtastic radios share.',
            style: T.meta.copyWith(height: 1.45),
          ),
        ),
      ],
    );
  }

  Widget _sw(IconData i, String t, String sub, bool v, void Function(bool) f) => Padding(
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
        Switch(
          value: v,
          onChanged: (x) => setState(() {
            f(x);
            dirty = true;
          }),
        ),
      ],
    ),
  );
}
