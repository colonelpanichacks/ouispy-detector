// Phone-side alerting: each alert (a new or watched device), a drone turning up, a node
// dropping off or coming back and a lost node being seen becomes a notification, a sound
// and a buzz, each switchable (Settings, Sound); the in-app banner and toasts come from the
// model. Silence all stops every one of them.
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'model.dart';
import 'settings.dart';
import 'sounds.dart';

const detectGapS = 2.0; // the dashboard's ALERT_GAP_MS: a burst of alerts is one sound
const droneGapS = 2.0;
const nodeSoundGapS = 30.0; // the Tab5: a flapping link sounds once per 30 s at most

class AlertOut {
  AlertOut(this.m, this.s) {
    I = this;
  }
  static AlertOut? I;
  final Model m;
  final Settings s;
  final _n = FlutterLocalNotificationsPlugin();
  bool _ready = false;
  final Map<Sfx, double> _lastSound = {};

  Future<void> start() async {
    try {
      _ready =
          await _n.initialize(
            settings: const InitializationSettings(
              android: AndroidInitializationSettings('@mipmap/ic_launcher'),
              iOS: DarwinInitializationSettings(requestAlertPermission: true, requestSoundPermission: true, requestBadgePermission: true),
            ),
          ) ??
          false;
      await _n.resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()?.requestNotificationsPermission();
    } catch (_) {
      _ready = false;
    }
    Sounds.I.bind(_n, _ready, s, m);
    m.alertStream.listen(_fire);
    m.meshEvents.listen(_event);
  }

  static double get _wall => DateTime.now().millisecondsSinceEpoch / 1000;

  /// The event's tone if its switches allow it and it has not just sounded.
  Sfx? _sound(Sfx e, double gapS, {Iterable<Sfx>? shares}) {
    if (!Sounds.I.allowed(e)) return null;
    final t = _wall;
    for (final k in [e, ...?shares]) {
      if (t - (_lastSound[k] ?? -1e9) < gapS) return null;
    }
    _lastSound[e] = t;
    return e;
  }

  Future<void> _fire(Alert a) async {
    if (m.muteAll) return;
    final d = a.det;
    final who = m.labelOf(d.mac, d.descr);
    final e = a.watch != null ? Sfx.watch : Sfx.detect;
    await Sounds.I.out(
      sound: _sound(e, detectGapS, shares: const [Sfx.detect, Sfx.watch]),
      kind: e,
      title: a.watch != null ? 'WATCHED  ${a.watch}' : 'OUI-SPY ALERT  ${d.type.isEmpty ? d.dom.toUpperCase() : d.type}',
      body: '$who  ${d.rssi ?? ''} dBm  heard by ${m.nodeName(d.node)}',
      notify: s.notify && _ready,
    );
  }

  Future<void> _event(MeshEvent e) async {
    if (m.muteAll) return;
    switch (e.kind) {
      case MeshEventKind.drone:
        await Sounds.I.out(sound: _sound(Sfx.drone, droneGapS), kind: Sfx.drone, title: e.title, body: e.body, notify: s.notify);
      case MeshEventKind.nodeDown || MeshEventKind.nodeUp:
        // a toast in the app (the model's); here only the jingle
        final x = e.kind == MeshEventKind.nodeDown ? Sfx.nodeDown : Sfx.nodeUp;
        final snd = _sound(x, nodeSoundGapS, shares: const [Sfx.nodeDown, Sfx.nodeUp]);
        if (snd != null) await Sounds.I.out(sound: snd, title: e.title, body: e.body, notify: false);
      case MeshEventKind.lostNode:
        await Sounds.I.out(sound: _sound(Sfx.lost, droneGapS), kind: Sfx.lost, title: e.title, body: e.body, notify: s.notify);
    }
  }
}
