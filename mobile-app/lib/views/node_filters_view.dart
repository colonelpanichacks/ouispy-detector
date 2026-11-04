// A node's installed filter list: read off the node (USB: its RFL line into MeshStore;
// the dashboard: the fleet record's `filters`, GET /api/nodes/<id>), checked against the
// CRC the node reports (cfg.fcrc), and edited in place: add a row, remove one, replace the
// list with the phone's watch list or with the home's.
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../data/local_mesh.dart';
import '../data/model.dart';
import '../data/store.dart';
import '../data/watch.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

final _crcTable = List<int>.generate(256, (n) {
  var c = n;
  for (var k = 0; k < 8; k++) {
    c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
  }
  return c;
});

/// The standard CRC-32 (zlib.crc32, what the firmware reports as fcrc).
int crc32(Uint8List b) {
  var c = 0xFFFFFFFF;
  for (final x in b) {
    c = _crcTable[(c ^ x) & 0xFF] ^ (c >> 8);
  }
  return (c ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

/// A filter list's CRC as a node reports it: 8 upper-case hex digits.
String filtersCrc(List<Map<String, dynamic>> fs) => crc32(encodeFilters(fs)).toRadixString(16).toUpperCase().padLeft(8, '0');

const _addTypes = ['OUI', 'MAC', 'NAME', 'CID', 'SVC', 'PROBE', 'BEACON'];

class NodeFiltersView extends StatefulWidget {
  const NodeFiltersView({super.key, required this.id});
  final String id;
  @override
  State<NodeFiltersView> createState() => _NodeFiltersViewState();
}

class _NodeFiltersViewState extends State<NodeFiltersView> {
  List<Map<String, dynamic>>? list; // null = not read yet
  double? readAt;
  bool busy = false;
  Timer? _poll;
  int _pollLeft = 0;
  String _addT = 'OUI';
  final _addId = TextEditingController(), _addD = TextEditingController();
  String? _addErr;

  bool get _usb => App.I.link is LocalMesh;

  @override
  void initState() {
    super.initState();
    _load().then((_) {
      if (list == null) _read();
    });
  }

  @override
  void dispose() {
    _poll?.cancel();
    _addId.dispose();
    _addD.dispose();
    super.dispose();
  }

  static List<Map<String, dynamic>> _rows(Object? o) => [
    for (final f in (o is List ? o : const []))
      if (f is Map) {'t': '${f['t'] ?? ''}'.toUpperCase(), 'id': '${f['id'] ?? ''}', 'd': '${f['d'] ?? ''}'},
  ];

  /// A node's list as the backend holds it now (null when never read).
  static Future<(List<Map<String, dynamic>>?, double?)> listOf(String id) async {
    if (App.I.link is LocalMesh) {
      final l = MeshStore.I.filters[id];
      return (l == null ? null : _rows(l), null);
    }
    final j = await App.I.dash.getJson('/api/nodes/$id');
    final n = j is Map ? j['node'] : null;
    if (n is! Map || n['filters'] is! List) return (null, null);
    return (_rows(n['filters']), (n['filters_ts'] as num?)?.toDouble());
  }

  Future<void> _load() async {
    final (l, ts) = await listOf(widget.id);
    if (!mounted) return;
    final changed = l != null && (list == null || ts != readAt || filtersCrc(l) != filtersCrc(list!) || l.length != list!.length);
    if (changed) {
      _pollLeft = 0;
      _poll?.cancel();
    }
    setState(() {
      if (l != null) list = l;
      readAt = ts ?? (changed ? DateTime.now().millisecondsSinceEpoch / 1000 : readAt);
    });
  }

  /// Asks the node for its list, then watches for the answer (it comes over the mesh).
  Future<void> _read() async {
    setState(() => busy = true);
    await App.I.link.nodeCmd(widget.id, 'filters_get');
    if (!mounted) return;
    setState(() => busy = false);
    _pollLeft = 15;
    _poll?.cancel();
    _poll = Timer.periodic(const Duration(seconds: 2), (_) {
      if (--_pollLeft <= 0) _poll?.cancel();
      _load();
    });
    _load();
  }

  Future<void> _send(String op, List<Map<String, dynamic>> fs) async {
    setState(() => busy = true);
    final r = await App.I.link.nodeCmd(widget.id, op, {'filters': fs});
    if (!mounted) return;
    setState(() => busy = false);
    if (r != null && r['ok'] != false) _read();
  }

  Future<void> _remove(FleetNode? n, Map<String, dynamic> f) async {
    if (!await confirm(
      context,
      'Remove this filter?',
      '${typeLabel[f['t']] ?? f['t']} ${f['id']}${'${f['d']}'.isNotEmpty ? ' (${f['d']})' : ''} comes off ${n?.label ?? widget.id}.',
      yes: 'Remove',
    )) {
      return;
    }
    await _send('filters_del', [
      {'t': f['t'], 'id': f['id'], 'd': ''},
    ]);
  }

  Future<void> _add() async {
    final (r, err) = Rule.parse(_addT, _addId.text, _addD.text);
    setState(() => _addErr = err);
    if (r == null) return;
    await _send('filters_add', [r.filter]);
    if (!mounted) return;
    _addId.clear();
    _addD.clear();
  }

  Future<void> _fromWatch(FleetNode? n) async {
    final fs = [for (final r in Watch.I.rules.where((r) => r.forNodes)) r.filter];
    if (fs.isEmpty) {
      App.I.m.toast('the watch list has nothing a node can hold');
      return;
    }
    if (!await confirm(
      context,
      'Replace with the watch list?',
      "${n?.label ?? widget.id}'s ${list?.length ?? 'current'} filters are replaced with the ${fs.length} rules on the watch list.",
      yes: 'Replace',
      color: C.violet,
    )) {
      return;
    }
    await _send('filters_set', fs);
  }

  Future<void> _fromHome(FleetNode home, FleetNode? n) async {
    var (fs, _) = await listOf(home.id);
    if (!mounted) return;
    if (fs == null) {
      App.I.m.toast("reading the home's list first; try again when it arrives");
      App.I.link.nodeCmd(home.id, 'filters_get');
      return;
    }
    if (fs.isEmpty) {
      App.I.m.toast('the home has no filters');
      return;
    }
    if (!await confirm(context, "Copy the home's list?", "${n?.label ?? widget.id}'s filters are replaced with the home's ${fs.length}.", yes: 'Copy', color: C.violet)) {
      return;
    }
    await _send('filters_set', fs);
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final n = m.nodes[widget.id];
        FleetNode? home;
        for (final x in m.nodes.values) {
          if (x.isHome) home = x;
        }
        // USB: the RFL lands in the store without a notification; pick it up on any model tick.
        if (_usb) {
          final l = MeshStore.I.filters[widget.id];
          if (l != null && (list == null || l.length != list!.length || filtersCrc(_rows(l)) != filtersCrc(list!))) {
            WidgetsBinding.instance.addPostFrameCallback((_) => _load());
          }
        }
        final fs = list;
        final ours = fs == null ? null : filtersCrc(fs);
        final theirs = n?.cfg['fcrc'] is num ? (n!.cfg['fcrc'] as num).toInt().toRadixString(16).toUpperCase().padLeft(8, '0') : '${n?.cfg['fcrc'] ?? ''}'.toUpperCase();
        final same = ours != null && theirs.isNotEmpty ? ours == theirs : null;
        return ReconPage(
          title: 'FILTERS · ${n?.label ?? widget.id}',
          actions: [
            IconButton(
              tooltip: 'Read from the node',
              onPressed: busy ? null : _read,
              icon: busy || _pollLeft > 0
                  ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 1.6, color: C.accent))
                  : const Icon(Ph.arrowsClockwise, color: C.silk),
            ),
          ],

          body: ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              Group(
                title: 'On the node',
                edge: same == false ? C.amber : null,
                children: [
                  KV('Filters', fs == null ? (n?.filters != null ? '${n!.filters} (list not read yet)' : 'Not read yet') : '${fs.length}'),
                  if (ours != null) KV('List CRC', ours, monoValue: true),
                  KV('Node reports', theirs.isEmpty ? 'Not reported yet' : theirs, monoValue: theirs.isNotEmpty),
                  if (same != null) KV('Match', same ? 'Same list' : 'Differs: read it again', color: same ? C.green : C.amber),
                  if (readAt != null) KV('Read', agoText(readAt!)),
                ],
              ),
              SectionHead('Installed', trailing: fs == null ? null : Text('${fs.length}', style: T.meta)),
              if (fs == null)
                Panel(child: Text(busy || _pollLeft > 0 ? 'Asking the node for its list...' : 'Tap refresh to read the list off the node.', style: T.meta))
              else if (fs.isEmpty)
                Panel(
                  child: Text('No filters: the node has nothing to scan for.', style: T.meta.copyWith(color: C.amber)),
                )
              else
                Group(
                  children: [
                    for (final f in fs)
                      Padding(
                        padding: const EdgeInsets.fromLTRB(16, 8, 4, 8),
                        child: Row(
                          children: [
                            SizedBox(width: 64, child: Tag('${f['t']}', color: C.violet)),
                            const SizedBox(width: 8),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text('${f['id']}', style: T.code.copyWith(color: C.text, fontSize: 13)),
                                  if ('${f['d']}'.isNotEmpty) ...[
                                    const SizedBox(height: 2),
                                    Text('${f['d']}', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta),
                                  ],
                                ],
                              ),
                            ),
                            IconButton(
                              tooltip: 'Remove',
                              icon: const Icon(Ph.trash, size: 18, color: C.muted),
                              onPressed: busy ? null : () => _remove(n, f),
                            ),
                          ],
                        ),
                      ),
                  ],
                ),
              SectionHead('Add a filter'),
              Panel(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Wrap(
                      spacing: 6,
                      runSpacing: 6,
                      children: [for (final t in _addTypes) Chip2(t, on: t == _addT, color: C.violet, onTap: () => setState(() => _addT = t))],
                    ),
                    const SizedBox(height: 6),
                    Text(typeLabel[_addT] ?? _addT, style: T.meta),
                    const SizedBox(height: 10),
                    TextField(
                      controller: _addId,
                      style: T.code.copyWith(color: C.text, fontSize: 13),
                      autocorrect: false,
                      decoration: InputDecoration(hintText: hexLen.containsKey(_addT) ? '${hexLen[_addT]} hex digits' : 'Text, 1-16 characters'),
                    ),
                    const SizedBox(height: 8),
                    TextField(
                      controller: _addD,
                      style: T.row,
                      decoration: const InputDecoration(hintText: 'Description (optional)'),
                    ),
                    if (_addErr != null) ...[const SizedBox(height: 6), Text(_addErr!, style: T.meta.copyWith(color: C.red))],
                    const SizedBox(height: 10),
                    Btn('Add to the node', icon: Ph.plus, expand: true, onTap: busy ? null : _add),
                  ],
                ),
              ),
              Group(
                title: 'Replace the list',
                children: [
                  Tile(
                    icon: Ph.eye,
                    color: C.violet,
                    title: 'Replace with the watch list',
                    subtitle: Text('${Watch.I.rules.where((r) => r.forNodes).length} rules a node can hold', style: T.meta),
                    onTap: busy ? null : () => _fromWatch(n),
                  ),
                  if (home != null && home.id != widget.id)
                    Tile(
                      icon: Ph.house,
                      color: C.violet,
                      title: "Copy the home's list",
                      subtitle: Text(home.filters != null ? '${home.filters} filters on ${home.label}' : home.label, style: T.meta),
                      onTap: busy ? null : () => _fromHome(home!, n),
                    ),
                ],
              ),
            ],
          ),
        );
      },
    );
  }
}
