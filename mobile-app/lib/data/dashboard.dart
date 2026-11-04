// The home dashboard client: the same endpoints, events and token as the Tab5
// (tab5-dashboard/src/net.cpp). REST for state and on-demand records, Socket.IO for the
// live feed, the access token as the `ouispy_token` cookie, the dashboard found on the
// LAN as _ouispy._tcp or typed in (a Tailscale name or IP).
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;
import 'package:nsd/nsd.dart' as nsd;
import 'package:socket_io_client/socket_io_client.dart' as io;

import 'model.dart';
import 'settings.dart';

/// Anything that feeds the model. The dashboard is the only one in phase 1; a node on USB
/// serial or BLE (docs/PHASE2.md) implements the same thing.
abstract class FeedSource {
  Future<void> start();
  Future<void> stop();
  String get label;
}

class Discovered {
  Discovered(this.name, this.host, this.port);
  final String name, host;
  final int port;
}

/// Looks for dashboards announcing `_ouispy._tcp` for [wait].
Future<List<Discovered>> discoverDashboards({Duration wait = const Duration(seconds: 4)}) async {
  final out = <Discovered>[];
  nsd.Discovery? d;
  try {
    d = await nsd.startDiscovery('_ouispy._tcp', ipLookupType: nsd.IpLookupType.v4);
    await Future.delayed(wait);
    for (final s in d.services) {
      final ip = s.addresses?.where((a) => a.type == InternetAddressType.IPv4).map((a) => a.address).firstOrNull ?? s.host;
      if (ip == null || s.port == null) continue;
      if (out.any((o) => o.host == ip && o.port == s.port)) continue;
      out.add(Discovered(s.name ?? 'OUI-SPY Mesh', ip, s.port!));
    }
  } catch (_) {
    // no multicast here (emulator, VPN-only): the host field is the way in
  } finally {
    if (d != null) {
      try {
        await nsd.stopDiscovery(d);
      } catch (_) {}
    }
  }
  return out;
}

class Dashboard implements FeedSource {
  Dashboard(this.m, this.s);
  final Model m;
  final Settings s;

  io.Socket? _sock;
  String _host = '';
  int _port = 5077;
  Timer? _poll;
  bool _running = false;
  DateTime _lastState = DateTime(0), _lastStats = DateTime(0), _lastDrones = DateTime(0);
  final _client = http.Client();

  @override
  String get label => 'dashboard';
  String get base => _host.isEmpty ? '' : 'http://$_host:$_port';
  Map<String, String> get _headers => {
        'User-Agent': 'OUI-SPY-Mesh-Mobile',
        if (s.token.isNotEmpty) 'Cookie': 'ouispy_token=${s.token}',
      };
  Map<String, String> get authHeaders => _headers;

  Future<bool> _resolve() async {
    var h = s.host.trim();
    _port = s.port;
    if (h.isEmpty || h == 'auto') {
      m.lastError = 'looking for OUI-SPY Mesh on this network';
      m.changed();
      final found = await discoverDashboards();
      if (found.isEmpty) {
        m.lastError = 'no OUI-SPY Mesh found on this network (set a host in Settings)';
        return false;
      }
      _host = found.first.host;
      _port = found.first.port;
      return true;
    }
    // "host:port" typed in one field
    final c = h.lastIndexOf(':');
    if (c > 0 && !h.contains(']') && int.tryParse(h.substring(c + 1)) != null) {
      _port = int.parse(h.substring(c + 1));
      h = h.substring(0, c);
    }
    _host = h.replaceFirst(RegExp(r'^https?://'), '');
    return true;
  }

  @override
  Future<void> start() async {
    await stop();
    _running = true;
    m.link = Link.connecting;
    m.changed();
    if (!await _resolve()) {
      m.link = Link.off;
      m.changed();
      // try discovery again in a while
      _poll = Timer(const Duration(seconds: 10), () {
        if (_running) start();
      });
      return;
    }
    m.target = '$_host:$_port';
    _connectSocket();
    _poll = Timer.periodic(const Duration(seconds: 1), (_) => _tick());
  }

  void _connectSocket() {
    final opts = io.OptionBuilder()
        .setTransports(['websocket'])
        .setExtraHeaders(_headers)
        .disableAutoConnect()
        .enableReconnection()
        .setReconnectionDelay(3000)
        .enableForceNew()
        .build();
    final sock = io.io(base, opts);
    sock.onConnect((_) {
      m.link = Link.up;
      m.lastError = '';
      m.changed();
      fetchState(); // resync: the feed may have moved while we were away
    });
    sock.onDisconnect((_) {
      if (m.link == Link.up) m.link = Link.connecting;
      m.changed();
    });
    sock.onConnectError((e) {
      m.link = Link.refused;
      m.lastError = 'no socket to $base (${_short(e)})';
      m.changed();
    });
    void on(String ev, void Function(Map<String, dynamic>) f) => sock.on(ev, (d) {
          if (d is Map) {
            try {
              f(d.cast<String, dynamic>());
            } catch (_) {}
          }
        });
    on('fleet', m.ingestFleet);
    on('det', m.ingestDet);
    on('sighting', m.ingestSighting);
    on('dets_cleared', m.ingestCleared);
    on('drone', m.ingestDrone);
    on('tri', m.ingestTri);
    on('mac_aliases', m.ingestAliases);
    on('groups', m.ingestGroups);
    on('marks', m.ingestMarks);
    on('node_event', (b) => m.toast('${m.nodeName('${b['id']}')} ${b['event']}'));
    on('node_filters', (b) => m.toast(b['filters_ok'] == true ? '${m.nodeName('${b['id']}')} filters updated' : '${m.nodeName('${b['id']}')} filter push failed'));
    on('lost_node', (b) => m.toast('${m.nodeName('${b['node']}')} reported lost'));
    on('job', (b) {
      final st = '${b['state']}';
      if (st == 'done' || st == 'error' || st == 'failed') m.toast('${b['label'] ?? b['op']}: $st');
    });
    _sock = sock;
    sock.connect();
  }

  String _short(Object? e) {
    final t = '$e';
    return t.length > 60 ? t.substring(0, 60) : t;
  }

  void _tick() {
    final now = DateTime.now();
    if (m.link != Link.up) return;
    if (now.difference(_lastState).inMinutes >= 10) fetchState();
    if (now.difference(_lastStats).inSeconds >= 30) {
      _lastStats = now;
      fetchStats();
    }
    if (now.difference(_lastDrones).inSeconds >= 60) fetchDroneWindow();
    if (now.second % 5 == 0) m.pruneTargets();
  }

  @override
  Future<void> stop() async {
    _running = false;
    _poll?.cancel();
    _poll = null;
    _sock?.dispose();
    _sock = null;
    m.link = Link.off;
  }

  // ------------------------------------------------------------ REST

  Uri uri(String path) => Uri.parse('$base$path');

  Future<dynamic> getJson(String path, {Duration timeout = const Duration(seconds: 12)}) async {
    if (base.isEmpty) return null;
    try {
      final r = await _client.get(uri(path), headers: _headers).timeout(timeout);
      if (r.statusCode == 401) {
        m.lastError = 'the dashboard wants the access token (Settings)';
        m.link = Link.refused;
        m.changed();
        return null;
      }
      if (r.statusCode != 200) {
        m.lastError = 'HTTP ${r.statusCode} on $path';
        m.changed();
        return null;
      }
      return jsonDecode(utf8.decode(r.bodyBytes));
    } catch (e) {
      m.lastError = 'no reply on $path';
      m.changed();
      return null;
    }
  }

  Future<Map<String, dynamic>?> post(String path, Map<String, dynamic> body, {String? okToast}) async {
    if (base.isEmpty) return null;
    try {
      final r = await _client
          .post(uri(path), headers: {..._headers, 'Content-Type': 'application/json'}, body: jsonEncode(body))
          .timeout(const Duration(seconds: 10));
      if (r.statusCode != 200) {
        m.toast("dashboard didn't take it (${r.statusCode})");
        return null;
      }
      if (okToast != null) m.toast(okToast);
      final j = jsonDecode(utf8.decode(r.bodyBytes));
      return j is Map ? j.cast<String, dynamic>() : {};
    } catch (_) {
      m.toast("dashboard didn't take it");
      return null;
    }
  }

  Future<void> fetchState() async {
    _lastState = DateTime.now();
    // The Tab5's trimmed state: everything the views draw, a quarter of the full one.
    final j = await getJson('/api/state?for=tab5', timeout: const Duration(seconds: 20));
    if (j is Map) m.ingestState(j.cast<String, dynamic>());
    fetchDroneWindow();
    fetchStats();
  }

  Future<void> fetchStats() async {
    final j = await getJson('/api/stats');
    if (j is Map) {
      m.stats = j.cast<String, dynamic>();
      if (j['hourly'] is List) m.hourly = [for (final h in j['hourly']) (h as Map).cast<String, dynamic>()];
      m.changed();
    }
  }

  /// Every drone heard in the window, with its recent track (the drones tab and the map).
  Future<void> fetchDroneWindow() async {
    _lastDrones = DateTime.now();
    final j = await getJson('/api/drones?${m.droneWindow.query}&track=0&rtrack=64&limit=60');
    if (j is! Map) return;
    if (j['tags'] is List) m.droneTags = [for (final t in j['tags']) '$t'];
    for (final l in ['live', 'recent']) {
      if (j[l] is List) {
        for (final d in j[l]) {
          m.ingestDrone((d as Map).cast<String, dynamic>(), notify: false);
        }
      }
    }
    m.changed();
  }

  Future<List<Flight>> flights(String mac) async {
    final j = await getJson('/api/drones/$mac/flights?compact=1&limit=10&pts=96');
    if (j is! Map || j['flights'] is! List) return [];
    return [for (final f in j['flights']) Flight((f as Map).cast<String, dynamic>())];
  }

  Future<Map<String, dynamic>?> device(String mac) async {
    final j = await getJson('/api/devices/$mac?flat=1');
    return j is Map ? j.cast<String, dynamic>() : null;
  }

  Future<Map<String, dynamic>?> history({String q = '', String dom = '', int limit = 300}) async {
    final p = StringBuffer('/api/devices?src=det&limit=$limit');
    if (q.isNotEmpty) p.write('&q=${Uri.encodeQueryComponent(q)}');
    if (dom.isNotEmpty) p.write('&dom=$dom');
    final j = await getJson(p.toString());
    return j is Map ? j.cast<String, dynamic>() : null;
  }

  Future<Map<String, String>> tileLayers() async {
    final j = await getJson('/api/tiles');
    if (j is! Map) return {};
    return {for (final e in j.entries) '${e.key}': '${(e.value as Map)['name']}'};
  }

  // ---- writes (the same bodies the Tab5 posts)
  Future<void> setAlias(String mac, String alias) => post('/api/devices/$mac', {'alias': alias}, okToast: alias.isEmpty ? 'name cleared' : 'named $alias');
  Future<void> setNotes(String mac, String notes) => post('/api/devices/$mac', {'notes': notes}, okToast: 'notes saved');
  Future<void> groupAdd(int id, String mac) => post('/api/groups', {'id': id, 'add': [mac]}, okToast: 'added to group');
  Future<void> groupRemove(int id, String mac) => post('/api/groups', {'id': id, 'remove': [mac]}, okToast: 'taken out of group');
  Future<void> groupNew(String name, String mac) =>
      post('/api/groups', {'name': name, 'color': DateTime.now().millisecond % 360, 'add': [mac]}, okToast: 'group $name made');
  Future<void> droneMeta(String mac, Map<String, dynamic> body) => post('/api/drones/$mac', body, okToast: 'saved on the dashboard');
  Future<void> nodeSet(String id, Map<String, dynamic> body) => post('/api/nodes/$id', body, okToast: 'saved on the dashboard');
  Future<Map<String, dynamic>?> nodeCmd(String id, String op, [Map<String, dynamic>? args]) =>
      post('/api/nodes/$id/cmd', {'op': op, 'args': ?args}, okToast: '$op sent to ${m.nodeName(id)}');
  Future<void> addMark(Map<String, dynamic> mark) => post('/api/marks', mark, okToast: 'mark saved');
}
