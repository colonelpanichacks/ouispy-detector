// SETTINGS: where the dashboard is (found on the LAN or typed in), the access token, how
// alerts reach you, the offline map cache and app updates.
import 'package:flutter/material.dart';

import '../data/dashboard.dart';
import '../data/model.dart';
import '../data/tiles.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

const appVersion = '0.1.0';
const appBuild = 1;

class SettingsView extends StatefulWidget {
  const SettingsView({super.key});
  @override
  State<SettingsView> createState() => _SettingsViewState();
}

class _SettingsViewState extends State<SettingsView> {
  final s = App.I.s;
  late final host = TextEditingController(text: s.host);
  late final port = TextEditingController(text: '${s.port}');
  late final token = TextEditingController(text: s.token);
  bool showToken = false;
  List<Discovered>? found;
  bool scanning = false;
  (int, int)? cache;
  String update = '';

  @override
  void initState() {
    super.initState();
    _usage();
  }

  Future<void> _usage() async {
    final u = await TileCache.I.usage();
    if (mounted) setState(() => cache = u);
  }

  Future<void> _scan() async {
    setState(() => scanning = true);
    final f = await discoverDashboards(wait: const Duration(seconds: 5));
    if (mounted) {
      setState(() {
        found = f;
        scanning = false;
      });
    }
  }

  Future<void> _apply() async {
    s.host = host.text.trim();
    s.port = int.tryParse(port.text.trim()) ?? 5077;
    s.token = token.text.trim();
    await s.save();
    App.I.m.clearData();
    await App.I.dash.start();
    if (mounted) App.I.m.toast('reconnecting');
  }

  // Over-the-air app updates come from the dashboard too (nothing from a store or the
  // internet): it would serve the newest build at /api/mobile/latest.
  Future<void> _checkUpdate() async {
    setState(() => update = 'checking');
    final j = await App.I.dash.getJson('/api/mobile/latest');
    if (!mounted) return;
    setState(() {
      if (j is Map && j['build'] is num) {
        update = (j['build'] as num) > appBuild ? 'version ${j['version']} is on the dashboard' : 'up to date';
      } else {
        update = 'the dashboard has no app builds to offer yet';
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Scaffold(
      appBar: AppBar(title: const Glow('SETTINGS', color: C.accent, size: 17, spacing: 2)),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) => ListView(padding: const EdgeInsets.only(bottom: 40), children: [
          const SectionHead('DASHBOARD'),
          Panel(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              KV('Link', switch (m.link) { Link.up => 'connected', Link.connecting => 'connecting', Link.refused => 'refused', Link.off => 'off' },
                  color: m.link == Link.up ? C.green : (m.link == Link.connecting ? C.amber : C.red)),
              if (m.target.isNotEmpty) KV('Talking to', m.target, monoValue: true, color: C.cyan),
              if (m.mode.isNotEmpty) KV('Mode', m.mode, color: m.mode == 'live' ? C.green : C.amber),
              if (m.lastError.isNotEmpty) KV('Last error', m.lastError, color: C.red),
              const SizedBox(height: 10),
              TextField(controller: host, decoration: const InputDecoration(labelText: 'Host (blank = find it on this network)', hintText: '192.168.1.20, mac.tailnet.ts.net')),
              const SizedBox(height: 10),
              Row(children: [
                SizedBox(width: 110, child: TextField(controller: port, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'Port'))),
                const SizedBox(width: 10),
                Expanded(
                  child: TextField(
                    controller: token,
                    obscureText: !showToken,
                    decoration: InputDecoration(
                      labelText: 'Access token',
                      hintText: 'home-dashboard/access_token',
                      suffixIcon: IconButton(icon: Icon(showToken ? Icons.visibility_off : Icons.visibility, color: C.muted), onPressed: () => setState(() => showToken = !showToken)),
                    ),
                  ),
                ),
              ]),
              const SizedBox(height: 6),
              const Text('Trusted home networks need no token; anything else (Tailscale, another Wi-Fi) does.',
                  style: TextStyle(color: C.muted, fontSize: 11.5)),
              const SizedBox(height: 12),
              Wrap(spacing: 8, runSpacing: 8, children: [
                Pill('CONNECT', color: C.green, onTap: _apply),
                Pill(scanning ? 'SEARCHING' : 'FIND ON LAN', color: C.cyan, on: scanning, onTap: scanning ? null : _scan),
              ]),
              if (found != null) ...[
                const SizedBox(height: 10),
                if (found!.isEmpty) const Text('Nothing announced _ouispy._tcp here.', style: TextStyle(color: C.amber)),
                for (final f in found!)
                  ListTile(
                    contentPadding: EdgeInsets.zero,
                    leading: const Icon(Icons.dns_outlined, color: C.cyan),
                    title: Text(f.name, style: const TextStyle(color: C.text)),
                    subtitle: Text('${f.host}:${f.port}', style: const TextStyle(color: C.soft, fontFamily: mono)),
                    trailing: const Pill('USE', color: C.green, small: true),
                    onTap: () {
                      host.text = f.host;
                      port.text = '${f.port}';
                      _apply();
                    },
                  ),
              ],
            ]),
          ),
          const SectionHead('ALERTS ON THIS PHONE', color: C.hot),
          Panel(
            child: Column(children: [
              _sw('Notifications', 'one per alert', s.notify, (v) => s.notify = v),
              _sw('Vibrate', 'buzz on an alert', s.vibrate, (v) => s.vibrate = v),
              _sw('Sound', 'notification sound', s.alertSound, (v) => s.alertSound = v),
              _sw('Silence everything', 'no banner, no buzz, no notification', m.muteAll, (v) => m.setMuteAll(v)),
              if (m.muted.isNotEmpty) ...[
                const Divider(),
                for (final mac in m.muted)
                  ListTile(
                    dense: true,
                    contentPadding: EdgeInsets.zero,
                    title: Text(m.labelOf(mac), style: const TextStyle(color: C.text)),
                    subtitle: Text(mac, style: const TextStyle(color: C.soft, fontFamily: mono)),
                    trailing: Pill('UNMUTE', color: C.green, small: true, onTap: () {
                      m.toggleMute(mac);
                      s.muted = m.muted;
                      s.save();
                    }),
                  ),
              ],
            ]),
          ),
          const SectionHead('MAP', color: C.green),
          Panel(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              KV('Layer', s.tileLayer, monoValue: true),
              KV('Saved tiles', cache == null ? '...' : '${cache!.$1} tiles, ${(cache!.$2 / 1048576).toStringAsFixed(1)} MB', color: C.green),
              _sw('Fetch new tiles', 'from the dashboard only; off = saved tiles only', s.tilesOnline, (v) => s.tilesOnline = v),
              Wrap(spacing: 8, children: [
                Pill('CLEAR SAVED TILES', color: C.red, onTap: () async {
                  if (await confirm(context, 'CLEAR SAVED TILES?', 'The map needs the dashboard again to show anything.', yes: 'CLEAR')) {
                    await TileCache.I.clear();
                    _usage();
                  }
                }),
              ]),
            ]),
          ),
          const SectionHead('APP', color: C.purple),
          Panel(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              const KV('Version', '$appVersion ($appBuild)'),
              const KV('Privacy', 'Talks only to your dashboard. No accounts, no analytics, no outside map servers.', color: C.green),
              if (update.isNotEmpty) KV('Update', update, color: C.amber),
              const SizedBox(height: 6),
              Pill('CHECK FOR UPDATE', color: C.purple, onTap: _checkUpdate),
            ]),
          ),
        ]),
      ),
    );
  }

  Widget _sw(String t, String sub, bool v, ValueChanged<bool> f) => SwitchListTile(
        contentPadding: EdgeInsets.zero,
        title: Text(t, style: const TextStyle(color: C.text, fontWeight: FontWeight.w600)),
        subtitle: Text(sub, style: const TextStyle(color: C.muted, fontSize: 12)),
        value: v,
        onChanged: (x) {
          setState(() => f(x));
          s.save();
          App.I.m.changed();
        },
      );
}
