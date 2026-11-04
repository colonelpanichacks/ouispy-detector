// What the app keeps on the phone. The access token stays in the app's private storage and
// is only ever sent to the dashboard it was typed for.
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class Settings extends ChangeNotifier {
  late SharedPreferences _p;

  String host = ''; // blank = find the dashboard on the LAN (mDNS _ouispy._tcp)
  int port = 5077;
  String token = '';
  String tileLayer = 'esri-sat';
  bool tilesOnline = true; // false: cached tiles only, the dashboard is never asked
  bool vibrate = true;
  bool notify = true;
  bool alertSound = true; // the Sound page's ALL SOUNDS
  // which events make a sound (sounds.dart), the volume they play at, the find-it beep
  bool sndDetect = true, sndWatch = true, sndDrone = true, sndNodeDown = true, sndNodeUp = true, proxBeep = false;
  int volume = 80; // 20..100, steps of 20
  Set<String> muted = {};
  int droneHours = 24;
  String source = 'auto'; // auto: a node on USB when one is plugged in, else the dashboard; usb; dashboard

  Future<void> load() async {
    _p = await SharedPreferences.getInstance();
    host = _p.getString('host') ?? '';
    port = _p.getInt('port') ?? 5077;
    token = _p.getString('token') ?? '';
    tileLayer = _p.getString('tileLayer') ?? 'esri-sat';
    tilesOnline = _p.getBool('tilesOnline') ?? true;
    vibrate = _p.getBool('vibrate') ?? true;
    notify = _p.getBool('notify') ?? true;
    alertSound = _p.getBool('alertSound') ?? true;
    sndDetect = _p.getBool('sndDetect') ?? true;
    sndWatch = _p.getBool('sndWatch') ?? true;
    sndDrone = _p.getBool('sndDrone') ?? true;
    sndNodeDown = _p.getBool('sndNodeDown') ?? true;
    sndNodeUp = _p.getBool('sndNodeUp') ?? true;
    proxBeep = _p.getBool('proxBeep') ?? false;
    volume = (_p.getInt('volume') ?? 80).clamp(20, 100);
    muted = (_p.getStringList('muted') ?? const []).toSet();
    droneHours = _p.getInt('droneHours') ?? 24;
    source = _p.getString('source') ?? 'auto';
  }

  Future<void> save() async {
    await _p.setString('host', host.trim());
    await _p.setInt('port', port);
    await _p.setString('token', token.trim());
    await _p.setString('tileLayer', tileLayer);
    await _p.setBool('tilesOnline', tilesOnline);
    await _p.setBool('vibrate', vibrate);
    await _p.setBool('notify', notify);
    await _p.setBool('alertSound', alertSound);
    await _p.setBool('sndDetect', sndDetect);
    await _p.setBool('sndWatch', sndWatch);
    await _p.setBool('sndDrone', sndDrone);
    await _p.setBool('sndNodeDown', sndNodeDown);
    await _p.setBool('sndNodeUp', sndNodeUp);
    await _p.setBool('proxBeep', proxBeep);
    await _p.setInt('volume', volume);
    await _p.setStringList('muted', muted.toList());
    await _p.setInt('droneHours', droneHours);
    await _p.setString('source', source);
    notifyListeners();
  }
}
