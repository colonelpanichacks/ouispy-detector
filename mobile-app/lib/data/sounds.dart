// How the phone sounds off. Flutter has no audio player and the app carries no audio
// package, so the Tab5's jingles (tones.dart, made as WAVs in res/raw at five volumes) play
// as Android notification-channel sounds: one channel per event, volume and vibrate setting,
// the stale ones deleted when those change. An event that should only sound (a node down,
// a test) posts a notification that clears itself after 3 s. The find-it beep is the system
// click (SystemSound), which Android plays only with touch sounds on. iOS plays its default
// notification sound for all of them.
import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'model.dart';
import 'settings.dart';
import 'tones.dart';

export 'tones.dart' show Sfx;

class Sounds {
  Sounds._();
  static final I = Sounds._();

  FlutterLocalNotificationsPlugin? _n;
  bool ready = false;
  late Settings s;
  late Model m;
  int _id = 2000;

  static String label(Sfx e) => switch (e) {
    Sfx.detect => 'New device',
    Sfx.watch => 'Watched device',
    Sfx.drone => 'Drone appears',
    Sfx.nodeDown => 'Node down',
    Sfx.nodeUp => 'Node back',
    Sfx.lost => 'Lost node seen',
  };

  /// The event's own switch (ALL SOUNDS and Silence all are checked by [allowed]). A lost
  /// node being seen always sounds while sounds are on: it is the fleet's alarm.
  bool eventOn(Sfx e) => switch (e) {
    Sfx.detect => s.sndDetect,
    Sfx.watch => s.sndWatch,
    Sfx.drone => s.sndDrone,
    Sfx.nodeDown => s.sndNodeDown,
    Sfx.nodeUp => s.sndNodeUp,
    Sfx.lost => true,
  };

  bool allowed(Sfx e) => s.alertSound && !m.muteAll && eventOn(e);

  void bind(FlutterLocalNotificationsPlugin n, bool ok, Settings settings, Model model) {
    _n = n;
    ready = ok;
    s = settings;
    m = model;
    tidy();
    // the volume or vibrate changing makes new channels: drop the old ones
    var was = (vol, s.vibrate);
    s.addListener(() {
      if ((vol, s.vibrate) == was) return;
      was = (vol, s.vibrate);
      tidy();
    });
  }

  AndroidFlutterLocalNotificationsPlugin? get _android => _n?.resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>();

  int get vol => snapVolume(s.volume);
  static String channelOf(Sfx e, int vol, bool vib) => '${toneName(e, vol)}_${vib ? 'v' : 'q'}';
  String get _quiet => s.vibrate ? 'alerts_quiet_v' : 'alerts_quiet_q';

  /// Drop the channels of other volumes / vibrate settings, so the system's list holds one per event.
  Future<void> tidy() async {
    final a = _android;
    if (a == null || !ready) return;
    try {
      final keep = {for (final e in Sfx.values) channelOf(e, vol, s.vibrate), _quiet};
      for (final c in await a.getNotificationChannels() ?? const <AndroidNotificationChannel>[]) {
        if ((c.id.startsWith('sfx_') || c.id.startsWith('alerts_quiet_')) && !keep.contains(c.id)) {
          await a.deleteNotificationChannel(channelId: c.id);
        }
      }
    } catch (_) {}
  }

  static Int64List _buzz(Sfx e) => Int64List.fromList(switch (e) {
    Sfx.detect => [0, 220],
    Sfx.watch => [0, 120, 90, 120, 90, 260],
    Sfx.drone => [0, 300, 120, 300, 120, 300],
    Sfx.nodeDown => [0, 90, 90, 90],
    Sfx.nodeUp => [0, 60],
    Sfx.lost => [0, 500, 200, 500, 200, 500],
  });

  static void _haptic(Sfx? e) {
    switch (e) {
      case Sfx.nodeUp || Sfx.nodeDown:
        HapticFeedback.mediumImpact();
      case null || Sfx.detect:
        HapticFeedback.heavyImpact();
      default:
        HapticFeedback.heavyImpact();
        Future.delayed(const Duration(milliseconds: 180), HapticFeedback.heavyImpact);
        Future.delayed(const Duration(milliseconds: 360), HapticFeedback.heavyImpact);
    }
  }

  /// Say something: a notification when [notify], the event's tone when [sound] is set.
  /// Neither: a buzz in the hand when vibrate is on. Callers check the switches.
  Future<void> out({Sfx? sound, Sfx? kind, required String title, required String body, required bool notify}) async {
    final e = sound ?? kind;
    if (!ready || _n == null || (sound == null && !notify) || !await _enabled()) {
      if (s.vibrate) _haptic(e);
      return;
    }
    final vib = s.vibrate;
    final loud = e != Sfx.nodeDown && e != Sfx.nodeUp; // node news: no heads-up banner
    final android = sound != null
        ? AndroidNotificationDetails(
            channelOf(sound, vol, vib),
            label(sound),
            channelDescription: 'The ${label(sound).toLowerCase()} sound at volume $vol',
            importance: loud ? Importance.high : Importance.defaultImportance,
            priority: loud ? Priority.high : Priority.defaultPriority,
            playSound: true,
            sound: RawResourceAndroidNotificationSound(toneName(sound, vol)),
            enableVibration: vib,
            vibrationPattern: vib ? _buzz(sound) : null,
            timeoutAfter: notify ? null : 3000,
            color: const Color(0xFFFF2BD6),
          )
        : AndroidNotificationDetails(
            _quiet,
            'Alerts without sound',
            channelDescription: 'Alerts while their sound is off',
            importance: Importance.high,
            priority: Priority.high,
            playSound: false,
            enableVibration: vib,
            color: const Color(0xFFFF2BD6),
          );
    try {
      await _n!.show(
        id: _id++ % 1000 + 2000,
        title: title,
        body: body,
        notificationDetails: NotificationDetails(
          android: android,
          iOS: DarwinNotificationDetails(presentAlert: notify, presentBanner: notify, presentList: notify, presentSound: sound != null),
        ),
      );
    } catch (_) {
      if (s.vibrate) _haptic(e);
    }
  }

  bool? _enabledCache;
  DateTime _enabledAt = DateTime(0);
  Future<bool> _enabled() async {
    if (!Platform.isAndroid) return true;
    if (_enabledCache != null && DateTime.now().difference(_enabledAt).inSeconds < 30) return _enabledCache!;
    try {
      _enabledCache = await _android?.areNotificationsEnabled() ?? true;
    } catch (_) {
      _enabledCache = true;
    }
    _enabledAt = DateTime.now();
    return _enabledCache!;
  }

  /// Whether Android lets the app post (and so sound) at all; null off Android.
  Future<bool?> notificationsAllowed() async {
    if (!Platform.isAndroid) return null;
    _enabledCache = null;
    return _enabled();
  }

  /// The Sound page's TEST: the tone at the current volume, whatever the event's switch.
  Future<void> test(Sfx e) => out(sound: e, title: 'Sound test', body: label(e), notify: false);

  DateTime _lastTick = DateTime(0);

  /// The find-it beep, for the proximity page's pulse: the system click.
  void proxTick() {
    if (!s.alertSound || !s.proxBeep || m.muteAll) return;
    final t = DateTime.now();
    if (t.difference(_lastTick).inMilliseconds < 120) return;
    _lastTick = t;
    SystemSound.play(SystemSoundType.click);
  }
}
