import 'package:flutter/material.dart';
import 'package:intl/intl.dart';

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
    final label = m.labelOf(d.mac, d.descr.isNotEmpty ? d.descr : d.mac);
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
