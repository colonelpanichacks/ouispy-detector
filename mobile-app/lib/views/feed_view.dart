// LIVE: the Tab5's overview KPIs on top, the detection feed under them.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

class FeedView extends StatefulWidget {
  const FeedView({super.key});
  @override
  State<FeedView> createState() => _FeedViewState();
}

class _FeedViewState extends State<FeedView> {
  String dom = ''; // '', ble, wifi, rid
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) {
        final list = m.dets.where((d) {
          if (dom == 'rid') return d.type.toUpperCase() == 'RID' || d.descr.startsWith('DRONE');
          return dom.isEmpty || d.dom == dom;
        }).toList();
        return RefreshIndicator(
          color: C.accent,
          backgroundColor: C.panel,
          onRefresh: App.I.dash.fetchState,
          child: CustomScrollView(slivers: [
            SliverToBoxAdapter(child: _Overview(m)),
            SliverToBoxAdapter(
              child: PillRow(children: [
                for (final (k, l, c) in [('', 'ALL', C.accent), ('ble', 'BLE', C.purple), ('wifi', 'WIFI', C.sky), ('rid', 'DRONE RID', C.hot)])
                  Pill(l, color: c, on: dom == k, onTap: () => setState(() => dom = k)),
              ]),
            ),
            if (list.isEmpty)
              SliverFillRemaining(
                  hasScrollBody: false,
                  child: Empty(m.link == Link.up ? 'Quiet. No detections yet.' : (m.lastError.isEmpty ? 'Connecting to the dashboard' : m.lastError)))
            else
              SliverList.builder(itemCount: list.length, itemBuilder: (_, i) => DetRow(list[i])),
          ]),
        );
      },
    );
  }
}

class _Overview extends StatelessWidget {
  const _Overview(this.m);
  final Model m;
  @override
  Widget build(BuildContext context) {
    final live = m.targets.values.where((t) => m.now - t.last < targetLiveS).length;
    final tiles = [
      Kpi('NODES ONLINE', '${m.nodesOnline}/${m.nodes.length}', m.nodesOnline == m.nodes.length && m.nodes.isNotEmpty ? C.green : C.amber),
      Kpi('DEVICES NOW', '$live', C.cyan, sub: '${m.targets.length} in 10 min'),
      Kpi('DRONES LIVE', '${m.liveDrones}', m.liveDrones > 0 ? C.hot : C.violet),
      Kpi('TODAY', '${m.stats['day'] ?? 0}', C.accent, sub: '${m.stats['total'] ?? 0} total'),
      Kpi(m.muteAll ? 'SILENCED' : 'ALERTS', '${m.alertsUnacked}', m.alertsUnacked > 0 ? C.hot : C.green),
      Kpi('KNOWN MACS', '${m.stats['macs'] ?? 0}', C.purple),
    ];
    return Column(children: [
      Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 0),
        child: GridView.count(
          crossAxisCount: 3,
          shrinkWrap: true,
          physics: const NeverScrollableScrollPhysics(),
          mainAxisSpacing: 8,
          crossAxisSpacing: 8,
          childAspectRatio: 1.55,
          children: tiles,
        ),
      ),
      Panel(
        margin: const EdgeInsets.fromLTRB(12, 8, 12, 0),
        padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('DETECTIONS, LAST 24 H', style: TextStyle(color: C.faint, fontSize: 9.5, fontWeight: FontWeight.w800, letterSpacing: 1)),
          const SizedBox(height: 6),
          SizedBox(height: 44, child: CustomPaint(painter: _Bars([for (final h in m.hourly) (h['c'] as num?)?.toInt() ?? 0]), size: Size.infinite)),
        ]),
      ),
    ]);
  }
}

class _Bars extends CustomPainter {
  _Bars(this.v);
  final List<int> v;
  @override
  void paint(Canvas c, Size s) {
    if (v.isEmpty) return;
    final mx = math.max(1, v.reduce(math.max));
    final w = s.width / v.length;
    for (var i = 0; i < v.length; i++) {
      final h = v[i] == 0 ? 2.0 : math.max(3.0, s.height * v[i] / mx);
      final col = Color.lerp(C.cyan, C.hot, v[i] / mx)!;
      final r = RRect.fromRectAndRadius(Rect.fromLTWH(i * w + 1, s.height - h, w - 2, h), const Radius.circular(2));
      c.drawRRect(r, Paint()..color = v[i] == 0 ? C.line2 : col);
      if (v[i] > 0) c.drawRRect(r, Paint()..color = col.withValues(alpha: 0.4)..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4));
    }
  }

  @override
  bool shouldRepaint(_Bars o) => true;
}
