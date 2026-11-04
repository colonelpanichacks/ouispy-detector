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
  bool alertSound = true;
  Set<String> muted = {};
  int droneHours = 24;

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
    muted = (_p.getStringList('muted') ?? const []).toSet();
    droneHours = _p.getInt('droneHours') ?? 24;
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
    await _p.setStringList('muted', muted.toList());
    await _p.setInt('droneHours', droneHours);
    notifyListeners();
  }
}
