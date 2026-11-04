// The node page's records beyond the node itself (app.js node drawer): lost / found, forget,
// and carrying a node with this phone. Each goes to whichever backend the app is on: the
// dashboard's REST (/api/nodes/<id>/lost, DELETE /api/nodes/<id>, /api/nodes/<id>/gps) or,
// on a node's USB, the phone's own store plus the console (`rdeny all <ids>`).
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:geolocator/geolocator.dart';

import '../main.dart';
import 'dashboard.dart';
import 'firmware.dart';
import 'local_mesh.dart';

/// Report [id] lost (every node drops it and reports it if it shows up again) or found.
Future<String?> reportLost(String id, bool lost, {String note = ''}) async {
  final l = App.I.link;
  if (l is LocalMesh) {
    final r = await l.setLost(id, lost, note: note);
    return r == null ? 'the lost list is kept, but the fleet did not take it yet' : null;
  }
  if (l is Dashboard) {
    final r = await dashReq(l, 'POST', '/api/nodes/$id/lost', json: {'lost': lost, 'note': note});
    if (r.$1 != 200) return dashError(r, 'lost');
    App.I.m.toast(lost ? 'reported lost' : 'cleared');
    l.fetchState();
    return null;
  }
  return 'no backend';
}

/// Forget an offline node: off the fleet list and the map.
Future<String?> forgetNode(String id) async {
  final l = App.I.link;
  if (l is LocalMesh) return l.forget(id) ? null : 'it is online: only an offline node can be forgotten';
  if (l is Dashboard) {
    final r = await dashReq(l, 'DELETE', '/api/nodes/$id');
    if (r.$1 != 200) return dashError(r, 'forget');
    App.I.m.toast('forgotten');
    return null;
  }
  return 'no backend';
}

/// This phone's GPS as a node's position: a fix every 2 s, or sooner once it moved 3 m.
class Carry extends ChangeNotifier {
  Carry._();
  static final I = Carry._();

  String? node;
  String info = '';
  double? acc;
  StreamSubscription<Position>? _sub;
  DateTime _sent = DateTime(0);
  Position? _last;

  bool carrying(String id) => node == id;

  Future<void> start(String id) async {
    if (node != null) await stop();
    try {
      if (!await Geolocator.isLocationServiceEnabled()) {
        info = 'location is off on this phone';
        notifyListeners();
        return;
      }
      var p = await Geolocator.checkPermission();
      if (p == LocationPermission.denied) p = await Geolocator.requestPermission();
      if (p == LocationPermission.denied || p == LocationPermission.deniedForever) {
        info = 'location permission denied';
        notifyListeners();
        return;
      }
    } catch (e) {
      info = 'no GPS ($e)';
      notifyListeners();
      return;
    }
    node = id;
    info = 'waiting for a GPS fix';
    _sent = DateTime(0);
    _last = null;
    _sub = Geolocator.getPositionStream(locationSettings: const LocationSettings(accuracy: LocationAccuracy.best, distanceFilter: 0)).listen(
      _fix,
      onError: (_) {
        info = 'no GPS fix yet';
        notifyListeners();
      },
    );
    App.I.m.toast("carrying ${App.I.m.nodeName(id)}: this phone's GPS positions it");
    notifyListeners();
  }

  Future<void> _fix(Position p) async {
    final id = node;
    if (id == null) return;
    acc = p.accuracy;
    final moved = _last == null ? 1e9 : Geolocator.distanceBetween(_last!.latitude, _last!.longitude, p.latitude, p.longitude);
    if (DateTime.now().difference(_sent).inMilliseconds >= 2000 || moved >= 3) {
      _sent = DateTime.now();
      _last = p;
      final l = App.I.link;
      if (l is LocalMesh) {
        l.place(id, p.latitude, p.longitude);
      } else if (l is Dashboard) {
        await dashReq(l, 'POST', '/api/nodes/$id/gps', json: {'lat': p.latitude, 'lon': p.longitude, 'acc': p.accuracy}, timeout: const Duration(seconds: 6));
      }
    }
    info = 'GPS live, accuracy ${p.accuracy.round()} m';
    notifyListeners();
  }

  Future<void> stop() async {
    final id = node;
    await _sub?.cancel();
    _sub = null;
    node = null;
    info = '';
    acc = null;
    notifyListeners();
    final l = App.I.link;
    if (id != null && l is Dashboard) await dashReq(l, 'DELETE', '/api/nodes/$id/gps');
  }
}
