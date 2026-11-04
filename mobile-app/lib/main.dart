import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'data/alerts.dart';
import 'data/dashboard.dart';
import 'data/model.dart';
import 'data/settings.dart';
import 'theme.dart';
import 'views/alerts_view.dart';
import 'views/devices_view.dart';
import 'views/drones_view.dart';
import 'views/feed_view.dart';
import 'views/fleet_view.dart';
import 'views/map_view.dart';
import 'views/settings_view.dart';

/// Everything the views share.
class App {
  App._(this.m, this.s, this.dash);
  static late App I;
  final Model m;
  final Settings s;
  final Dashboard dash;
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setSystemUIOverlayStyle(const SystemUiOverlayStyle(statusBarColor: C.bg, systemNavigationBarColor: C.bg));
  final s = Settings();
  await s.load();
  final m = Model()..muted.addAll(s.muted);
  m.droneWindow = s.droneHours == 0 ? const DroneWindow.all() : DroneWindow.hours(s.droneHours);
  final dash = Dashboard(m, s);
  App.I = App._(m, s, dash);
  AlertOut(m, s).start();
  dash.start();
  runApp(const OuiSpyApp());
}

class OuiSpyApp extends StatelessWidget {
  const OuiSpyApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'OUI-SPY Mesh',
        debugShowCheckedModeBanner: false,
        theme: buildTheme(),
        home: const Shell(),
      );
}

class _Tab {
  const _Tab(this.label, this.icon, this.color);
  final String label;
  final IconData icon;
  final Color color;
}

const _tabs = [
  _Tab('FEED', Icons.bolt, C.accent),
  _Tab('DEVICES', Icons.radar, C.cyan),
  _Tab('MAP', Icons.map_outlined, C.green),
  _Tab('DRONES', Icons.flight, C.hot),
  _Tab('FLEET', Icons.hub_outlined, C.orange),
];

class Shell extends StatefulWidget {
  const Shell({super.key});
  @override
  State<Shell> createState() => _ShellState();
}

class _ShellState extends State<Shell> with SingleTickerProviderStateMixin {
  int tab = 0;
  late final AnimationController pulse = AnimationController(vsync: this, duration: const Duration(milliseconds: 1400))..repeat();
  StreamSubscription<String>? _toasts;
  final _pages = const [FeedView(), DevicesView(), MapView(), DronesView(), FleetView()];

  @override
  void initState() {
    super.initState();
    _toasts = App.I.m.toasts.listen((t) {
      if (!mounted) return;
      ScaffoldMessenger.of(context)
        ..hideCurrentSnackBar()
        ..showSnackBar(SnackBar(content: Text(t), duration: const Duration(seconds: 2)));
    });
  }

  @override
  void dispose() {
    pulse.dispose();
    _toasts?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) => Scaffold(
        body: SafeArea(
          bottom: false,
          child: Column(children: [
            _TopBar(pulse: pulse),
            if (m.alertsUnacked > 0 && !m.muteAll && m.alerts.isNotEmpty) _AlertBanner(pulse: pulse),
            Expanded(child: IndexedStack(index: tab, children: _pages)),
          ]),
        ),
        bottomNavigationBar: _BottomNav(tab: tab, onTap: (i) {
          setState(() => tab = i);
          if (i == 3 || i == 2) App.I.dash.fetchDroneWindow();
        }),
      ),
    );
  }
}

class _TopBar extends StatelessWidget {
  const _TopBar({required this.pulse});
  final Animation<double> pulse;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final (String state, Color c) = switch (m.link) {
      Link.off => (m.lastError.startsWith('looking') ? 'SEARCHING' : 'NO LINK', C.red),
      Link.refused => ('REFUSED', C.red),
      Link.connecting => ('CONNECTING', C.amber),
      Link.up => m.mode == 'demo'
          ? ('DEMO', C.amber)
          : m.mode == 'replay'
              ? ('REPLAY', C.purple)
              : m.home['alive'] == false
                  ? ('HOME DOWN', C.amber)
                  : ('LIVE', C.green),
    };
    final down = c == C.red;
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 6, 6, 6),
      decoration: const BoxDecoration(border: Border(bottom: BorderSide(color: C.line2))),
      child: Row(children: [
        const Column(crossAxisAlignment: CrossAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
          Glow('OUI-SPY', color: C.accent, size: 18, spacing: 1.5),
          Glow('M E S H', color: C.pink, size: 10.5, spacing: 2.5),
        ]),
        const Spacer(),
        GestureDetector(
          onTap: () => _open(context, const SettingsView()),
          child: AnimatedBuilder(
            animation: pulse,
            builder: (_, _) {
              final a = down ? 0.55 + 0.45 * math.sin(pulse.value * 2 * math.pi) : 1.0;
              return Container(
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                decoration: BoxDecoration(
                  color: c.withValues(alpha: 0.13),
                  borderRadius: BorderRadius.circular(20),
                  border: Border.all(color: c.withValues(alpha: a)),
                  boxShadow: [BoxShadow(color: c.withValues(alpha: 0.35 * a), blurRadius: 10)],
                ),
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  Dot(c.withValues(alpha: a), size: 8),
                  const SizedBox(width: 7),
                  Glow(state, color: c, size: 12, spacing: 1.2),
                ]),
              );
            },
          ),
        ),
        _iconBadge(context, Icons.notifications_active_outlined, m.alertsUnacked, C.hot, () => _open(context, const AlertsView())),
        IconButton(icon: const Icon(Icons.tune, color: C.soft), onPressed: () => _open(context, const SettingsView())),
      ]),
    );
  }

  Widget _iconBadge(BuildContext context, IconData icon, int n, Color c, VoidCallback onTap) => Stack(clipBehavior: Clip.none, children: [
        IconButton(icon: Icon(icon, color: n > 0 ? c : C.soft), onPressed: onTap),
        if (n > 0)
          Positioned(
            right: 4,
            top: 4,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
              decoration: BoxDecoration(color: c, borderRadius: BorderRadius.circular(9), boxShadow: [BoxShadow(color: c, blurRadius: 6)]),
              child: Text(n > 99 ? '99+' : '$n', style: const TextStyle(color: C.bg, fontSize: 10, fontWeight: FontWeight.w800)),
            ),
          ),
      ]);
}

void _open(BuildContext context, Widget page) => Navigator.of(context).push(MaterialPageRoute(builder: (_) => page));

class _AlertBanner extends StatelessWidget {
  const _AlertBanner({required this.pulse});
  final Animation<double> pulse;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final d = m.alerts.first.det;
    return AnimatedBuilder(
      animation: pulse,
      builder: (_, _) {
        final a = 0.75 + 0.25 * math.sin(pulse.value * 2 * math.pi);
        return GestureDetector(
          onTap: () => _open(context, const AlertsView()),
          child: Container(
            margin: const EdgeInsets.fromLTRB(10, 6, 10, 2),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(
              color: C.hot.withValues(alpha: 0.14 * a),
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: C.hot.withValues(alpha: a)),
              boxShadow: [BoxShadow(color: C.hot.withValues(alpha: 0.35 * a), blurRadius: 14)],
            ),
            child: Row(children: [
              Glow('ALERT x${m.alertsUnacked}', color: C.hot, size: 13, spacing: 1.2),
              const SizedBox(width: 10),
              Expanded(
                child: Text('${m.labelOf(d.mac, d.descr)}  ${m.nodeName(d.node)}  ${d.rssi ?? ''} dBm',
                    overflow: TextOverflow.ellipsis, style: const TextStyle(color: C.text, fontSize: 13)),
              ),
              GestureDetector(onTap: m.ackAlerts, child: const Pill('ACK', color: C.green, small: true)),
            ]),
          ),
        );
      },
    );
  }
}

class _BottomNav extends StatelessWidget {
  const _BottomNav({required this.tab, required this.onTap});
  final int tab;
  final ValueChanged<int> onTap;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return Container(
      decoration: const BoxDecoration(color: C.bg, border: Border(top: BorderSide(color: C.line2))),
      child: SafeArea(
        top: false,
        child: SizedBox(
          height: 60,
          child: Row(children: [
            for (var i = 0; i < _tabs.length; i++)
              Expanded(
                child: InkWell(
                  onTap: () => onTap(i),
                  child: Stack(alignment: Alignment.center, children: [
                    if (tab == i)
                      Positioned(
                          top: 0,
                          left: 14,
                          right: 14,
                          child: Container(height: 3, decoration: BoxDecoration(color: _tabs[i].color, boxShadow: [BoxShadow(color: _tabs[i].color, blurRadius: 8)]))),
                    Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                      Icon(_tabs[i].icon, size: 22, color: tab == i ? _tabs[i].color : C.muted, shadows: tab == i ? glowOf(_tabs[i].color) : null),
                      const SizedBox(height: 3),
                      Text(_tabs[i].label,
                          style: TextStyle(
                              fontSize: 10.5,
                              fontWeight: FontWeight.w800,
                              letterSpacing: 1,
                              color: tab == i ? _tabs[i].color : C.muted,
                              shadows: tab == i ? glowOf(_tabs[i].color, 6) : null)),
                    ]),
                    if (i == 3 && m.liveDrones > 0)
                      Positioned(
                        top: 5,
                        right: 14,
                        child: Container(
                          padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
                          decoration: BoxDecoration(color: C.hot, borderRadius: BorderRadius.circular(9), boxShadow: const [BoxShadow(color: C.hot, blurRadius: 6)]),
                          child: Text('${m.liveDrones}', style: const TextStyle(color: C.bg, fontSize: 10, fontWeight: FontWeight.w800)),
                        ),
                      ),
                  ]),
                ),
              ),
          ]),
        ),
      ),
    );
  }
}
