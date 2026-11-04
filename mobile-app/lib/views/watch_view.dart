// DETECTIONS: what the phone and the nodes alert on. The signature library (the
// dashboard's: firmware presets plus the repo's OUI lists), your own rules, and
// installing them on OUI-SPY nodes as their filter list.
import 'package:flutter/material.dart';

import '../data/watch.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

class WatchView extends StatelessWidget {
  const WatchView({super.key});

  @override
  Widget build(BuildContext context) {
    final w = Watch.I;
    return ListenableBuilder(
      listenable: w,
      builder: (context, _) {
        final mine = [
          for (final r in w.rules)
            if (r.group.isEmpty) r,
        ];
        return ListView(
          padding: const EdgeInsets.only(bottom: 24),
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 14, 20, 4),
              child: Text(
                w.rules.isEmpty
                    ? 'Pick what to watch for. The phone alerts every time one turns up, and the same list installs on your nodes so they beep and flash for it.'
                    : 'The phone alerts every time one of these ${w.rules.length} turns up, even if it was heard a minute ago.',
                style: T.meta.copyWith(height: 1.45),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
              child: Row(
                children: [
                  Expanded(
                    child: Btn('Add a rule', icon: Ph.plus, kind: BtnKind.ghost, color: C.text, expand: true, onTap: () => addRule(context)),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Btn('Install on nodes', icon: Ph.uploadSimple, expand: true, onTap: w.rules.any((r) => r.forNodes) ? () => install(context) : null),
                  ),
                ],
              ),
            ),
            SectionHead('Your rules', trailing: Text('${mine.length}', style: T.silk)),
            if (mine.isEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 0, 20, 4),
                child: Text('None yet. Add one here, or tap Watch on any device.', style: T.meta),
              )
            else
              Panel(
                padding: EdgeInsets.zero,
                margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
                child: Column(
                  children: [
                    for (final (i, r) in mine.indexed) ...[if (i > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), _RuleRow(r)],
                  ],
                ),
              ),
            SectionHead('Signature library', trailing: Text('${w.library.length} vendors', style: T.silk)),
            Panel(
              padding: EdgeInsets.zero,
              margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
              child: Column(
                children: [
                  for (final (i, v) in w.library.indexed) ...[if (i > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), _VendorRow(v)],
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}

class _RuleRow extends StatelessWidget {
  const _RuleRow(this.r);
  final Rule r;
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(16, 10, 6, 10),
    child: Row(
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(r.d.isNotEmpty ? r.d : r.id, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row.copyWith(fontSize: 14.5)),
              const SizedBox(height: 3),
              Row(
                children: [
                  Tag(r.t == 'VENDOR' ? 'Phone only' : r.t, color: r.forNodes ? C.silk : C.amber),
                  const SizedBox(width: 8),
                  Flexible(
                    child: Text(r.id, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.code),
                  ),
                ],
              ),
            ],
          ),
        ),
        IconButton(
          icon: const Icon(Ph.trash, size: 18, color: C.silk),
          onPressed: () => Watch.I.remove([r]),
        ),
      ],
    ),
  );
}

class _VendorRow extends StatelessWidget {
  const _VendorRow(this.v);
  final Vendor v;
  @override
  Widget build(BuildContext context) {
    final st = Watch.I.vendorState(v);
    final cat = '${v.meta['Category'] ?? ''}';
    return InkWell(
      onTap: () => st == 2 ? Watch.I.remove(v.rows) : Watch.I.add([for (final r in v.rows) Rule(r.t, r.id, r.d, group: v.key)]),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(v.label, style: T.row.copyWith(fontSize: 14.5)),
                  const SizedBox(height: 2),
                  Text([if (cat.isNotEmpty) cat, '${v.rows.length} signatures'].join('  ·  '), style: T.meta.copyWith(fontSize: 12)),
                ],
              ),
            ),
            if (st == 1) const Tag('Some', color: C.amber),
            const SizedBox(width: 10),
            Icon(st == 2 ? PhF.checkSquare : (st == 1 ? Ph.minusSquare : Ph.square), color: st == 0 ? C.silk : C.accent, size: 22),
          ],
        ),
      ),
    );
  }
}

/// Add a rule by hand (also used by a device's Watch button, prefilled).
Future<void> addRule(BuildContext context, {String t = 'OUI', String id = '', String d = ''}) async {
  var type = t;
  final idc = TextEditingController(text: id), dc = TextEditingController(text: d);
  String? err;
  await showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    builder: (c) => StatefulBuilder(
      builder: (c, set) => Padding(
        padding: EdgeInsets.fromLTRB(16, 0, 16, 16 + MediaQuery.viewInsetsOf(c).bottom),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('WATCH FOR', style: T.label),
            const SizedBox(height: 10),
            Wrap(
              spacing: 6,
              runSpacing: 6,
              children: [
                for (final k in const ['OUI', 'MAC', 'NAME', 'VENDOR', 'CID', 'SVC', 'PROBE', 'BEACON'])
                  Chip2(k == 'VENDOR' ? 'Maker' : k, on: type == k, onTap: () => set(() => type = k)),
              ],
            ),
            const SizedBox(height: 6),
            Text(typeLabel[type]! + (type == 'VENDOR' ? '  (phone only)' : ''), style: T.meta),
            const SizedBox(height: 12),
            TextField(
              controller: idc,
              style: T.row,
              autofocus: true,
              decoration: InputDecoration(labelText: hexLen.containsKey(type) ? '${hexLen[type]} hex digits' : 'Text'),
            ),
            const SizedBox(height: 10),
            TextField(
              controller: dc,
              style: T.row,
              decoration: const InputDecoration(labelText: 'Label (shown in the alert)'),
            ),
            if (err != null) ...[const SizedBox(height: 8), Text(err!, style: T.meta.copyWith(color: C.red))],
            const SizedBox(height: 14),
            Btn(
              'Watch for it',
              icon: Ph.eye,
              kind: BtnKind.filled,
              expand: true,
              onTap: () {
                final (r, e) = Rule.parse(type, idc.text, dc.text);
                if (r == null) return set(() => err = e);
                Watch.I.add([r]);
                Navigator.pop(c);
              },
            ),
          ],
        ),
      ),
    ),
  );
}

/// Install the list on nodes: every OUI-SPY node, or one.
Future<void> install(BuildContext context) async {
  final m = App.I.m;
  final rules = [
    for (final r in Watch.I.rules)
      if (r.forNodes) r,
  ];
  final home = m.nodes.values.where((n) => n.isHome).firstOrNull;
  final pick = await showModalBottomSheet<String>(
    context: context,
    builder: (c) => SafeArea(
      child: ListView(
        shrinkWrap: true,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
            child: Text('INSTALL ${rules.length} FILTERS ON', style: T.label),
          ),
          ListTile(
            leading: const Icon(Ph.graph, color: C.accent),
            title: const Text('Every node'),
            subtitle: const Text('Added to what each one already watches for'),
            onTap: () => Navigator.pop(c, '*'),
          ),
          for (final n in m.nodes.values)
            ListTile(
              leading: Icon(n.isHome ? Ph.house : Ph.cpu, color: n.online ? C.text : C.silk),
              title: Text(n.label),
              subtitle: Text('${n.id}  ·  ${n.online ? 'online' : 'offline'}', style: T.code),
              onTap: () => Navigator.pop(c, n.id),
            ),
        ],
      ),
    ),
  );
  if (pick == null || !context.mounted) return;
  if (!await confirm(
    context,
    'Install ${rules.length} filters?',
    'They are added to the node${pick == '*' ? 's' : ''} filter list and apply straight away.',
    yes: 'Install',
    color: C.accent,
  )) {
    return;
  }
  // one transfer holds 4000 bytes: send the list in parts that fit
  final parts = <List<Map<String, dynamic>>>[];
  var cur = <Map<String, dynamic>>[], size = 0;
  for (final r in rules) {
    final n = 4 + r.id.length + r.d.length;
    if (cur.isNotEmpty && size + n > 3900) {
      parts.add(cur);
      cur = [];
      size = 0;
    }
    cur.add(r.filter);
    size += n;
  }
  if (cur.isNotEmpty) parts.add(cur);
  for (final p in parts) {
    if (pick == '*') {
      await App.I.link.nodeCmd('all', 'filters_add', {'filters': p});
      if (home != null) await App.I.link.nodeCmd(home.id, 'filters_add', {'filters': p});
    } else {
      await App.I.link.nodeCmd(pick, 'filters_add', {'filters': p});
    }
  }
}
