// LIVE: the Tab5's overview KPIs on top, the detection feed under them.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/model.dart';
import '../data/tri.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'settings_view.dart';

class FeedView extends StatefulWidget {
  const FeedView({super.key});
  @override
  State<FeedView> createState() => _FeedViewState();
}

class _FeedViewState extends State<FeedView> {
  String dom = ''; // '', ble, wifi, rid
  String node = ''; // '', kind:<detector|skyspy|wardrive>, or a node id

  @override
  void initState() {
    super.initState();
    // the feed is always built: it starts the device trails the device page draws
    TriTrails.I.attach(App.I.m);
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final list = m.dets.where((d) => (dom == 'rid' ? isDrone(d) : (dom.isEmpty || d.dom == dom)) && nodePasses(d.node, node)).toList();
        final kinds = {for (final n in m.nodes.values) nodeKindKey(n)};
        final nodes = m.nodes.values.toList()..sort((a, b) => a.label.toLowerCase().compareTo(b.label.toLowerCase()));
        if (node.isNotEmpty && !node.startsWith('kind:') && !m.nodes.containsKey(node)) node = '';
        return RefreshIndicator(
          color: C.accent,
          backgroundColor: C.panel,
          onRefresh: App.I.link.fetchState,
          child: CustomScrollView(
            slivers: [
              SliverToBoxAdapter(child: _Overview(m)),
              SliverToBoxAdapter(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(16, 14, 16, 6),
                  child: Seg<String>(
                    items: const [('', 'All'), ('ble', 'Bluetooth'), ('wifi', 'Wi-Fi'), ('rid', 'Drones')],
                    value: dom,
                    onChanged: (v) => setState(() => dom = v),
                  ),
                ),
              ),
              if (m.nodes.length > 1 || kinds.length > 1)
                SliverToBoxAdapter(
                  child: ChipRow(
                    padding: const EdgeInsets.fromLTRB(16, 2, 16, 6),
                    children: [
                      for (final (k, l) in nodeKinds)
                        if (kinds.contains(k)) Chip2(l, on: node == 'kind:$k', onTap: () => setState(() => node = node == 'kind:$k' ? '' : 'kind:$k')),
                      if (kinds.isNotEmpty) Container(width: 1, height: 22, color: C.silkLine),
                      for (final (i, n) in nodes.indexed)
                        Chip2(n.label, on: node == n.id, color: C.hue(n.hue, i), onTap: () => setState(() => node = node == n.id ? '' : n.id)),
                    ],
                  ),
                ),
              if (list.isEmpty)
                SliverFillRemaining(hasScrollBody: false, child: linkEmpty(context, 'Nothing heard yet', 'Detections show up here the moment a node hears something.'))
              else
                SliverList.separated(itemCount: list.length, separatorBuilder: (_, _) => const Divider(indent: 64), itemBuilder: (_, i) => DetRow(list[i])),
              const SliverToBoxAdapter(child: SizedBox(height: 16)),
            ],
          ),
        );
      },
    );
  }
}

/// The empty state, or why there is no data: the link to the dashboard is down.
Widget linkEmpty(BuildContext context, String title, String body, {IconData icon = Ph.waveform}) {
  final m = App.I.m;
  if (m.link == Link.up) return Empty(title, body: body, icon: icon);
  final searching = m.lastError.startsWith('looking') || m.link == Link.connecting;
  return Empty(
    searching ? 'Looking for the dashboard' : "Can't reach the dashboard",
    body: searching
        ? 'Searching this network. Or set its address by hand.'
        : (m.target.isNotEmpty ? '${m.target.replaceFirst(RegExp(r'^https?://'), '')} is not answering.' : 'No address is set and none was found on this network.'),
    icon: searching ? Ph.magnifyingGlass : Ph.plugs,
    color: searching ? C.amber : C.red,
    action: Btn(
      'Connection settings',
      icon: Ph.gearSix,
      onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const SettingsView())),
    ),
  );
}

/// The overview as one printed strip: six readouts in a row, the last 24 hours under them.
class _Overview extends StatelessWidget {
  const _Overview(this.m);
  final Model m;
  @override
  Widget build(BuildContext context) {
    final live = m.targets.values.where((t) => m.now - t.last < targetLiveS).length;
    final allUp = m.nodesOnline == m.nodes.length && m.nodes.isNotEmpty;
    final hours = [for (final h in m.hourly) (h['c'] as num?)?.toInt() ?? 0];
    return ReadoutStrip(
      [
        ('Nodes', '${m.nodesOnline}/${m.nodes.length}', m.nodes.isEmpty ? C.text : (allUp ? C.green : C.amber)),
        ('Now', '$live', C.text),
        ('Drones', '${m.liveDrones}', m.liveDrones > 0 ? C.hot : C.text),
        ('Today', '${m.stats['day'] ?? 0}', C.text),
        (m.muteAll ? 'Silent' : 'Alerts', '${m.alertsUnacked}', m.alertsUnacked > 0 ? C.hot : C.text),
        ('MACs', '${m.stats['macs'] ?? 0}', C.text),
      ],
      footer: Padding(
        padding: const EdgeInsets.fromLTRB(12, 6, 12, 6),
        child: Row(
          children: [
            Text('24H', style: T.silk.copyWith(fontSize: 9)),
            const SizedBox(width: 8),
            Expanded(
              child: SizedBox(
                height: 18,
                child: CustomPaint(painter: _Bars(hours), size: Size.infinite),
              ),
            ),
            const SizedBox(width: 8),
            Text('${hours.fold(0, (a, b) => a + b)}', style: T.code.copyWith(fontSize: 11, color: C.soft)),
          ],
        ),
      ),
    );
  }
}

class _Bars extends CustomPainter {
  _Bars(this.v);
  final List<int> v;
  @override
  void paint(Canvas c, Size s) {
    final n = v.isEmpty ? 24 : v.length;
    final mx = v.isEmpty ? 1 : math.max(1, v.reduce(math.max));
    final w = s.width / n;
    for (var i = 0; i < n; i++) {
      final x = i < v.length ? v[i] : 0;
      final h = x == 0 ? 2.0 : math.max(4.0, s.height * x / mx);
      final r = RRect.fromRectAndRadius(Rect.fromLTWH(i * w + 1.5, s.height - h, w - 3, h), const Radius.circular(2));
      c.drawRRect(r, Paint()..color = x == 0 ? C.line2 : Color.lerp(C.accent.withValues(alpha: 0.45), C.accent, x / mx)!);
    }
  }

  @override
  bool shouldRepaint(_Bars o) => true;
}
