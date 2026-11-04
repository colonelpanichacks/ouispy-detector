// FIND IT: the Tab5's proximity mode on the phone. One device's signal, as heard by the
// node closest to it, drives a pulse that quickens as it gets stronger (the Tab5's curve:
// 1400 ms at -95 dBm down to 150 ms at -35), felt as a buzz and, if wanted, heard.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../main.dart';
import '../theme.dart';

class ProximityView extends StatefulWidget {
  const ProximityView({super.key, required this.mac, required this.title});
  final String mac, title;
  @override
  State<ProximityView> createState() => _ProximityViewState();
}

class _ProximityViewState extends State<ProximityView> with SingleTickerProviderStateMixin {
  final _hist = <(double, int)>[]; // (ts, rssi)
  Timer? _pulse;
  bool buzz = true, beep = false;
  double _lastBest = 0;
  late final AnimationController ring = AnimationController(vsync: this, duration: const Duration(milliseconds: 700));

  int? get rssi {
    final t = App.I.m.targets[widget.mac];
    if (t == null || App.I.m.now - t.last > 20) return null; // not heard for 20 s: gone quiet
    return t.best;
  }

  @override
  void initState() {
    super.initState();
    App.I.m.addListener(_heard);
    _schedule();
  }

  @override
  void dispose() {
    App.I.m.removeListener(_heard);
    _pulse?.cancel();
    ring.dispose();
    super.dispose();
  }

  void _heard() {
    final t = App.I.m.targets[widget.mac];
    if (t == null || t.best == null || t.last == _lastBest) return;
    _lastBest = t.last;
    setState(() {
      _hist.add((t.last, t.best!));
      if (_hist.length > 120) _hist.removeAt(0);
    });
  }

  /// The Tab5's curve: weaker = slower.
  Duration _interval(int? r) {
    if (r == null) return const Duration(milliseconds: 2000);
    final f = ((r + 95) / 60).clamp(0.0, 1.0);
    return Duration(milliseconds: (1400 - f * 1250).round());
  }

  void _schedule() {
    _pulse?.cancel();
    _pulse = Timer(_interval(rssi), () {
      if (!mounted) return;
      final r = rssi;
      if (r != null) {
        ring.forward(from: 0);
        if (buzz) r > -60 ? HapticFeedback.heavyImpact() : HapticFeedback.mediumImpact();
        if (beep) SystemSound.play(SystemSoundType.click);
      }
      _schedule();
    });
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ReconPage(
      title: 'FIND IT',
      actions: const [],

      body: ListenableBuilder(
        listenable: m,
        builder: (_, _) {
          final r = rssi;
          final t = m.targets[widget.mac];
          final c = C.rssi(r);
          final near = t?.seen.isNotEmpty == true ? m.nodeName(t!.seen.first.node) : null;
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(widget.title, style: T.title),
                    const SizedBox(height: 4),
                    Text(widget.mac, style: T.code.copyWith(fontSize: 13)),
                  ],
                ),
              ),
              const SizedBox(height: 18),
              // the readout: a ring that pulses with the beat, the dBm in its middle
              SizedBox(
                height: 260,
                child: AnimatedBuilder(
                  animation: ring,
                  builder: (_, _) => CustomPaint(
                    painter: _Ring(ring.value, r == null ? C.silk : c),
                    child: Center(
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Text(r == null ? '--' : '$r', style: T.num.copyWith(fontSize: 64, color: r == null ? C.silk : c, letterSpacing: -2)),
                          Text(r == null ? 'NOT HEARD FOR 20 S' : 'DBM', style: T.silk),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 20),
                child: Text(
                  r == null
                      ? 'Waiting for a node to hear it again.'
                      : (near != null ? 'Strongest at $near. Walk toward the faster pulse.' : 'Walk toward the faster pulse.'),
                  textAlign: TextAlign.center,
                  style: T.body,
                ),
              ),
              const SectionHead('Last two minutes'),
              Panel(
                margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
                child: SizedBox(
                  height: 90,
                  child: CustomPaint(painter: _Trace(_hist, m.now), size: Size.infinite),
                ),
              ),
              if (t != null && t.seen.length > 1)
                Group(
                  title: 'Heard by',
                  children: [for (final s in t.seen) KV(m.nodeName(s.node), '${s.rssi} dBm', color: C.rssi(s.rssi))],
                ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: ActionStrip([
                  (buzz ? 'Buzz on' : 'Buzz off', Ph.vibrate, buzz ? C.accent : C.silk, () => setState(() => buzz = !buzz)),
                  (beep ? 'Beep on' : 'Beep off', beep ? Ph.speakerHigh : Ph.speakerSlash, beep ? C.accent : C.silk, () => setState(() => beep = !beep)),
                  ('Stop', Ph.x, C.text, () => Navigator.pop(context)),
                ]),
              ),
            ],
          );
        },
      ),
    );
  }
}

class _Ring extends CustomPainter {
  _Ring(this.t, this.c);
  final double t;
  final Color c;
  @override
  void paint(Canvas canvas, Size s) {
    final o = s.center(Offset.zero);
    final r0 = math.min(s.width, s.height) * 0.36;
    canvas.drawCircle(
      o,
      r0,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.2
        ..color = C.silkLine,
    );
    for (var k = 0; k < 2; k++) {
      final p = (t + k * 0.5) % 1.0;
      canvas.drawCircle(
        o,
        r0 + p * r0 * 0.4,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2
          ..color = c.withValues(alpha: (1 - p) * 0.6),
      );
    }
    canvas.drawCircle(
      o,
      r0,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..color = c.withValues(alpha: 0.8),
    );
  }

  @override
  bool shouldRepaint(_Ring o) => o.t != t || o.c != c;
}

class _Trace extends CustomPainter {
  _Trace(this.h, this.now);
  final List<(double, int)> h;
  final double now;
  @override
  void paint(Canvas c, Size s) {
    double y(int r) => s.height - ((r + 100) / 70).clamp(0.0, 1.0) * s.height;
    for (final g in [-90, -70, -50]) {
      c.drawLine(
        Offset(0, y(g)),
        Offset(s.width, y(g)),
        Paint()
          ..color = C.silkLine
          ..strokeWidth = 1,
      );
    }
    final pts = [
      for (final (ts, r) in h)
        if (now - ts <= 120) Offset(s.width * (1 - (now - ts) / 120), y(r)),
    ];
    if (pts.length < 2) return;
    final path = Path()..moveTo(pts.first.dx, pts.first.dy);
    for (final p in pts.skip(1)) {
      path.lineTo(p.dx, p.dy);
    }
    c.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..color = C.accent,
    );
  }

  @override
  bool shouldRepaint(_Trace o) => true;
}
