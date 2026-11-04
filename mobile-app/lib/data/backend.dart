// What the views ask of wherever the mesh comes from. Two implement it: the home
// dashboard (dashboard.dart, the same REST + socket the Tab5 uses) and the app being the
// dashboard itself, straight off a node on USB or BLE (local_mesh.dart).
import 'dart:async';

import 'model.dart';

/// One console line: when, where from, what.
class ConsoleLine {
  ConsoleLine(this.ts, this.src, this.line);
  final double ts;
  final String src, line;
}

/// The last lines of the console, and the live ones after them.
class Console {
  final ring = <ConsoleLine>[];
  final _ctl = StreamController<ConsoleLine>.broadcast();
  Stream<ConsoleLine> get stream => _ctl.stream;
  void add(String line, {String src = 'serial', double? ts}) {
    final c = ConsoleLine(ts ?? DateTime.now().millisecondsSinceEpoch / 1000, src, line);
    ring.add(c);
    if (ring.length > 600) ring.removeRange(0, ring.length - 600);
    _ctl.add(c);
  }
}

abstract class Backend {
  String get label;
  Future<void> start();
  Future<void> stop();

  Future<void> fetchState();
  Future<void> fetchStats();
  Future<void> fetchDroneWindow();
  Future<List<Flight>> flights(String mac);

  /// A device's record: {rows: [[label, value]], companions, groups, group, alias, notes, name}.
  Future<Map<String, dynamic>?> device(String mac);

  /// Everything heard: {rows: [{mac, dom, type, name, descr, last, hits, best, nodes}], total}.
  Future<Map<String, dynamic>?> history({String q = '', String dom = '', int limit = 300});

  Future<void> setAlias(String mac, String alias);
  Future<void> setNotes(String mac, String notes);
  Future<void> groupAdd(int id, String mac);
  Future<void> groupRemove(int id, String mac);
  Future<void> groupNew(String name, String mac);
  Future<void> droneMeta(String mac, Map<String, dynamic> body);
  Future<void> nodeSet(String id, Map<String, dynamic> body);

  /// The dashboard's node ops (audio, identify, status, reboot, locate, preset, alias,
  /// wardrive, filters_get, rechan, ...): one node by id, or `all`.
  Future<Map<String, dynamic>?> nodeCmd(String id, String op, [Map<String, dynamic>? args]);
  Future<void> addMark(Map<String, dynamic> mark);

  /// The node's console (USB: the board itself; the dashboard: its serial console).
  Console get console;

  /// Sends one console line; false when nothing is listening.
  Future<bool> sendLine(String line);
}
