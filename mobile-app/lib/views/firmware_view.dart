// FIRMWARE: reflash any node over the mesh (the dashboard's firmware box and fleet update,
// the Tab5's FIRMWARE chips). An image is a library build on the dashboard (by kind) or a
// .bin on the phone (a path, a URL, or a file in the app's firmware folder). It is staged on
// the home once, then each selected remote gets it in turn and the home installs it last.
// Every install is tap to arm, tap again within 4 s, as on the Tab5.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/dashboard.dart';
import '../data/firmware.dart';
import '../data/local_mesh.dart';
import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

class FirmwareView extends StatefulWidget {
  /// [node]: this node only (its page's Firmware); null: the fleet update.
  const FirmwareView({super.key, this.node});
  final String? node;
  @override
  State<FirmwareView> createState() => _FirmwareViewState();
}

class _FirmwareViewState extends State<FirmwareView> {
  bool fromFile = false;
  Map<String, Map<String, dynamic>> lib = {};
  bool libRead = false;
  String? kind; // the library build picked
  FwImage? img;
  String loading = '', loadErr = '';
  List<String> files = [];
  final sel = <String>{};
  String? armed; // what the next tap installs
  Timer? _arm;

  bool get usb => App.I.link is LocalMesh;

  @override
  void initState() {
    super.initState();
    fromFile = usb;
    if (widget.node != null) sel.add(widget.node!);
    _readLib();
    _readFiles();
  }

  @override
  void dispose() {
    _arm?.cancel();
    super.dispose();
  }

  Future<void> _readLib() async {
    final l = App.I.link;
    if (l is! Dashboard) {
      setState(() => libRead = true);
      return;
    }
    final r = await fwLibrary(l);
    if (!mounted) return;
    setState(() {
      lib = r;
      libRead = true;
      final n = widget.node == null ? null : App.I.m.nodes[widget.node!];
      if (n != null && r.containsKey(nodeKindOf(n.fw))) kind ??= nodeKindOf(n.fw);
    });
  }

  Future<void> _readFiles() async {
    final f = await fwFiles();
    if (mounted) setState(() => files = [for (final x in f) x.path]);
  }

  Future<void> _load(String where) async {
    setState(() {
      loading = where;
      loadErr = '';
    });
    try {
      final i = await fwLoad(where);
      if (mounted) setState(() => img = i);
    } catch (e) {
      if (mounted) setState(() => loadErr = '$e');
    } finally {
      if (mounted) setState(() => loading = '');
    }
  }

  /// Tap to arm [what], tap again within 4 s to go.
  bool _tap(String what) {
    if (armed == what) {
      _arm?.cancel();
      setState(() => armed = null);
      return true;
    }
    _arm?.cancel();
    setState(() => armed = what);
    _arm = Timer(const Duration(seconds: 4), () {
      if (mounted) setState(() => armed = null);
    });
    return false;
  }

  String? get _kindNow => fromFile ? img?.kind : kind;

  /// Why the picked image can't go on [n] (wrong board), or null.
  String? _misfit(FleetNode n) {
    if (fromFile) return img == null ? null : imageError(img!, n);
    return kind == null ? null : boardError(kind!, n.fw, n.id);
  }

  void _go() {
    final ids = sel.toList();
    if (ids.isEmpty) return;
    if (!_tap('go')) return;
    FwRun.I.run(ids, kind: fromFile ? null : kind, img: fromFile ? img : null);
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final run = FwRun.I;
    return ReconPage(
      title: widget.node == null ? 'FIRMWARE' : 'FIRMWARE · ${m.nodeName(widget.node!)}',
      body: ListenableBuilder(
        listenable: Listenable.merge([m, run]),
        builder: (context, _) {
          final nodes = m.nodes.values.toList()..sort((a, b) => a.isHome != b.isHome ? (a.isHome ? 1 : -1) : a.label.compareTo(b.label));
          final shown = widget.node == null ? nodes : nodes.where((n) => n.id == widget.node).toList();
          final ready = fromFile ? img != null : kind != null;
          final bad = [
            for (final n in shown)
              if (sel.contains(n.id) && _misfit(n) != null) _misfit(n)!,
          ];
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              if (run.running || run.steps.isNotEmpty) _runPanel(run),
              SectionHead('Image', trailing: Text(usb ? 'via the home on USB' : 'via the dashboard', style: T.meta)),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                child: Seg<bool>(
                  items: const [(false, 'Library'), (true, 'A .bin file')],
                  value: fromFile,
                  onChanged: (v) => setState(() {
                    fromFile = v;
                    armed = null;
                  }),
                ),
              ),
              if (!fromFile) ..._library(shown) else ..._file(),
              SectionHead(widget.node == null ? 'Nodes' : 'Node', trailing: widget.node == null ? Text('${sel.length} picked', style: T.meta) : null),
              if (shown.isEmpty) Panel(child: Text('No nodes in the fleet yet.', style: T.meta)),
              if (shown.isNotEmpty) Group(children: [for (final n in shown) _nodeRow(n, run)]),
              if (bad.isNotEmpty)
                Panel(
                  edge: C.red,
                  child: Text(bad.join('\n'), style: T.body.copyWith(color: C.red)),
                ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
                child: run.running
                    ? Btn('Stop the update', icon: Ph.stop, color: C.red, expand: true, onTap: run.abort)
                    : Btn(
                        armed == 'go' ? 'Tap again to install on ${sel.length}' : _goLabel(shown),
                        icon: Ph.lightning,
                        color: armed == 'go' ? C.red : C.accent,
                        kind: armed == 'go' ? BtnKind.filled : BtnKind.tonal,
                        expand: true,
                        onTap: ready && sel.isNotEmpty && bad.isEmpty && sel.every((id) => m.nodes[id]?.online == true) ? _go : null,
                      ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 10, 20, 0),
                child: Text(
                  'It is staged on the home once, then sent over the mesh to one node at a time; the home installs it last. '
                  'Each node restarts into the new build when it has it. Stop finishes the node in flight${usb ? ' and aborts the transfer' : ''}.',
                  style: T.meta,
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  String _goLabel(List<FleetNode> shown) {
    final k = _kindNow;
    if (k == null) return fromFile ? 'Load an image first' : 'Pick a build first';
    if (widget.node != null && shown.isNotEmpty) {
      final n = shown.first;
      return nodeKindOf(n.fw) == k ? 'Update with ${fwNames[k] ?? k}' : 'Turn into ${fwNames[k] ?? k}';
    }
    return 'Update ${sel.length} selected';
  }

  // ---- the library on the dashboard

  List<Widget> _library(List<FleetNode> shown) {
    if (usb) {
      return [
        Panel(
          child: Text(
            "The library lives on the dashboard. On a node's USB, pick a .bin on the phone instead "
            '(or connect to the dashboard to install a library build).',
            style: T.meta,
          ),
        ),
      ];
    }
    if (!libRead) return [const Panel(child: LinearProgressIndicator())];
    if (lib.isEmpty) return [Panel(child: Text('No builds in the dashboard\'s checkout (pio run -e <env> there first).', style: T.meta))];
    // one node: only its board's builds, like the Tab5's chips
    final one = widget.node == null || shown.isEmpty ? null : shown.first;
    final kinds = fwKinds.where((k) => one == null || one.fw.isEmpty || isAtomKind(k) == isAtomKind(nodeKindOf(one.fw))).toList();
    return [
      Group(
        children: [
          for (final k in kinds)
            Tile(
              icon: kind == k ? Ph.checkCircle : Ph.circle,
              color: lib[k] == null ? C.faint : (kind == k ? C.accent : C.muted),
              title: fwNames[k] ?? k,
              titleColor: lib[k] == null ? C.faint : null,
              subtitle: Text(
                lib[k] == null ? 'not built' : '${boardOf(k)}  ·  build ${lib[k]!['build'] ?? '?'}  ·  ${(((lib[k]!['size'] as num?) ?? 0) / 1024).round()} KB',
                style: T.meta,
              ),
              trailing: one != null && nodeKindOf(one.fw) == k ? const Tag('Runs now', color: C.green) : const SizedBox.shrink(),
              onTap: lib[k] == null
                  ? null
                  : () => setState(() {
                      kind = k;
                      armed = null;
                    }),
            ),
        ],
      ),
    ];
  }

  // ---- a .bin on the phone

  List<Widget> _file() {
    final i = img;
    return [
      Group(
        children: [
          Tile(
            icon: Ph.folderOpen,
            color: C.accent,
            title: 'Path or URL',
            subtitle: Text('A file on this phone, or http(s)://.../firmware.bin', style: T.meta),
            onTap: () async {
              final v = await askText(
                context,
                'Firmware image',
                loading.isNotEmpty ? loading : (i != null ? '' : '/sdcard/Download/firmware.bin'),
                hint: '/path/firmware.bin or https://...',
              );
              if (v != null && v.isNotEmpty) _load(v);
            },
          ),
          for (final f in files.take(8))
            Tile(
              icon: Ph.fileArrowUp,
              color: C.violet,
              title: f.split('/').last,
              subtitle: Text(f, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.code.copyWith(fontSize: 11)),
              onTap: () => _load(f),
            ),
          FutureBuilder(
            future: fwFolders(),
            builder: (context, s) => Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
              child: Text(
                'Files in ${s.data == null ? 'the app\'s firmware folder' : s.data!.map((d) => d.path).join(', ')} are listed here.',
                style: T.meta.copyWith(fontSize: 11.5),
              ),
            ),
          ),
        ],
      ),
      if (loading.isNotEmpty) const Panel(child: LinearProgressIndicator()),
      if (loadErr.isNotEmpty)
        Panel(
          edge: C.red,
          child: Text(loadErr, style: T.body.copyWith(color: C.red)),
        ),
      if (i != null)
        Group(
          title: 'Loaded',
          edge: C.accent,
          children: [
            KV('File', i.name, monoValue: true),
            KV('Firmware', '${fwNames[i.kind] ?? i.kind}  (${boardOf(i.kind)})'),
            KV('Build', i.build.isNotEmpty ? i.build : '${i.date} ${i.time}', monoValue: true),
            KV('Chip', i.chip == chipEsp32s3 ? 'ESP32-S3 (id ${i.chip})' : 'id ${i.chip}', color: i.chip == chipEsp32s3 ? C.green : C.red),
            KV('Size', '${(i.size / 1024).round()} KB  ·  ${(i.size + fwChunk - 1) ~/ fwChunk} lines'),
            KV('SHA-256', i.sha256, monoValue: true),
          ],
        ),
    ];
  }

  // ---- nodes

  Widget _nodeRow(FleetNode n, FwRun run) {
    final s = run.steps[n.id];
    final ota = n.ota;
    final live = ota.isNotEmpty && const ['offered', 'sending'].contains(ota['state']);
    final build = fromFile ? (img?.build ?? '') : '${lib[kind]?['build'] ?? ''}';
    final current = fwIsCurrent(n.fw, build);
    final misfit = _misfit(n);
    final pct = s != null && !s.over ? s.pct : (live ? (ota['pct'] as num?)?.toInt() : null);
    final status = s != null
        ? '${s.state}${s.detail.isNotEmpty && s.detail != s.state ? ': ${s.detail}' : ''}${s.kbps != null && s.kbps! > 0 ? '  ·  ${s.kbps} kbps' : ''}'
        : (ota.isNotEmpty ? 'last update ${ota['state'] ?? '-'} ${ota['pct'] ?? ''}%${'${ota['err'] ?? ''}'.isNotEmpty ? ' (${ota['err']})' : ''}' : '');
    final sc = switch (s?.state) {
      'done' => C.green,
      'failed' => C.red,
      'skipped' => C.muted,
      null => ota['state'] == 'failed' ? C.red : C.muted,
      _ => C.accent,
    };
    final can = n.online && !run.running && misfit == null;
    return Padding(
      padding: const EdgeInsets.fromLTRB(6, 6, 14, 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              if (widget.node == null)
                Checkbox(value: sel.contains(n.id), onChanged: can || sel.contains(n.id) ? (v) => setState(() => v == true ? sel.add(n.id) : sel.remove(n.id)) : null)
              else
                const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Dot(n.online ? C.green : C.red, size: 7, halo: n.online),
                        const SizedBox(width: 6),
                        Flexible(
                          child: Text(n.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                        ),
                        if (n.isHome) ...[const SizedBox(width: 8), const Tag('Home', color: C.accent)],
                      ],
                    ),
                    const SizedBox(height: 3),
                    Row(
                      children: [
                        Flexible(
                          child: Text('${n.id}  ${n.fw.isEmpty ? 'build unknown' : n.fw}', maxLines: 1, overflow: TextOverflow.ellipsis, style: T.code),
                        ),
                        const SizedBox(width: 8),
                        if (build.isNotEmpty && n.fw.isNotEmpty) Tag(current ? 'Current' : 'Older', color: current ? C.green : C.amber),
                        if (misfit != null) ...[const SizedBox(width: 6), const Tag('Other board', color: C.red)],
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
          if (pct != null) ...[
            const SizedBox(height: 8),
            Padding(
              padding: EdgeInsets.only(left: widget.node == null ? 12 : 10),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(2),
                child: LinearProgressIndicator(value: pct.clamp(0, 100) / 100, minHeight: 4),
              ),
            ),
          ],
          if (status.isNotEmpty)
            Padding(
              padding: EdgeInsets.only(left: widget.node == null ? 12 : 10, top: 5),
              child: Text('${pct != null ? '$pct%  ' : ''}$status', style: T.meta.copyWith(color: sc)),
            ),
        ],
      ),
    );
  }

  Widget _runPanel(FwRun run) {
    final c = run.running ? C.accent : (run.error.isNotEmpty || run.steps.values.any((s) => s.state == 'failed') ? C.red : C.green);
    return Panel(
      edge: c,
      margin: const EdgeInsets.fromLTRB(16, 8, 16, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Dot(c, halo: run.running),
              const SizedBox(width: 8),
              Expanded(child: Text(run.running ? 'Updating: ${run.what}' : 'Last run: ${run.what}', style: T.row.copyWith(fontSize: 14))),
            ],
          ),
          const SizedBox(height: 8),
          Text('STAGED ON HOME  ${run.stagePct}%', style: T.silk),
          const SizedBox(height: 4),
          LinearProgressIndicator(value: run.stagePct / 100, minHeight: 4),
          if (run.error.isNotEmpty) ...[const SizedBox(height: 8), Text(run.error, style: T.meta.copyWith(color: C.red))],
        ],
      ),
    );
  }
}
