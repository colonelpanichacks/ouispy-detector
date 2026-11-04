// SETTINGS: where the dashboard is (found on the LAN or typed in), the access token, how
// alerts reach you, the offline map cache and app updates.
import 'package:flutter/material.dart';

import '../data/dashboard.dart';
import '../data/local_mesh.dart';
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
    await App.I.link.start();
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
    final (String link, Color lc) = switch (m.link) {
      Link.up => ('Connected', C.green),
      Link.connecting => ('Connecting', C.amber),
      Link.refused => ('Refused', C.red),
      Link.off => ('Not connected', C.red),
    };
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) => ListView(
          padding: const EdgeInsets.only(bottom: 40),
          children: [
            const SectionHead('Source'),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 6),
              child: Seg<String>(
                items: const [('auto', 'Auto'), ('usb', 'Node on USB'), ('dashboard', 'Dashboard')],
                value: s.source,
                onChanged: (v) async {
                  setState(() => s.source = v);
                  await s.save();
                  await App.I.pick();
                },
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 2, 20, 4),
              child: Text(
                App.I.link is LocalMesh
                    ? 'This phone is the dashboard: it reads the node on USB itself. Names, groups, notes and marks are kept on the phone.'
                    : (s.source == 'dashboard'
                          ? 'Data comes from the home dashboard.'
                          : 'Plug a node into the phone (USB OTG) and the app becomes the dashboard. With none plugged in it uses the home dashboard.'),
                style: T.meta.copyWith(height: 1.4),
              ),
            ),
            SectionHead(
              'Dashboard',
              trailing: Row(
                children: [
                  Dot(lc, size: 7, halo: m.link == Link.up),
                  const SizedBox(width: 6),
                  Text(link, style: T.meta.copyWith(color: lc)),
                ],
              ),
            ),
            Panel(
              margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  if (m.target.isNotEmpty) ...[
                    Row(
                      children: [
                        const Icon(Ph.hardDrives, size: 16, color: C.muted),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(m.target.replaceFirst(RegExp(r'^https?://'), ''), style: T.code.copyWith(color: C.text, fontSize: 13)),
                        ),
                        if (m.mode.isNotEmpty && m.mode != 'live') Tag(m.mode, color: C.amber),
                      ],
                    ),
                    if (m.lastError.isNotEmpty && m.link != Link.up) ...[const SizedBox(height: 6), Text(m.lastError, style: T.meta.copyWith(color: C.red))],
                    const SizedBox(height: 14),
                  ],
                  TextField(
                    controller: host,
                    style: T.row,
                    decoration: const InputDecoration(labelText: 'Host', hintText: 'Blank finds it on this network'),
                  ),
                  const SizedBox(height: 10),
                  Row(
                    children: [
                      SizedBox(
                        width: 100,
                        child: TextField(
                          controller: port,
                          style: T.row,
                          keyboardType: TextInputType.number,
                          decoration: const InputDecoration(labelText: 'Port'),
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: TextField(
                          controller: token,
                          style: T.row,
                          obscureText: !showToken,
                          decoration: InputDecoration(
                            labelText: 'Access token',
                            suffixIcon: IconButton(
                              icon: Icon(showToken ? Ph.eyeSlash : Ph.eye, color: C.muted, size: 19),
                              onPressed: () => setState(() => showToken = !showToken),
                            ),
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'Home networks the dashboard trusts need no token. Tailscale or any other network needs the one in home-dashboard/access_token.',
                    style: T.meta.copyWith(height: 1.4),
                  ),
                  const SizedBox(height: 14),
                  Row(
                    children: [
                      Expanded(
                        child: Btn('Connect', icon: Ph.plugs, kind: BtnKind.filled, expand: true, onTap: _apply),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn(scanning ? 'Searching' : 'Find on network', icon: Ph.magnifyingGlass, expand: true, onTap: scanning ? null : _scan),
                      ),
                    ],
                  ),
                  if (found != null) ...[
                    const SizedBox(height: 10),
                    if (found!.isEmpty) Text('Nothing on this network announces a dashboard.', style: T.meta.copyWith(color: C.amber)),
                    for (final f in found!)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: const IconTile(Ph.hardDrives, C.accent),
                        title: Text(f.name),
                        subtitle: Text('${f.host}:${f.port}', style: T.code),
                        trailing: const Icon(Ph.caretRight, size: 16, color: C.faint),
                        onTap: () {
                          host.text = f.host;
                          port.text = '${f.port}';
                          _apply();
                        },
                      ),
                  ],
                ],
              ),
            ),
            Group(
              title: 'Alerts on this phone',
              children: [
                _sw(Ph.bell, 'Notifications', 'One for each alert', s.notify, (v) => s.notify = v),
                _sw(Ph.vibrate, 'Vibrate', 'Buzz on an alert', s.vibrate, (v) => s.vibrate = v),
                _sw(Ph.speakerHigh, 'Sound', 'Notification sound', s.alertSound, (v) => s.alertSound = v),
                _sw(Ph.speakerSlash, 'Silence everything', 'No banner, buzz or notification', m.muteAll, (v) => m.setMuteAll(v)),
              ],
            ),
            if (m.muted.isNotEmpty)
              Group(
                title: 'Muted devices',
                children: [
                  for (final mac in m.muted)
                    Tile(
                      icon: Ph.bellSlash,
                      title: m.labelOf(mac),
                      subtitle: Text(mac, style: T.code),
                      trailing: TextButton(
                        onPressed: () {
                          m.toggleMute(mac);
                          s.muted = m.muted;
                          s.save();
                        },
                        child: const Text(
                          'Unmute',
                          style: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
                        ),
                      ),
                    ),
                ],
              ),
            Group(
              title: 'Map',
              children: [
                KV('Layer', s.tileLayer, monoValue: true),
                KV('Saved tiles', cache == null ? '...' : '${cache!.$1} tiles, ${(cache!.$2 / 1048576).toStringAsFixed(1)} MB'),
                _sw(Ph.cloudArrowDown, 'Fetch new tiles', 'From the dashboard only. Off uses saved tiles only.', s.tilesOnline, (v) => s.tilesOnline = v),
                Tile(
                  icon: Ph.trash,
                  color: C.red,
                  title: 'Clear saved tiles',
                  titleColor: C.red,
                  onTap: () async {
                    if (await confirm(context, 'Clear saved tiles?', 'The map will need the dashboard again to show anything.', yes: 'Clear')) {
                      await TileCache.I.clear();
                      _usage();
                    }
                  },
                ),
              ],
            ),
            Group(
              title: 'App',
              children: [
                const KV('Version', '$appVersion ($appBuild)', monoValue: true),
                Tile(
                  icon: Ph.arrowsClockwise,
                  color: C.accent,
                  title: 'Check for update',
                  subtitle: update.isEmpty ? null : Text(update, style: T.meta.copyWith(color: C.amber)),
                  onTap: _checkUpdate,
                ),
                Padding(
                  padding: const EdgeInsets.all(16),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Icon(Ph.shieldCheck, size: 18, color: C.green),
                      const SizedBox(width: 10),
                      Expanded(child: Text('Talks only to your own dashboard. No accounts, no analytics, no outside map servers.', style: T.meta.copyWith(height: 1.4))),
                    ],
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _sw(IconData i, String t, String sub, bool v, ValueChanged<bool> f) => Padding(
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
          onChanged: (x) {
            setState(() => f(x));
            s.save();
            App.I.m.changed();
          },
        ),
      ],
    ),
  );
}
