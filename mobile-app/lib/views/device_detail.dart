// A device's full record (/api/devices/<mac>?flat=1, the Tab5's detail page): what it
// broadcast, who heard it, companions, its group; name it, note it, group it, mute it.
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'map_view.dart';

class DeviceDetail extends StatefulWidget {
  const DeviceDetail({super.key, required this.mac});
  final String mac;
  @override
  State<DeviceDetail> createState() => _DeviceDetailState();
}

class _DeviceDetailState extends State<DeviceDetail> {
  Map<String, dynamic>? d;
  bool loading = true;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final r = await App.I.dash.device(widget.mac);
    if (!mounted) return;
    setState(() {
      d = r;
      loading = false;
    });
  }

  Future<void> _rename() async {
    final v = await askText(context, 'Name this device', App.I.m.aliasOf(widget.mac), hint: 'blank clears it');
    if (v == null) return;
    await App.I.dash.setAlias(widget.mac, v);
    _load();
  }

  Future<void> _notes() async {
    final v = await askText(context, 'Notes', '${d?['notes'] ?? ''}', maxLines: 4);
    if (v == null) return;
    await App.I.dash.setNotes(widget.mac, v);
    _load();
  }

  Future<void> _group() async {
    final groups = [for (final g in (d?['groups'] as List? ?? const [])) (g as Map).cast<String, dynamic>()];
    final cur = (d?['group'] as Map?)?['name'];
    final pick = await showModalBottomSheet<Object>(
      context: context,
      builder: (c) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            const SectionHead('Group'),
            for (final g in groups)
              ListTile(
                leading: Icon(g['name'] == cur ? PhF.checkCircle : Ph.folderSimple, color: g['name'] == cur ? C.accent : C.muted),
                title: Text('${g['name']}'),
                trailing: Text('${g['n']} devices', style: T.meta),
                onTap: () => Navigator.pop(c, g['id']),
              ),
            ListTile(
              leading: const Icon(Ph.plus, color: C.accent),
              title: const Text('New group', style: TextStyle(color: C.accent)),
              onTap: () => Navigator.pop(c, 'new'),
            ),
            if (cur != null)
              ListTile(
                leading: const Icon(Ph.minus, color: C.red),
                title: const Text('Take out of group', style: TextStyle(color: C.red)),
                onTap: () => Navigator.pop(c, 'out'),
              ),
          ],
        ),
      ),
    );
    if (pick == null || !mounted) return;
    if (pick == 'new') {
      final n = await askText(context, 'New group', '');
      if (n == null || n.isEmpty) return;
      await App.I.dash.groupNew(n, widget.mac);
    } else if (pick == 'out') {
      final id = (d?['group'] as Map?)?['id'] ?? groups.where((g) => g['name'] == cur).firstOrNull?['id'];
      if (id is int) await App.I.dash.groupRemove(id, widget.mac);
    } else if (pick is int) {
      await App.I.dash.groupAdd(pick, widget.mac);
    }
    _load();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Device'),
        actions: [
          IconButton(
            onPressed: _load,
            icon: const Icon(Ph.arrowsClockwise, color: C.soft),
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: m,
        builder: (_, _) {
          final t = m.targets[widget.mac];
          final alias = m.aliasOf(widget.mac);
          final name = '${d?['name'] ?? ''}'.isNotEmpty ? '${d!['name']}' : m.nameOf(widget.mac);
          final muted = m.muted.contains(widget.mac);
          final group = (d?['group'] as Map?)?['name'] ?? m.groupOf[widget.mac];
          final dom = t?.dom ?? '';
          final live = t != null && m.now - t.last < 60;
          return RefreshIndicator(
            color: C.accent,
            onRefresh: _load,
            child: ListView(
              padding: const EdgeInsets.only(bottom: 30),
              children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 4, 20, 4),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      IconTile(domIcon(dom), C.dom(dom), size: 48),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(alias.isNotEmpty ? alias : (name.isNotEmpty ? name : (t?.descr.isNotEmpty == true ? t!.descr : 'Unnamed device')), style: T.title),
                            const SizedBox(height: 4),
                            SelectableText(widget.mac, style: T.code.copyWith(fontSize: 13)),
                            if (alias.isNotEmpty && name.isNotEmpty) ...[const SizedBox(height: 3), Text('Calls itself "$name"', style: T.meta)],
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 14, 16, 6),
                  child: Row(
                    children: [
                      Expanded(
                        child: Btn(alias.isEmpty ? 'Name' : 'Rename', icon: Ph.pencilSimple, onTap: _rename, expand: true),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn(group == null || '$group'.isEmpty ? 'Group' : '$group', icon: Ph.folderSimple, onTap: _group, color: C.orange, expand: true),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn('Notes', icon: Ph.notePencil, onTap: _notes, color: C.violet, expand: true),
                      ),
                      const SizedBox(width: 8),
                      Btn(
                        '',
                        icon: muted ? Ph.bellSlash : Ph.bell,
                        color: muted ? C.red : C.green,
                        onTap: () {
                          m.toggleMute(widget.mac);
                          App.I.s.muted = m.muted;
                          App.I.s.save();
                        },
                      ),
                    ],
                  ),
                ),
                if (t != null) ...[
                  Group(
                    title: 'Right now',
                    trailing: Row(
                      children: [
                        Dot(live ? C.green : C.amber, size: 7, halo: live),
                        const SizedBox(width: 6),
                        Text(live ? 'Live' : 'Recent', style: T.meta.copyWith(color: live ? C.green : C.amber)),
                      ],
                    ),
                    children: [
                      KV('Last heard', '${agoText(t.last)}  ·  ${clock(t.last)}', color: live ? C.green : C.amber),
                      KV('First seen', clock(t.first)),
                      if (t.triOk)
                        KV(
                          'Position',
                          '${t.triLat.toStringAsFixed(5)}, ${t.triLon.toStringAsFixed(5)}\n±${t.triErr.round()} m${t.triRough ? ', rough' : ''}',
                          color: t.triRough ? C.amber : C.green,
                          monoValue: true,
                        ),
                    ],
                  ),
                  if (t.seen.isNotEmpty)
                    Group(
                      title: 'Heard by',
                      children: [
                        for (final (i, s) in t.seen.indexed)
                          Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                            child: Row(
                              children: [
                                Dot(C.hue(m.nodes[s.node]?.hue, i), size: 9),
                                const SizedBox(width: 10),
                                Expanded(child: Text(m.nodeName(s.node), style: T.row)),
                                Text('${s.rssi} dBm', style: T.code.copyWith(color: C.rssi(s.rssi), fontSize: 12.5)),
                                const SizedBox(width: 6),
                                RssiBar(s.rssi),
                              ],
                            ),
                          ),
                      ],
                    ),
                  if (t.triOk)
                    Panel(
                      padding: EdgeInsets.zero,
                      margin: const EdgeInsets.fromLTRB(16, 4, 16, 8),
                      child: SizedBox(
                        height: 220,
                        child: MiniMap(
                          center: LatLng(t.triLat, t.triLon),
                          zoom: 17,
                          layers: [
                            CircleLayer(
                              circles: [
                                CircleMarker(
                                  point: LatLng(t.triLat, t.triLon),
                                  radius: t.triErr,
                                  useRadiusInMeter: true,
                                  color: (t.triRough ? C.amber : C.green).withValues(alpha: 0.16),
                                  borderColor: t.triRough ? C.amber : C.green,
                                  borderStrokeWidth: 1.5,
                                ),
                              ],
                            ),
                            nodeMarkers(),
                          ],
                        ),
                      ),
                    ),
                ],
                if ('${d?['notes'] ?? ''}'.isNotEmpty)
                  Group(
                    title: 'Notes',
                    children: [
                      Padding(
                        padding: const EdgeInsets.all(16),
                        child: Text('${d!['notes']}', style: T.body),
                      ),
                    ],
                  ),
                if (loading)
                  const Padding(
                    padding: EdgeInsets.all(30),
                    child: Center(child: CircularProgressIndicator()),
                  )
                else if (d == null)
                  const Empty('No record', body: 'The dashboard has nothing stored for this device, or it could not be reached.', icon: Ph.cloudSlash)
                else
                  Group(
                    title: 'Record',
                    children: [
                      for (final r in (d!['rows'] as List? ?? const []))
                        if (r is List && r.length >= 2) KV('${r[0]}', '${r[1]}', color: _rowColor('${r[0]}')),
                    ],
                  ),
                if ((d?['companions'] as List?)?.isNotEmpty == true)
                  Group(
                    title: 'Turns up with',
                    children: [
                      for (final c in d!['companions'] as List)
                        Tile(
                          icon: Ph.linkSimple,
                          color: C.hot,
                          title: '${c['label'] ?? c['mac']}',
                          subtitle: Text('${c['mac']}', style: T.code),
                          trailing: Column(
                            crossAxisAlignment: CrossAxisAlignment.end,
                            children: [
                              Text(
                                '${c['together']}x',
                                style: T.code.copyWith(color: C.hot, fontWeight: FontWeight.w600),
                              ),
                              Text('${c['occasions']} times', style: T.meta.copyWith(fontSize: 11)),
                            ],
                          ),
                          onTap: () => openDevice(context, '${c['mac']}'),
                        ),
                    ],
                  ),
              ],
            ),
          );
        },
      ),
    );
  }

  Color _rowColor(String k) => switch (k) {
    'MAC vendor' => C.amber,
    'Broadcast name' || 'SSID' || 'Looks for' => C.accent,
    _ => C.text,
  };
}
