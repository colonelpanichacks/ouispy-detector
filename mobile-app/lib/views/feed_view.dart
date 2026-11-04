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

class _Overview extends StatelessWidget {
  const _Overview(this.m);
  final Model m;
  @override
  Widget build(BuildContext context) {
    final live = m.targets.values.where((t) => m.now - t.last < targetLiveS).length;
    final allUp = m.nodesOnline == m.nodes.length && m.nodes.isNotEmpty;
    Widget cell(Widget w, {bool right = true, bool bottom = true}) => Container(
      decoration: BoxDecoration(
        border: Border(
          right: right ? const BorderSide(color: C.silkLine) : BorderSide.none,
          bottom: bottom ? const BorderSide(color: C.silkLine) : BorderSide.none,
        ),
      ),
      child: w,
    );
    final hours = [for (final h in m.hourly) (h['c'] as num?)?.toInt() ?? 0];
    return Column(
      children: [
        Panel(
          padding: EdgeInsets.zero,
          margin: const EdgeInsets.fromLTRB(16, 18, 16, 0),
          child: Column(
            children: [
              IntrinsicHeight(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(
                      child: cell(Stat('Nodes', '${m.nodesOnline}/${m.nodes.length}', icon: Ph.graph, color: m.nodes.isEmpty ? C.text : (allUp ? C.green : C.amber))),
                    ),
                    Expanded(
                      child: cell(Stat('Devices now', '$live', icon: Ph.broadcast, sub: '${m.targets.length} in 10 min')),
                    ),
                    Expanded(
                      child: cell(Stat('Drones', '${m.liveDrones}', icon: Ph.drone, color: m.liveDrones > 0 ? C.hot : C.text), right: false),
                    ),
                  ],
                ),
              ),
              IntrinsicHeight(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(
                      child: cell(Stat('Today', '${m.stats['day'] ?? 0}', icon: Ph.clock, sub: '${m.stats['total'] ?? 0} total'), bottom: false),
                    ),
                    Expanded(
                      child: cell(
                        Stat(
                          m.muteAll ? 'Silenced' : 'Alerts',
                          '${m.alertsUnacked}',
                          icon: m.muteAll ? Ph.speakerSlash : Ph.bell,
                          color: m.alertsUnacked > 0 ? C.hot : C.text,
                        ),
                        bottom: false,
                      ),
                    ),
                    Expanded(child: cell(Stat('Known MACs', '${m.stats['macs'] ?? 0}', icon: Ph.fingerprint), right: false, bottom: false)),
                  ],
                ),
              ),
            ],
          ),
        ),
        Panel(
          margin: const EdgeInsets.fromLTRB(16, 10, 16, 0),
          padding: const EdgeInsets.fromLTRB(14, 12, 14, 10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Text('DETECTIONS · LAST 24 H', style: T.silk),
                  const Spacer(),
                  Text(
                    '${hours.fold(0, (a, b) => a + b)}',
                    style: T.code.copyWith(color: C.text, fontWeight: FontWeight.w600),
                  ),
                ],
              ),
              const SizedBox(height: 10),
              SizedBox(
                height: 46,
                child: CustomPaint(painter: _Bars(hours), size: Size.infinite),
              ),
              const SizedBox(height: 6),
              Row(
                children: [
                  Text('-24H', style: T.silk),
                  const Spacer(),
                  Text('NOW', style: T.silk),
                ],
              ),
            ],
          ),
        ),
      ],
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
