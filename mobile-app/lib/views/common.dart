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

void openDevice(BuildContext context, String mac) =>
    Navigator.of(context).push(MaterialPageRoute(builder: (_) => DeviceDetail(mac: mac.toUpperCase())));

Color typeColor(Det d) {
  final t = d.type.toUpperCase();
  if (t == 'RID' || d.descr.startsWith('DRONE')) return C.hot;
  if (d.dom == 'ble') return C.purple;
  return C.cyan;
}

/// One detection, as the Tab5's LIVE rows: time, what, who heard it, signal.
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
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 9),
        decoration: BoxDecoration(
          border: const Border(bottom: BorderSide(color: C.line)),
          color: alert ? C.hot.withValues(alpha: 0.05) : null,
        ),
        child: Row(children: [
          Container(width: 3, height: 38, decoration: BoxDecoration(color: tc, boxShadow: [BoxShadow(color: tc.withValues(alpha: 0.7), blurRadius: 6)])),
          const SizedBox(width: 10),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(children: [
                Flexible(child: Text(label, overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.text, fontSize: 14.5, fontWeight: FontWeight.w600))),
                if (d.isNew) ...[const SizedBox(width: 6), const Pill('NEW', color: C.green, small: true)],
                if (group.isNotEmpty) ...[const SizedBox(width: 6), Pill(group.toUpperCase(), color: C.orange, small: true)],
              ]),
              const SizedBox(height: 3),
              Row(children: [
                Text(d.mac, style: const TextStyle(color: C.soft, fontSize: 11.5, fontFamily: mono)),
                const SizedBox(width: 8),
                Text((d.type.isEmpty ? d.dom : d.type).toUpperCase(), style: TextStyle(color: tc, fontSize: 11, fontWeight: FontWeight.w700)),
                const SizedBox(width: 8),
                Flexible(
                    child: Text(m.nodeName(d.node),
                        overflow: TextOverflow.ellipsis, style: TextStyle(color: C.hue(m.nodes[d.node]?.hue), fontSize: 11.5, fontWeight: FontWeight.w600))),
              ]),
            ]),
          ),
          Column(crossAxisAlignment: CrossAxisAlignment.end, children: [
            Text(clock(d.ts), style: const TextStyle(color: C.muted, fontSize: 11.5, fontFamily: mono)),
            const SizedBox(height: 4),
            Row(children: [
              Text('${d.rssi ?? '?'}', style: TextStyle(color: C.rssi(d.rssi), fontSize: 12, fontWeight: FontWeight.w700, fontFamily: mono)),
              const SizedBox(width: 5),
              RssiBar(d.rssi),
            ]),
          ]),
        ]),
      ),
    );
  }
}

/// A KPI tile, as the Tab5 Overview's.
class Kpi extends StatelessWidget {
  const Kpi(this.label, this.value, this.color, {super.key, this.sub});
  final String label, value;
  final Color color;
  final String? sub;
  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
        decoration: BoxDecoration(
          color: C.panel,
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: color.withValues(alpha: 0.5)),
          boxShadow: [BoxShadow(color: color.withValues(alpha: 0.15), blurRadius: 10)],
        ),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          Text(label, style: const TextStyle(color: C.faint, fontSize: 9.5, fontWeight: FontWeight.w800, letterSpacing: 1)),
          const SizedBox(height: 2),
          Glow(value, color: color, size: 22, weight: FontWeight.w800),
          if (sub != null) Text(sub!, overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.muted, fontSize: 10)),
        ]),
      );
}

/// Pills in a scrolling row.
class PillRow extends StatelessWidget {
  const PillRow({super.key, required this.children, this.padding = const EdgeInsets.fromLTRB(12, 8, 12, 6)});
  final List<Widget> children;
  final EdgeInsets padding;
  @override
  Widget build(BuildContext context) => SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        padding: padding,
        child: Row(children: [for (final c in children) Padding(padding: const EdgeInsets.only(right: 7), child: c)]),
      );
}

Future<String?> askText(BuildContext context, String title, String initial, {String hint = '', int maxLines = 1}) {
  final ctl = TextEditingController(text: initial);
  return showDialog<String>(
    context: context,
    builder: (c) => AlertDialog(
      title: Glow(title, color: C.accent, size: 16),
      content: TextField(controller: ctl, autofocus: true, maxLines: maxLines, decoration: InputDecoration(hintText: hint)),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c), child: const Text('CANCEL', style: TextStyle(color: C.red))),
        TextButton(onPressed: () => Navigator.pop(c, ctl.text.trim()), child: const Text('SAVE', style: TextStyle(color: C.green))),
      ],
    ),
  );
}

Future<bool> confirm(BuildContext context, String title, String body, {String yes = 'YES', Color color = C.red}) async =>
    await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: Glow(title, color: color, size: 16),
        content: Text(body, style: const TextStyle(color: C.text)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('CANCEL', style: TextStyle(color: C.soft))),
          TextButton(onPressed: () => Navigator.pop(c, true), child: Text(yes, style: TextStyle(color: color))),
        ],
      ),
    ) ??
    false;
