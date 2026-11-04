import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:intl/intl.dart';

import '../data/lookup.dart';
import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'device_detail.dart';

String ago(double ts) {
  if (ts <= 0) return '-';
  final s = (App.I.m.now - ts).round();
  if (s < 5) return 'now';
  if (s < 60) return '${s}s';
  if (s < 3600) return '${s ~/ 60}m';
  if (s < 86400) return '${s ~/ 3600}h ${(s % 3600) ~/ 60}m';
  return '${s ~/ 86400}d';
}

/// "12s ago", or "just now".
String agoText(double ts) {
  final a = ago(ts);
  return a == 'now' ? 'just now' : '$a ago';
}

String clock(double ts, {bool date = false}) {
  if (ts <= 0) return '-';
  final t = DateTime.fromMillisecondsSinceEpoch((ts * 1000).round());
  return DateFormat(date ? 'MMM d HH:mm' : 'HH:mm:ss').format(t);
}

String dur(num s) {
  final v = s.round();
  if (v < 60) return '${v}s';
  if (v < 3600) return '${v ~/ 60}m ${v % 60}s';
  if (v < 86400) return '${v ~/ 3600}h ${(v % 3600) ~/ 60}m';
  return '${v ~/ 86400}d ${(v % 86400) ~/ 3600}h';
}

void openDevice(BuildContext context, String mac) => Navigator.of(context).push(MaterialPageRoute(builder: (_) => DeviceDetail(mac: mac.toUpperCase())));

final _macRe = RegExp(r'^([0-9A-Fa-f]{2}[:-]?){5}[0-9A-Fa-f]{2}$');

/// A device's title in a list: its name or what it says it is, without the "DRONE " the
/// icon already shows; never just its MAC again (that is on the line under it): the maker,
/// or "Random address" / "Unknown device".
String deviceTitle(String mac, [String descr = '']) {
  final m = App.I.m;
  final l = m.labelOf(mac, descr.replaceFirst(RegExp(r'^DRONE\s+'), '').trim()).trim();
  if (l.isNotEmpty && !_macRe.hasMatch(l) && l.toUpperCase() != mac.toUpperCase()) return l;
  return Lookup.I.vendor(mac) ?? (Lookup.randomMac(mac) ? 'Random address' : 'Unknown device');
}

bool isDrone(Det d) => d.type.toUpperCase() == 'RID' || d.descr.startsWith('DRONE');

Color typeColor(Det d) => isDrone(d) ? C.hot : C.dom(d.dom);

IconData domIcon(String dom, {bool drone = false}) => drone ? Ph.drone : (dom == 'ble' ? Ph.bluetooth : Ph.wifiHigh);

/// One detection: what it is, its MAC and who heard it, when and how loud.
class DetRow extends StatelessWidget {
  const DetRow(this.d, {super.key, this.alert = false});
  final Det d;
  final bool alert;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final tc = typeColor(d);
    final label = deviceTitle(d.mac, d.descr);
    final group = m.groupOf[d.mac] ?? d.group;
    return InkWell(
      onTap: () => openDevice(context, d.mac),
      child: Container(
        color: alert ? C.hot.withValues(alpha: 0.06) : null,
        padding: const EdgeInsets.fromLTRB(16, 11, 16, 11),
        child: Row(
          children: [
            IconTile(domIcon(d.dom, drone: isDrone(d)), tc),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Flexible(
                        child: Text(label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row),
                      ),
                      if (d.isNew) ...[const SizedBox(width: 6), const Tag('New', color: C.green)],
                      if (group.isNotEmpty) ...[const SizedBox(width: 6), Tag(group, color: C.orange)],
                    ],
                  ),
                  const SizedBox(height: 3),
                  Row(
                    children: [
                      Text(d.mac, style: T.code),
                      const SizedBox(width: 8),
                      Dot(C.hue(m.nodes[d.node]?.hue), size: 6),
                      const SizedBox(width: 5),
                      Flexible(
                        child: Text(m.nodeName(d.node), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta),
                      ),
                    ],
                  ),
                ],
              ),
            ),
            const SizedBox(width: 8),
            Column(
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                Text(clock(d.ts), style: T.code.copyWith(fontSize: 11.5)),
                const SizedBox(height: 5),
                Row(
                  children: [
                    Text(
                      '${d.rssi ?? '-'}',
                      style: T.code.copyWith(color: C.rssi(d.rssi), fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(width: 4),
                    RssiBar(d.rssi),
                  ],
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

/// Chips in a scrolling row.
class ChipRow extends StatelessWidget {
  const ChipRow({super.key, required this.children, this.padding = const EdgeInsets.fromLTRB(16, 8, 16, 8)});
  final List<Widget> children;
  final EdgeInsets padding;
  @override
  Widget build(BuildContext context) => SingleChildScrollView(
    scrollDirection: Axis.horizontal,
    padding: padding,
    child: Row(
      children: [
        for (final (i, c) in children.indexed)
          Padding(
            padding: EdgeInsets.only(left: i == 0 ? 0 : 8),
            child: c,
          ),
      ],
    ),
  );
}

Future<String?> askText(BuildContext context, String title, String initial, {String hint = '', int maxLines = 1}) {
  final ctl = TextEditingController(text: initial);
  return showDialog<String>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: ctl,
        autofocus: true,
        maxLines: maxLines,
        style: T.row,
        decoration: InputDecoration(hintText: hint),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(c),
          child: const Text('Cancel', style: TextStyle(color: C.muted)),
        ),
        TextButton(
          onPressed: () => Navigator.pop(c, ctl.text.trim()),
          child: const Text(
            'Save',
            style: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
          ),
        ),
      ],
    ),
  );
}

Future<bool> confirm(BuildContext context, String title, String body, {String yes = 'Yes', Color color = C.red}) async =>
    await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: Text(title),
        content: Text(body, style: T.body),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(c, false),
            child: const Text('Cancel', style: TextStyle(color: C.muted)),
          ),
          TextButton(
            onPressed: () => Navigator.pop(c, true),
            child: Text(
              yes,
              style: TextStyle(color: color, fontWeight: FontWeight.w600),
            ),
          ),
        ],
      ),
    ) ??
    false;

// ------------------------------------------------------------ node / kind filters

/// The node picker's kinds, as the dashboard's (`node=kind:detector|skyspy|wardrive`).
const nodeKinds = [('detector', 'Detectors'), ('skyspy', 'Sky-Spy'), ('wardrive', 'Wardrivers')];

/// The dashboard's kind key for a node: detector, skyspy or wardrive.
String nodeKindKey(FleetNode? n) {
  final k = n?.kind ?? 'detector';
  if (k.startsWith('wardriver')) return 'wardrive';
  if (k == 'sky-spy') return 'skyspy';
  return 'detector';
}

/// A node filter: '' every node, `kind:detector` (etc.) every node of a kind, else one node id.
bool nodePasses(String id, String filter) {
  if (filter.isEmpty) return true;
  if (filter.startsWith('kind:')) return nodeKindKey(App.I.m.nodes[id]) == filter.substring(5);
  return id == filter;
}

/// Any of [ids] passes the node filter.
bool nodesPass(Iterable<String> ids, String filter) => filter.isEmpty || ids.any((i) => nodePasses(i, filter));

String nodeFilterLabel(String f) {
  if (f.isEmpty) return 'All nodes';
  if (f.startsWith('kind:')) return nodeKinds.where((k) => k.$1 == f.substring(5)).firstOrNull?.$2 ?? f.substring(5);
  return App.I.m.nodeName(f);
}

/// The node picker's choices: every node, each kind, each node (by name).
List<(String, String)> nodeFilterItems() {
  final m = App.I.m;
  final ns = m.nodes.values.toList()..sort((a, b) => a.label.toLowerCase().compareTo(b.label.toLowerCase()));
  return [('', 'All nodes'), for (final (k, l) in nodeKinds) ('kind:$k', l), for (final n in ns) (n.id, n.label)];
}

/// A compact drop-down in a printed box: "SORT  LAST", lit when it is not the default.
class PickChip<V> extends StatelessWidget {
  const PickChip({super.key, required this.label, required this.value, required this.items, required this.onChanged, this.icon, this.lit, this.dots});
  final String label;
  final V value;
  final List<(V, String)> items;
  final ValueChanged<V> onChanged;
  final IconData? icon;

  /// Lit (accent) when true; by default when [value] is not the first item's.
  final bool? lit;

  /// A colour dot per item (a node's hue).
  final Color? Function(V v)? dots;

  @override
  Widget build(BuildContext context) {
    final on = lit ?? (items.isNotEmpty && value != items.first.$1);
    final cur = items.where((i) => i.$1 == value).firstOrNull?.$2 ?? '';
    return PopupMenuButton<V>(
      tooltip: label,
      color: C.panel,
      position: PopupMenuPosition.under,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(4),
        side: const BorderSide(color: C.silkLine),
      ),
      onSelected: onChanged,
      itemBuilder: (_) => [
        for (final (v, l) in items)
          PopupMenuItem<V>(
            value: v,
            height: 40,
            child: Row(
              children: [
                if (dots?.call(v) case final c?) ...[Dot(c, size: 7), const SizedBox(width: 9)],
                Expanded(
                  child: Text(
                    l,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: T.row.copyWith(fontSize: 14, color: v == value ? C.accent : C.text),
                  ),
                ),
                if (v == value) const Icon(Ph.check, size: 15, color: C.accent),
              ],
            ),
          ),
      ],
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
        decoration: BoxDecoration(
          color: on ? C.accent.withValues(alpha: 0.1) : C.bg.withValues(alpha: 0.72),
          borderRadius: BorderRadius.circular(3),
          border: Border.all(color: on ? C.accent.withValues(alpha: 0.75) : C.silkLine),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (icon != null) ...[Icon(icon, size: 13, color: on ? C.accent : C.silk), const SizedBox(width: 5)],
            Text(
              label.toUpperCase(),
              style: const TextStyle(fontFamily: mono, color: C.silk, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1),
            ),
            const SizedBox(width: 6),
            ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 120),
              child: Text(
                cur.toUpperCase(),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(fontFamily: mono, color: on ? C.accent : C.soft, fontSize: 11, fontWeight: FontWeight.w600, letterSpacing: 0.8),
              ),
            ),
            const SizedBox(width: 3),
            Icon(Ph.caretDown, size: 11, color: on ? C.accent : C.silk),
          ],
        ),
      ),
    );
  }
}

/// The node picker as a [PickChip]: all, a kind, or one node, dotted in its colour.
class NodePick extends StatelessWidget {
  const NodePick({super.key, required this.value, required this.onChanged});
  final String value;
  final ValueChanged<String> onChanged;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final items = nodeFilterItems();
    return PickChip<String>(
      label: 'Node',
      icon: Ph.graph,
      value: items.any((i) => i.$1 == value) ? value : '',
      items: items,
      dots: (v) => v.isEmpty || v.startsWith('kind:') ? null : C.hue(m.nodes[v]?.hue),
      onChanged: onChanged,
    );
  }
}

/// The groups the devices are in, as picker items: any, none, then each name.
List<(String, String)> groupFilterItems() {
  final names = App.I.m.groupOf.values.where((g) => g.isNotEmpty).toSet().toList()..sort();
  return [('', 'Any'), ('none', 'No group'), for (final g in names) (g, g)];
}

bool groupPasses(String mac, String filter) {
  if (filter.isEmpty) return true;
  final g = App.I.m.groupOf[mac.toUpperCase()] ?? '';
  return filter == 'none' ? g.isEmpty : g == filter;
}

/// Writes a path where the user can find it: a dialog with the path and a copy button.
Future<void> showPath(BuildContext context, String title, String path) => showDialog<void>(
  context: context,
  builder: (c) => AlertDialog(
    title: Text(title),
    content: SelectableText(path, style: T.code.copyWith(color: C.soft)),
    actions: [
      TextButton(
        onPressed: () {
          Clipboard.setData(ClipboardData(text: path));
          Navigator.pop(c);
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('path copied')));
        },
        child: const Text(
          'Copy path',
          style: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
        ),
      ),
      TextButton(
        onPressed: () => Navigator.pop(c),
        child: const Text('Close', style: TextStyle(color: C.muted)),
      ),
    ],
  ),
);
