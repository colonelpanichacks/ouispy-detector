import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'data/alerts.dart';
import 'data/backend.dart';
import 'data/blue_modes.dart';
import 'data/dashboard.dart';
import 'data/fpv.dart';
import 'data/local_mesh.dart';
import 'data/lookup.dart';
import 'data/model.dart';
import 'data/settings.dart';
import 'data/rid_lookup.dart';
import 'data/updater.dart';
import 'data/usb_link.dart';
import 'data/wardrive.dart';
import 'data/watch.dart';
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
  App._(this.m, this.s, this.dash) : link = dash;
  static late App I;
  final Model m;
  final Settings s;

  /// The home dashboard client. Map tiles always come through it when it is reachable.
  final Dashboard dash;

  /// Where the mesh comes from right now: the dashboard, or the app itself on a node.
  Backend link;
  bool _picking = false;

  /// Settings' source: auto = a node on USB when one is plugged in, else the dashboard.
  Future<void> pick() async {
    if (_picking) return;
    _picking = true;
    try {
      final boards = s.source == 'dashboard' ? const [] : await UsbLink.boards();
      final Backend want;
      if (boards.isNotEmpty) {
        final cur = link;
        if (cur is LocalMesh && cur.link.open) return;
        want = LocalMesh(m, UsbLink(boards.first));
      } else {
        if (link is Dashboard && m.link != Link.off) return;
        want = dash;
      }
      await link.stop();
      m.clearData();
      link = want;
      Fpv.I.attach(want);
      Board.I.attach(want);
      Wardrive.I.attach(want);
      await want.start();
      // a board on USB: ask it what it is (boot lines are long gone when the port opens)
      if (want is LocalMesh) Future.delayed(const Duration(milliseconds: 1500), Board.I.probe);
      m.changed();
    } finally {
      _picking = false;
    }
  }
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setSystemUIOverlayStyle(const SystemUiOverlayStyle(statusBarColor: Colors.transparent, systemNavigationBarColor: C.bg));
  final s = Settings();
  await s.load();
  final m = Model()..muted.addAll(s.muted);
  m.droneWindow = s.droneHours == 0 ? const DroneWindow.all() : DroneWindow.hours(s.droneHours);
  final dash = Dashboard(m, s);
  App.I = App._(m, s, dash);
  await Lookup.I.load();
  await Watch.I.load();
  await Updater.I.load();
  await RidLookup.I.load();
  m.watcher = (d) => Watch.I.match(d, m.targets[d.mac]);
  AlertOut(m, s).start();
  // a node plugged in or pulled out switches the source (auto / usb)
  UsbLink.events?.listen((_) => Future.delayed(const Duration(milliseconds: 600), App.I.pick));
  if (s.source == 'dashboard' || (await UsbLink.boards()).isEmpty) {
    Fpv.I.attach(dash);
    Board.I.attach(dash);
    Wardrive.I.attach(dash);
    await dash.start();
  } else {
    await App.I.pick();
  }
  runApp(const OuiSpyApp());
  Future.delayed(const Duration(seconds: 20), () => Updater.I.maybeAuto(dash));
}

class OuiSpyApp extends StatelessWidget {
  const OuiSpyApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(title: 'OUI-SPY Recon', debugShowCheckedModeBanner: false, theme: buildTheme(), home: const Shell());
}

class _Tab {
  const _Tab(this.label, this.icon, this.on);
  final String label;
  final IconData icon, on;
}

const _tabs = [
  _Tab('Feed', Ph.pulse, PhF.pulse),
  _Tab('Devices', Ph.broadcast, PhF.broadcast),
  _Tab('Map', Ph.mapTrifold, PhF.mapTrifold),
  _Tab('Drones', Ph.drone, PhF.drone),
  _Tab('Fleet', Ph.graph, PhF.graph),
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
    }); // a drone's Show on map / Follow land on the map tab
    showDrone.addListener(_toMap);
    followDrone.addListener(_toMap);
  }

  void _toMap() {
    if ((showDrone.value ?? followDrone.value) == null || !mounted) return;
    setState(() => tab = 2);
    App.I.link.fetchDroneWindow();
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
        body: ReconFrame(
          pulse: pulse,
          banner: true,
          body: IndexedStack(index: tab, children: _pages),
        ),
        bottomNavigationBar: _BottomNav(
          tab: tab,
          onTap: (i) {
            setState(() => tab = i);
            if (i == 3 || i == 2) App.I.link.fetchDroneWindow();
          },
        ),
      ),
    );
  }
}

/// The header every screen shares: the board's art and moon behind the title, the status
/// and the band's two buttons, closed off by a silk rule, then the screen's own content.
class ReconFrame extends StatelessWidget {
  const ReconFrame({super.key, required this.pulse, required this.body, this.leading, this.trailing, this.banner = false});
  final Animation<double> pulse;
  final Widget body;
  final Widget? leading, trailing;
  final bool banner;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ListenableBuilder(
      listenable: m,
      builder: (context, _) => Stack(
        children: [
          // the art lives in the header only: clipped at its foot, where a silk rule closes it off
          Positioned(
            top: 0,
            left: 0,
            right: 0,
            height: MediaQuery.paddingOf(context).top + _titleH(context) + _barH(context) + 6,
            child: Container(
              decoration: const BoxDecoration(
                border: Border(bottom: BorderSide(color: C.silkLine)),
              ),
              child: ClipRect(
                child: Stack(
                  children: [
                    Positioned(
                      top: MediaQuery.paddingOf(context).top + _titleH(context) + _barH(context) / 2 - _bandCentre(context),
                      left: 0,
                      right: 0,
                      child: const HeaderArt(),
                    ),
                    Positioned(
                      top: MediaQuery.paddingOf(context).top + _titleH(context) + _barH(context) / 2 - _moonD(context) / 2,
                      left: MediaQuery.sizeOf(context).width / 2 - _moonD(context) / 2,
                      child: Moon(size: _moonD(context), live: isLive(m)),
                    ),
                  ],
                ),
              ),
            ),
          ),
          SafeArea(
            bottom: false,
            child: Column(
              children: [
                _TopBar(pulse: pulse, leading: leading, trailing: trailing),
                const SizedBox(height: 7),
                if (banner && m.alertsUnacked > 0 && !m.muteAll && m.alerts.isNotEmpty) _AlertBanner(pulse: pulse),
                Expanded(child: body),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// A page opened from a tab, under the same header: back in the bell's place, the page's
/// name printed in silk under the rule, its actions beside the name.
class ReconPage extends StatefulWidget {
  const ReconPage({super.key, required this.title, required this.body, this.actions = const []});
  final String title;
  final Widget body;
  final List<Widget> actions;
  @override
  State<ReconPage> createState() => _ReconPageState();
}

class _ReconPageState extends State<ReconPage> with SingleTickerProviderStateMixin {
  late final AnimationController pulse = AnimationController(vsync: this, duration: const Duration(milliseconds: 1400))..repeat();

  @override
  void dispose() {
    pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: ReconFrame(
      pulse: pulse,
      leading: _RoundBtn(Ph.arrowLeft, C.text, () => Navigator.of(context).maybePop()),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 2, 8, 2),
            child: SizedBox(
              height: 36,
              child: Row(
                children: [
                  Text(widget.title.toUpperCase(), style: T.label.copyWith(fontSize: 12, color: C.soft)),
                  const SizedBox(width: 10),
                  Expanded(child: Container(height: 1, color: C.silkLine)),
                  ...widget.actions,
                ],
              ),
            ),
          ),
          Expanded(child: widget.body),
        ],
      ),
    ),
  );
}

class _TopBar extends StatelessWidget {
  const _TopBar({required this.pulse, this.leading, this.trailing});
  final Animation<double> pulse;
  final Widget? leading, trailing; // the band's two ends: the bell and settings, or a page's back and its action
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final (String state, Color c) = switch (m.link) {
      Link.off => (m.lastError.startsWith('looking') ? 'Searching' : 'Offline', C.red),
      Link.refused => ('Refused', C.red),
      Link.connecting => ('Connecting', C.amber),
      Link.up =>
        m.mode == 'demo'
            ? ('Demo', C.amber)
            : m.mode == 'replay'
            ? ('Replay', C.violet)
            : m.home['alive'] == false
            ? ('Home down', C.amber)
            : ('Live', C.green),
    };
    final down = c == C.red;
    final live = isLive(m);
    // cut out of the art like the title: a black outline round the letters
    const cut = [
      Shadow(color: C.bg, blurRadius: 0, offset: Offset(1.2, 0)),
      Shadow(color: C.bg, blurRadius: 0, offset: Offset(-1.2, 0)),
      Shadow(color: C.bg, blurRadius: 0, offset: Offset(0, 1.2)),
      Shadow(color: C.bg, blurRadius: 0, offset: Offset(0, -1.2)),
      Shadow(color: C.bg, blurRadius: 3),
    ];
    final status = GestureDetector(
      onTap: () => _open(context, const SettingsView()),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (!live) ...[
            AnimatedBuilder(
              animation: pulse,
              builder: (_, _) => Container(
                padding: const EdgeInsets.all(1.5),
                decoration: const BoxDecoration(color: C.bg, shape: BoxShape.circle),
                child: Dot(c.withValues(alpha: down ? 0.45 + 0.55 * math.sin(pulse.value * 2 * math.pi).abs() : 1), size: 7, halo: !down),
              ),
            ),
            const SizedBox(width: 6),
          ],
          Text(
            state.toUpperCase(),
            style: TextStyle(fontFamily: mono, color: c, fontSize: 11.5, fontWeight: FontWeight.w600, letterSpacing: 1.6, shadows: cut),
          ),
        ],
      ),
    );
    // the band's ends, outside the cicadas' wings (16 % of the width each side)
    final end = MediaQuery.sizeOf(context).width * 0.16;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          height: _titleH(context) - _statusH,
          child: Center(child: TitleArt(width: _titleW(context))),
        ),
        // the status sits right above the moon, tucked against it
        SizedBox(
          height: _statusH,
          child: Align(alignment: Alignment.bottomCenter, child: status),
        ),
        SizedBox(
          height: _barH(context),
          child: Row(
            children: [
              SizedBox(
                width: end,
                child: Center(
                  child:
                      leading ??
                      Stack(
                        clipBehavior: Clip.none,
                        children: [
                          _RoundBtn(m.alertsUnacked > 0 ? PhF.bellRinging : Ph.bell, m.alertsUnacked > 0 ? C.hot : C.text, () => _open(context, const AlertsView())),
                          if (m.alertsUnacked > 0) Positioned(right: -2, top: -2, child: _Badge(m.alertsUnacked, C.hot)),
                        ],
                      ),
                ),
              ),
              const Spacer(),
              SizedBox(
                width: end,
                child: Center(child: trailing ?? _RoundBtn(Ph.gearSix, C.text, () => _open(context, const SettingsView()))),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

const _statusH = 18.0;

/// An icon button on a dark disc, so it reads over the art.
class _RoundBtn extends StatelessWidget {
  const _RoundBtn(this.icon, this.color, this.onTap);
  final IconData icon;
  final Color color;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) => Padding(
    padding: EdgeInsets.zero,
    child: Material(
      color: C.bg.withValues(alpha: 0.85),
      shape: const CircleBorder(side: BorderSide(color: C.line2)),
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: SizedBox(width: 40, height: 40, child: Icon(icon, size: 20, color: color)),
      ),
    ),
  );
}

/// The board's back strip as silkscreen behind the top bar: the two cicadas facing the
/// full moon, rings radiating out from it (tool/make_header_art.py). Light grey at the
/// moon, darker grey out where the title and buttons sit, so it never fights them.
/// The board's own OUI SPY RECON banner (tool/make_title_art.py), cut out of the art
/// behind it by a black outline: OUI SPY in the text colour, RECON in the accent.
class TitleArt extends StatelessWidget {
  const TitleArt({super.key, this.width = 168});
  final double width;
  @override
  Widget build(BuildContext context) => Semantics(
    label: 'OUI-SPY Recon',
    child: ShaderMask(
      blendMode: BlendMode.modulate,
      shaderCallback: (r) => const LinearGradient(colors: [C.text, C.text, C.accent, C.accent], stops: [0, 0.545, 0.545, 1]).createShader(r),
      child: Image.asset('assets/art/title.png', width: width, filterQuality: FilterQuality.high),
    ),
  );
}

/// The link is up and the mesh is live (the status reads LIVE).
bool isLive(Model m) => m.link == Link.up && m.mode != 'demo' && m.mode != 'replay' && m.home['alive'] != false;

/// The board's full moon as the live light: green when live, the pale moon's own grey when
/// not, its silkscreen texture kept either way.
class Moon extends StatelessWidget {
  const Moon({super.key, required this.size, required this.live});
  final double size;
  final bool live;
  @override
  Widget build(BuildContext context) => AnimatedContainer(
    duration: const Duration(milliseconds: 400),
    width: size,
    height: size,
    decoration: BoxDecoration(
      shape: BoxShape.circle,
      boxShadow: live ? [BoxShadow(color: C.green.withValues(alpha: 0.45), blurRadius: 14, spreadRadius: 1)] : null,
    ),
    child: Image.asset(
      'assets/art/moon.png',
      width: size,
      height: size,
      color: live ? C.green : const Color(0xFFB4BAC2),
      colorBlendMode: BlendMode.srcIn,
      filterQuality: FilterQuality.high,
    ),
  );
}

class HeaderArt extends StatelessWidget {
  const HeaderArt({super.key});
  @override
  Widget build(BuildContext context) => IgnorePointer(
    child: ShaderMask(
      blendMode: BlendMode.srcIn,
      shaderCallback: (r) => const LinearGradient(colors: [Color(0xFF262B32), Color(0xFF9AA1AA), Color(0xFF262B32)], stops: [0.04, 0.5, 0.96]).createShader(r),
      child: Opacity(
        opacity: 0.6,
        child: Image.asset(
          'assets/art/header.png',
          width: MediaQuery.sizeOf(context).width,
          fit: BoxFit.fitWidth,
          alignment: Alignment.topCenter,
          filterQuality: FilterQuality.medium,
        ),
      ),
    ),
  );
}

// header.png is 60 mm wide: 10 mm of rings fading up past the title, then the band (cicadas and moon, 8.2 mm), then the tail
double _bandCentre(BuildContext context) => MediaQuery.sizeOf(context).width * (10.0 + 4.1) / 60;
// the title (board banner, 1620 x 199) is centred above the band
double _titleW(BuildContext context) => (MediaQuery.sizeOf(context).width * 0.8).clamp(240.0, 420.0);
double _titleH(BuildContext context) => _titleW(context) * 199 / 1620 + 12 + _statusH;
// the moon: 4.6 mm across on the 60 mm strip (tool/make_header_art.py MOON_D)
double _moonD(BuildContext context) => MediaQuery.sizeOf(context).width * 4.6 / 60;
double _barH(BuildContext context) => (MediaQuery.sizeOf(context).width * 8.2 / 60).clamp(56.0, 76.0);

class _Badge extends StatelessWidget {
  const _Badge(this.n, this.c);
  final int n;
  final Color c;
  @override
  Widget build(BuildContext context) => Container(
    constraints: const BoxConstraints(minWidth: 17),
    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1.5),
    decoration: BoxDecoration(
      color: c,
      borderRadius: BorderRadius.circular(9),
      border: Border.all(color: C.bg, width: 1.5),
    ),
    child: Text(
      n > 99 ? '99+' : '$n',
      textAlign: TextAlign.center,
      style: const TextStyle(fontFamily: sans, color: Colors.white, fontSize: 10, fontWeight: FontWeight.w700),
    ),
  );
}

void _open(BuildContext context, Widget page) => Navigator.of(context).push(MaterialPageRoute(builder: (_) => page));

class _AlertBanner extends StatelessWidget {
  const _AlertBanner({required this.pulse});
  final Animation<double> pulse;
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final d = m.alerts.first.det;
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
      child: AnimatedBuilder(
        animation: pulse,
        builder: (_, child) {
          final a = 0.5 + 0.5 * math.sin(pulse.value * 2 * math.pi).abs();
          return Container(
            decoration: BoxDecoration(
              color: C.hot.withValues(alpha: 0.10),
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: C.hot.withValues(alpha: 0.35 + 0.35 * a)),
              boxShadow: [BoxShadow(color: C.hot.withValues(alpha: 0.18 * a), blurRadius: 16)],
            ),
            child: child,
          );
        },
        child: Material(
          type: MaterialType.transparency,
          child: InkWell(
            borderRadius: BorderRadius.circular(14),
            onTap: () => _open(context, const AlertsView()),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 10, 8, 10),
              child: Row(
                children: [
                  const Icon(PhF.warning, color: C.hot, size: 20),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          m.alertsUnacked == 1 ? 'New device' : '${m.alertsUnacked} new devices',
                          style: const TextStyle(fontFamily: sans, color: C.hot, fontSize: 13.5, fontWeight: FontWeight.w600),
                        ),
                        const SizedBox(height: 1),
                        Text(
                          '${m.labelOf(d.mac, d.descr)}  ·  ${m.nodeName(d.node)}  ·  ${d.rssi ?? '-'} dBm',
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: T.meta.copyWith(color: C.soft),
                        ),
                      ],
                    ),
                  ),
                  TextButton(
                    onPressed: m.ackAlerts,
                    child: const Text(
                      'Dismiss',
                      style: TextStyle(fontFamily: sans, color: C.text, fontWeight: FontWeight.w600),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
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
      decoration: const BoxDecoration(
        color: C.bg,
        border: Border(top: BorderSide(color: C.silkLine)),
      ),
      child: SafeArea(
        top: false,
        child: SizedBox(
          height: 62,
          child: Row(
            children: [
              for (var i = 0; i < _tabs.length; i++)
                Expanded(
                  child: InkResponse(
                    onTap: () => onTap(i),
                    radius: 36,
                    child: Stack(
                      children: [
                        // the chosen tab is lit by a rule along the top edge
                        Positioned(
                          top: 0,
                          left: 18,
                          right: 18,
                          child: AnimatedContainer(
                            duration: const Duration(milliseconds: 180),
                            height: 2,
                            decoration: BoxDecoration(
                              color: tab == i ? C.accent : Colors.transparent,
                              boxShadow: tab == i ? [BoxShadow(color: C.accent.withValues(alpha: 0.6), blurRadius: 6)] : null,
                            ),
                          ),
                        ),
                        Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Stack(
                                clipBehavior: Clip.none,
                                children: [
                                  Icon(tab == i ? _tabs[i].on : _tabs[i].icon, size: 21, color: tab == i ? C.accent : C.silk),
                                  if (i == 3 && m.liveDrones > 0) Positioned(right: -10, top: -6, child: _Badge(m.liveDrones, C.hot)),
                                ],
                              ),
                              const SizedBox(height: 5),
                              Text(
                                _tabs[i].label.toUpperCase(),
                                style: TextStyle(fontFamily: mono, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 1.2, color: tab == i ? C.text : C.silk),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
