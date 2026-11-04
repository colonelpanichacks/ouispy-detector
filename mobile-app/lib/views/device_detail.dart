// A device's full record (/api/devices/<mac>?flat=1, the Tab5's detail page): what it
// broadcast, who heard it, companions, its group; name it, note it, group it, mute it.
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';

import '../main.dart';
import '../theme.dart';
import 'common.dart';
import '../data/watch.dart';
import 'map_view.dart';
import 'watch_view.dart';

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
    final r = await App.I.link.device(widget.mac);
    if (!mounted) return;
    setState(() {
      d = r;
      loading = false;
    });
  }

  Future<void> _rename() async {
    final v = await askText(context, 'Name this device', App.I.m.aliasOf(widget.mac), hint: 'blank clears it');
    if (v == null) return;
    await App.I.link.setAlias(widget.mac, v);
    _load();
  }

  Future<void> _notes() async {
    final v = await askText(context, 'Notes', '${d?['notes'] ?? ''}', maxLines: 4);
    if (v == null) return;
    await App.I.link.setNotes(widget.mac, v);
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
      await App.I.link.groupNew(n, widget.mac);
    } else if (pick == 'out') {
      final id = (d?['group'] as Map?)?['id'] ?? groups.where((g) => g['name'] == cur).firstOrNull?['id'];
      if (id is int) await App.I.link.groupRemove(id, widget.mac);
    } else if (pick is int) {
      await App.I.link.groupAdd(pick, widget.mac);
    }
    _load();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Scaffold(
      appBar: AppBar(
        titleSpacing: 0,
        title: Text('DEVICE', style: T.label.copyWith(fontSize: 12, color: C.soft)),
        actions: [
          IconButton(
            onPressed: _load,
            icon: const Icon(Ph.arrowsClockwise, color: C.silk, size: 20),
          ),
        ],
      ),
      body: ListenableBuilder(
        listenable: m,
        builder: (_, _) {
          final t = m.targets[widget.mac];
          final last = m.dets.where((x) => x.mac == widget.mac).firstOrNull;
          final alias = m.aliasOf(widget.mac);
          final name = '${d?['name'] ?? ''}'.isNotEmpty ? '${d!['name']}' : m.nameOf(widget.mac);
          final descr = (t?.descr.isNotEmpty == true ? t!.descr : last?.descr ?? '').replaceFirst(RegExp(r'^DRONE '), '');
          final vendor = _row('MAC vendor');
          final muted = m.muted.contains(widget.mac);
          final group = (d?['group'] as Map?)?['name'] ?? m.groupOf[widget.mac];
          final dom = t?.dom ?? last?.dom ?? '';
          final type = t?.type ?? last?.type ?? '';
          final drone = type == 'RID';
          final live = t != null && m.now - t.last < 60;
          final heard = t?.last ?? last?.ts ?? 0;
          final rssi = t?.best ?? last?.rssi;
          final title = [alias, name, descr, vendor].firstWhere((x) => x.isNotEmpty && x != 'not registered' && x != 'random address', orElse: () => 'Unknown device');
          final sub = [if (alias.isNotEmpty && name.isNotEmpty) 'calls itself "$name"', if (title != descr && descr.isNotEmpty) descr].join('  ·  ');
          final c = drone ? C.hot : C.dom(dom);
          return RefreshIndicator(
            color: C.accent,
            onRefresh: _load,
            child: ListView(
              padding: const EdgeInsets.only(bottom: 30),
              children: [
                // the part's label: what it is, its address, what kind of radio
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title, style: T.title.copyWith(fontSize: 24)),
                      if (sub.isNotEmpty) ...[const SizedBox(height: 3), Text(sub, style: T.meta.copyWith(fontSize: 13))],
                      const SizedBox(height: 10),
                      Row(
                        children: [
                          Icon(domIcon(dom, drone: drone), size: 15, color: c),
                          const SizedBox(width: 7),
                          SelectableText(widget.mac, style: T.code.copyWith(fontSize: 13.5, color: C.soft)),
                        ],
                      ),
                      const SizedBox(height: 10),
                      Wrap(
                        spacing: 6,
                        runSpacing: 6,
                        children: [
                          Tag(drone ? 'Remote ID' : (dom == 'ble' ? 'Bluetooth' : (dom == 'wifi' ? 'Wi-Fi' : '?')), color: c),
                          if (type.isNotEmpty && !drone) Tag(type),
                          if (vendor == 'random address') const Tag('Random MAC', color: C.amber),
                          if (group != null && '$group'.isNotEmpty) Tag('$group', color: C.orange),
                          if (muted) const Tag('Muted', color: C.red),
                        ],
                      ),
                    ],
                  ),
                ),
                // the reading right now
                Panel(
                  margin: const EdgeInsets.fromLTRB(16, 16, 16, 4),
                  padding: EdgeInsets.zero,
                  edge: live ? C.green : null,
                  child: IntrinsicHeight(
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        Expanded(
                          child: Padding(
                            padding: const EdgeInsets.fromLTRB(14, 12, 10, 12),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text('SIGNAL', style: T.silk),
                                const SizedBox(height: 6),
                                Row(
                                  crossAxisAlignment: CrossAxisAlignment.end,
                                  children: [
                                    Text(rssi == null ? '-' : '$rssi', style: T.num.copyWith(color: C.rssi(rssi))),
                                    const SizedBox(width: 4),
                                    Padding(
                                      padding: const EdgeInsets.only(bottom: 4),
                                      child: Text('dBm', style: T.meta),
                                    ),
                                    const Spacer(),
                                    Padding(padding: const EdgeInsets.only(bottom: 6), child: RssiBar(rssi)),
                                  ],
                                ),
                              ],
                            ),
                          ),
                        ),
                        Container(width: 1, color: C.silkLine),
                        Expanded(
                          child: Padding(
                            padding: const EdgeInsets.fromLTRB(14, 12, 10, 12),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text('LAST HEARD', style: T.silk),
                                const SizedBox(height: 6),
                                Row(
                                  children: [
                                    if (live) ...[const Dot(C.green, size: 7, halo: true), const SizedBox(width: 7)],
                                    Flexible(
                                      child: Text(heard == 0 ? '-' : (live ? 'now' : ago(heard)), style: T.num.copyWith(color: live ? C.green : C.text)),
                                    ),
                                  ],
                                ),
                                const SizedBox(height: 2),
                                Text(heard == 0 ? '' : clock(heard, date: true), style: T.code.copyWith(fontSize: 11)),
                              ],
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
                // what you can do with it: one printed row, one colour
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                  child: Row(
                    children: [
                      Expanded(
                        child: Btn(alias.isEmpty ? 'Name' : 'Rename', icon: Ph.pencilSimple, onTap: _rename, kind: BtnKind.ghost, color: C.text, expand: true),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn('Group', icon: Ph.folderSimple, onTap: _group, kind: BtnKind.ghost, color: C.text, expand: true),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Btn('Notes', icon: Ph.notePencil, onTap: _notes, kind: BtnKind.ghost, color: C.text, expand: true),
                      ),
                      const SizedBox(width: 8),
                      // watch it: the phone (and the nodes, once installed) alert whenever it shows up
                      ListenableBuilder(
                        listenable: Watch.I,
                        builder: (_, _) {
                          final r = Rule('MAC', widget.mac.replaceAll(':', ''), title);
                          final on = Watch.I.has(r);
                          return Btn(
                            '',
                            icon: on ? PhF.eye : Ph.eye,
                            kind: BtnKind.ghost,
                            color: on ? C.accent : C.text,
                            onTap: () async {
                              if (on) return Watch.I.remove([r]);
                              await addRule(context, t: 'MAC', id: widget.mac, d: title);
                            },
                          );
                        },
                      ),
                      const SizedBox(width: 8),
                      Btn(
                        '',
                        icon: muted ? Ph.bellSlash : Ph.bell,
                        kind: BtnKind.ghost,
                        color: muted ? C.red : C.text,
                        onTap: () {
                          m.toggleMute(widget.mac);
                          App.I.s.muted = m.muted;
                          App.I.s.save();
                        },
                      ),
                    ],
                  ),
                ),
                if (t != null && t.seen.isNotEmpty)
                  Group(
                    title: 'Heard by',
                    trailing: Text('${t.seen.length} ${t.seen.length == 1 ? 'node' : 'nodes'}', style: T.silk),
                    children: [
                      for (final (i, s) in t.seen.indexed)
                        Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                          child: Row(
                            children: [
                              Dot(C.hue(m.nodes[s.node]?.hue, i), size: 8),
                              const SizedBox(width: 10),
                              Expanded(child: Text(m.nodeName(s.node), style: T.row.copyWith(fontSize: 14.5))),
                              if (i == 0 && t.seen.length > 1) ...[const Tag('Closest', color: C.accent), const SizedBox(width: 10)],
                              Text(
                                '${s.rssi}',
                                style: T.code.copyWith(color: C.rssi(s.rssi), fontSize: 13, fontWeight: FontWeight.w600),
                              ),
                              const SizedBox(width: 6),
                              RssiBar(s.rssi),
                            ],
                          ),
                        ),
                    ],
                  ),
                if (t != null && t.triOk) ...[
                  SectionHead(
                    'Position',
                    trailing: Text('±${t.triErr.round()} m${t.triRough ? ' rough' : ''}'.toUpperCase(), style: T.silk.copyWith(color: t.triRough ? C.amber : C.green)),
                  ),
                  Panel(
                    padding: EdgeInsets.zero,
                    margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
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
                  const Empty('No record', body: 'Nothing stored for this device yet.', icon: Ph.cloudSlash)
                else
                  Group(
                    title: 'Over the air',
                    children: [
                      for (final r in (d!['rows'] as List? ?? const []))
                        if (r is List && r.length >= 2 && '${r[0]}' != 'Heard by') KV('${r[0]}', '${r[1]}', color: _rowColor('${r[0]}')),
                    ],
                  ),
                if ((d?['companions'] as List?)?.isNotEmpty == true)
                  Group(
                    title: 'Turns up with',
                    children: [
                      for (final c in d!['companions'] as List)
                        InkWell(
                          onTap: () => openDevice(context, '${c['mac']}'),
                          child: Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                            child: Row(
                              children: [
                                Expanded(
                                  child: Column(
                                    crossAxisAlignment: CrossAxisAlignment.start,
                                    children: [
                                      Text('${c['label'] ?? c['mac']}', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row.copyWith(fontSize: 14.5)),
                                      const SizedBox(height: 2),
                                      Text('${c['mac']}', style: T.code),
                                    ],
                                  ),
                                ),
                                Text(
                                  '${c['together']} / ${c['occasions']}',
                                  style: T.code.copyWith(color: C.soft, fontWeight: FontWeight.w600),
                                ),
                                const SizedBox(width: 6),
                                const Icon(Ph.caretRight, size: 14, color: C.silkMark),
                              ],
                            ),
                          ),
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

  /// A value from the record's rows ('' if it has none).
  String _row(String k) {
    for (final r in (d?['rows'] as List? ?? const [])) {
      if (r is List && r.length >= 2 && '${r[0]}' == k) return '${r[1]}';
    }
    return '';
  }

  Color _rowColor(String k) => switch (k) {
    'MAC vendor' => C.amber,
    'Broadcast name' || 'SSID' || 'Looks for' => C.accent,
    _ => C.text,
  };
}
