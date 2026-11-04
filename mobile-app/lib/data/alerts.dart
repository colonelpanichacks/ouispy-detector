// Phone-side alerting: a notification and a buzz per alert (each one switchable), the
// in-app banner draws from the model.
import 'package:flutter/services.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'model.dart';
import 'settings.dart';

class AlertOut {
  AlertOut(this.m, this.s);
  final Model m;
  final Settings s;
  final _n = FlutterLocalNotificationsPlugin();
  bool _ready = false;
  int _id = 1;

  Future<void> start() async {
    try {
      _ready = await _n.initialize(
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
    m.alertStream.listen(_fire);
  }

  Future<void> _fire(Alert a) async {
    if (s.vibrate) HapticFeedback.heavyImpact();
    if (!s.notify || !_ready) return;
    final d = a.det;
    final who = m.labelOf(d.mac, d.descr);
    try {
      await _n.show(
        id: _id++ % 1000,
        title: a.watch != null ? 'WATCHED  ${a.watch}' : 'OUI-SPY ALERT  ${d.type.isEmpty ? d.dom.toUpperCase() : d.type}',
        body: '$who  ${d.rssi ?? ''} dBm  heard by ${m.nodeName(d.node)}',
        notificationDetails: NotificationDetails(
          android: AndroidNotificationDetails('alerts', 'Detections',
              channelDescription: 'A device the mesh alerts on',
              importance: Importance.high,
              priority: Priority.high,
              playSound: s.alertSound,
              enableVibration: s.vibrate,
              color: const Color(0xFFFF2BD6)),
          iOS: DarwinNotificationDetails(presentSound: s.alertSound),
        ),
      );
    } catch (_) {}
  }
}
