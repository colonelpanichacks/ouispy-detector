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
    final v = await askText(context, 'NAME THIS DEVICE', App.I.m.aliasOf(widget.mac), hint: 'blank clears it');
    if (v == null) return;
    await App.I.dash.setAlias(widget.mac, v);
    _load();
  }

  Future<void> _notes() async {
    final v = await askText(context, 'NOTES', '${d?['notes'] ?? ''}', maxLines: 4);
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
        child: ListView(shrinkWrap: true, children: [
          const SectionHead('GROUP', color: C.orange),
          for (final g in groups)
            ListTile(
              title: Text('${g['name']}', style: TextStyle(color: g['name'] == cur ? C.orange : C.text, fontWeight: FontWeight.w600)),
              trailing: Text('${g['n']} devices', style: const TextStyle(color: C.muted)),
              onTap: () => Navigator.pop(c, g['id']),
            ),
          ListTile(leading: const Icon(Icons.add, color: C.green), title: const Text('NEW GROUP', style: TextStyle(color: C.green)), onTap: () => Navigator.pop(c, 'new')),
          if (cur != null)
            ListTile(leading: const Icon(Icons.remove, color: C.red), title: const Text('TAKE OUT OF GROUP', style: TextStyle(color: C.red)), onTap: () => Navigator.pop(c, 'out')),
        ]),
      ),
    );
    if (pick == null || !mounted) return;
    if (pick == 'new') {
      final n = await askText(context, 'NEW GROUP', '');
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
      appBar: AppBar(title: Glow(widget.mac, color: C.soft, size: 15, font: mono), actions: [
        IconButton(onPressed: _load, icon: const Icon(Icons.refresh, color: C.accent)),
      ]),
      body: ListenableBuilder(
        listenable: m,
        builder: (_, _) {
          final t = m.targets[widget.mac];
          final alias = m.aliasOf(widget.mac);
          final name = '${d?['name'] ?? ''}'.isNotEmpty ? '${d!['name']}' : m.nameOf(widget.mac);
          final muted = m.muted.contains(widget.mac);
          final group = (d?['group'] as Map?)?['name'] ?? m.groupOf[widget.mac];
          return RefreshIndicator(
            color: C.accent,
            onRefresh: _load,
            child: ListView(padding: const EdgeInsets.only(bottom: 30), children: [
              Panel(
                glow: true,
                color: C.cyan,
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Glow(alias.isNotEmpty ? alias : (name.isNotEmpty ? name : (t?.descr.isNotEmpty == true ? t!.descr : 'Unnamed device')), color: C.cyan, size: 21),
                  if (alias.isNotEmpty && name.isNotEmpty) Text('calls itself "$name"', style: const TextStyle(color: C.muted)),
                  const SizedBox(height: 10),
                  Wrap(spacing: 7, runSpacing: 7, children: [
                    Pill(alias.isEmpty ? 'NAME IT' : 'RENAME', color: C.accent, onTap: _rename),
                    Pill(group == null || '$group'.isEmpty ? 'GROUP' : 'GROUP: ${'$group'.toUpperCase()}', color: C.orange, onTap: _group),
                    Pill('NOTES', color: C.purple, onTap: _notes),
                    Pill(muted ? 'MUTED' : 'ALERTS ON', color: muted ? C.red : C.green, onTap: () {
                      m.toggleMute(widget.mac);
                      App.I.s.muted = m.muted;
                      App.I.s.save();
                    }),
                  ]),
                ]),
              ),
              if (t != null) ...[
                SectionHead('LIVE', color: m.now - t.last < 60 ? C.green : C.amber),
                Panel(
                  child: Column(children: [
                    KV('Last heard', '${agoText(t.last)}  (${clock(t.last)})', color: m.now - t.last < 60 ? C.green : C.amber),
                    KV('First this session', clock(t.first)),
                    if (t.seen.isNotEmpty) KV('Heard by', t.seen.map((s) => '${m.nodeName(s.node)} ${s.rssi} dBm').join('\n')),
                    if (t.triOk) KV('Position', '${t.triLat.toStringAsFixed(5)}, ${t.triLon.toStringAsFixed(5)}  +/- ${t.triErr.round()} m${t.triRough ? ' (rough)' : ''}', color: t.triRough ? C.amber : C.green),
                  ]),
                ),
                if (t.triOk)
                  Panel(
                    padding: EdgeInsets.zero,
                    child: ClipRRect(
                      borderRadius: BorderRadius.circular(14),
                      child: SizedBox(
                        height: 220,
                        child: MiniMap(center: LatLng(t.triLat, t.triLon), zoom: 17, layers: [
                          CircleLayer(circles: [
                            CircleMarker(
                                point: LatLng(t.triLat, t.triLon),
                                radius: t.triErr,
                                useRadiusInMeter: true,
                                color: (t.triRough ? C.amber : C.green).withValues(alpha: 0.18),
                                borderColor: t.triRough ? C.amber : C.green,
                                borderStrokeWidth: 2),
                          ]),
                          nodeMarkers(),
                        ]),
                      ),
                    ),
                  ),
              ],
              if ('${d?['notes'] ?? ''}'.isNotEmpty) ...[
                const SectionHead('NOTES', color: C.purple),
                Panel(child: Text('${d!['notes']}', style: const TextStyle(color: C.text, height: 1.35))),
              ],
              const SectionHead('RECORD'),
              if (loading)
                const Padding(padding: EdgeInsets.all(30), child: Center(child: CircularProgressIndicator(color: C.accent)))
              else if (d == null)
                const Empty('The dashboard has no record of it (or is unreachable).')
              else
                Panel(
                  child: Column(children: [
                    for (final r in (d!['rows'] as List? ?? const []))
                      if (r is List && r.length >= 2) KV('${r[0]}', '${r[1]}', color: _rowColor('${r[0]}')),
                  ]),
                ),
              if ((d?['companions'] as List?)?.isNotEmpty == true) ...[
                const SectionHead('TURNS UP WITH', color: C.hot),
                for (final c in d!['companions'] as List)
                  Panel(
                    onTap: () => openDevice(context, '${c['mac']}'),
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
                    child: Row(children: [
                      Expanded(
                        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                          Text('${c['label'] ?? c['mac']}', overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.text, fontWeight: FontWeight.w600)),
                          Text('${c['mac']}', style: const TextStyle(color: C.soft, fontSize: 11, fontFamily: mono)),
                        ]),
                      ),
                      Text('${c['together']}x together\n${c['occasions']} occasions', textAlign: TextAlign.right, style: const TextStyle(color: C.hot, fontSize: 11)),
                    ]),
                  ),
              ],
            ]),
          );
        },
      ),
    );
  }

  Color _rowColor(String k) => switch (k) {
        'MAC vendor' => C.amber,
        'Broadcast name' || 'SSID' || 'Looks for' => C.cyan,
        'Heard by' => C.green,
        'Detections' => C.pink,
        _ => C.text,
      };
}
