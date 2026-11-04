// Who made a Remote ID drone, instantly and then for sure.
//
// Instant (offline): an ANSI/CTA-2063-A serial number opens with a 4-character
// manufacturer code, then one length character, then the maker's own serial. The code
// table below maps the codes we know to a maker.
//
// For sure (online): the FAA's public UAS DOC registry (uasdoc.faa.gov), the same lookup
// home-dashboard/drones.py faa_lookup() runs: a cookie from /listdocs, then
// /api/v1/serialNumbers?findBy=serialNumber. Only serials and CAA registrations (ID type 1
// or 2) are what the FAA indexes. On the dashboard link the dashboard does it
// (POST /api/drones/<mac>/faa: it caches and broadcasts the record); on a USB node the
// phone asks the FAA itself. Results are kept in memory and in shared_preferences per
// serial; a failure tries again after an hour, and one serial has at most one request out.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

import '../main.dart';
import 'dashboard.dart';
import 'model.dart';

/// CTA-2063-A manufacturer codes known to this app. PARTIAL on purpose: only codes we are
/// certain of are listed, none guessed. Sources:
/// - 1581 = DJI: tab5-dashboard/src/ui.cpp (Drones view maker fallback), and every one of
///   the 2,876 FAA UAS DOC records cached by drone-mesh-mapper (faa_cache.csv) for 1581
///   serials came back makeName "DJI".
/// A code missing here still gets its maker from the FAA lookup.
const ridMakerCodes = <String, String>{'1581': 'DJI'};

/// Remote ID "ID type" (ASTM F3411 basic ID), as home-dashboard/drones.py ID_TYPES.
const ridIdTypes = ['None', 'Serial number (ANSI/CTA-2063-A)', 'CAA registration', 'UTM (USS) UUID', 'Specific session ID'];

/// The ID types the FAA registry indexes (drones.py FAA_ID_TYPES).
const faaIdTypes = {1, 2};

/// The label of an ID type number, or '' when it has none.
String ridIdTypeLabel(Object? idt) {
  final k = ridIdType(idt);
  return k != null && k >= 0 && k < ridIdTypes.length ? ridIdTypes[k] : '';
}

int? ridIdType(Object? idt) => idt is num ? idt.toInt() : int.tryParse('${idt ?? ''}');

/// A CTA-2063-A serial taken apart: [code] (4), [lengthChar] (1), [body] (the rest).
class RidSerial {
  RidSerial._(this.raw, this.code, this.lengthChar, this.body, this.declaredLen);
  final String raw, code, lengthChar, body;

  /// What the length character says the maker's part is (1-15), or null when unreadable.
  final int? declaredLen;

  /// The maker for [code], from [ridMakerCodes], or null when the code is not in the table.
  String? get maker => ridMakerCodes[code];

  /// The length character agrees with the serial's actual length.
  bool get lengthOk => declaredLen != null && declaredLen == body.length;

  /// Parses [s]; null when it is too short to hold a code, a length and one character.
  static RidSerial? parse(String s) {
    final v = s.trim().toUpperCase();
    if (v.length < 6 || !RegExp(r'^[0-9A-Z]+$').hasMatch(v)) return null;
    final lc = v[4];
    return RidSerial._(v, v.substring(0, 4), lc, v.substring(5), lengthOf(lc));
  }

  /// The length character: '1'-'9' = 1-9, 'A'-'F' = 10-15 ('0' is reserved).
  static int? lengthOf(String c) {
    if (c.length != 1) return null;
    final u = c.codeUnitAt(0);
    if (u >= 0x31 && u <= 0x39) return u - 0x30;
    if (u >= 0x41 && u <= 0x46) return u - 0x41 + 10;
    return null;
  }
}

/// Where a maker came from.
class RidSource {
  static const serial = 'serial code';
  static const faa = 'FAA';
  static const vendor = 'radio vendor';
}

/// What [RidLookup.describe] knows about a drone's maker.
class RidInfo {
  const RidInfo({this.maker = '', this.model = '', this.source = '', this.code = '', this.idt = '', this.faaStatus = ''});
  final String maker, model;

  /// [RidSource]: 'serial code', 'FAA' or 'radio vendor'; '' when nothing is known.
  final String source;

  /// The serial's 4-character manufacturer code, when it reads as CTA-2063-A.
  final String code;

  /// The ID type label.
  final String idt;

  /// The FAA DOC record's status (accepted, ...), when it came from the FAA.
  final String faaStatus;
  bool get known => maker.isNotEmpty || model.isNotEmpty;
  String get text => '$maker $model'.trim();
}

enum FaaState { none, pending, found, missing, failed }

class FaaEntry {
  FaaEntry(this.ts, this.ok, this.faa);
  final double ts; // epoch seconds of the attempt
  final bool ok; // the registry answered (found or not)
  final Map<String, dynamic>? faa; // {data: {items: [...]}}, as Drone.maker reads it
  List<Map<String, dynamic>> get items => faaItems(faa);
  Map<String, dynamic> toJson() => {'ts': ts, 'ok': ok, 'faa': ?faa};
  static FaaEntry? fromJson(Object? o) {
    if (o is! Map) return null;
    final ts = o['ts'];
    if (ts is! num) return null;
    final f = o['faa'];
    return FaaEntry(ts.toDouble(), o['ok'] == true, f is Map ? f.cast<String, dynamic>() : null);
  }
}

/// The items of an FAA reply in either shape ({data: {items}} or {items}).
List<Map<String, dynamic>> faaItems(Object? faa) {
  if (faa is! Map) return const [];
  final d = faa['data'];
  final items = (d is Map ? d['items'] : null) ?? faa['items'];
  if (items is! List) return const [];
  return [
    for (final i in items)
      if (i is Map) i.cast<String, dynamic>(),
  ];
}

/// The map follows this drone (a MAC) while it is set; null = not following.
/// The map tab watches it (see the report: wired in map_view.dart / main.dart).
final followDrone = ValueNotifier<String?>(null);

/// One-shot: centre the map on this drone (a MAC). The map sets it back to null once done.
final showDrone = ValueNotifier<String?>(null);

class RidLookup extends ChangeNotifier {
  RidLookup({http.Client? client, Model Function()? model, Future<Map<String, dynamic>?> Function(String mac, String serial)? viaDashboard, double Function()? now})
    : _client = client ?? http.Client(),
      _model = model ?? (() => App.I.m),
      _viaDashboard = viaDashboard,
      _now = now ?? (() => DateTime.now().millisecondsSinceEpoch / 1000);

  static RidLookup I = RidLookup();

  static const prefsKey = 'rid_faa_cache_v1';
  static const settingKey = 'rid_faa_lookup';
  static const retryS = 3600.0; // a failed lookup tries again in an hour (drones.py FAA_RETRY_S)
  static const missingRetryS = 86400.0; // "no record" is asked again a day later
  static const dashboardGraceS = 90.0; // the dashboard looks new drones up itself: give it this long
  static const maxKept = 400;

  final http.Client _client;
  final Model Function() _model;
  final Future<Map<String, dynamic>?> Function(String mac, String serial)? _viaDashboard;
  final double Function() _now;

  final Map<String, FaaEntry> _cache = {};
  final Map<String, Future<FaaEntry?>> _air = {};
  SharedPreferences? _p;
  Future<void>? _loading;
  bool _faaLookup = true;

  /// Look serials up online on their own (the setting, default on).
  bool get faaLookup => _faaLookup;
  Future<void> setFaaLookup(bool v) async {
    await load();
    _faaLookup = v;
    await _p?.setBool(settingKey, v);
    notifyListeners();
  }

  /// Reads the setting and the cache; safe to call more than once.
  Future<void> load() => _loading ??= _load();
  Future<void> _load() async {
    try {
      _p = await SharedPreferences.getInstance();
      _faaLookup = _p!.getBool(settingKey) ?? true;
      final s = _p!.getString(prefsKey);
      if (s != null) {
        final j = jsonDecode(s);
        if (j is Map) {
          for (final e in j.entries) {
            final f = FaaEntry.fromJson(e.value);
            if (f != null) _cache.putIfAbsent('${e.key}', () => f);
          }
        }
      }
    } catch (_) {
      // no prefs (or a bad blob): start empty
    }
  }

  Future<void> _save() async {
    final p = _p;
    if (p == null) return;
    if (_cache.length > maxKept) {
      final old = _cache.entries.toList()..sort((a, b) => a.value.ts.compareTo(b.value.ts));
      for (final e in old.take(_cache.length - maxKept)) {
        _cache.remove(e.key);
      }
    }
    await p.setString(prefsKey, jsonEncode({for (final e in _cache.entries) e.key: e.value.toJson()}));
  }

  static String _key(String serial) => serial.trim().toUpperCase();

  FaaEntry? cached(String serial) => _cache[_key(serial)];
  bool pending(String serial) => _air.containsKey(_key(serial));

  /// Where the FAA lookup of [d] stands.
  FaaState state(Drone d) {
    final s = _key(d.serial);
    if (s.isEmpty) return FaaState.none;
    if (_air.containsKey(s)) return FaaState.pending;
    if (faaItems(d.raw['faa']).isNotEmpty) return FaaState.found;
    final c = _cache[s];
    if (c == null) return d.raw['faa'] is Map ? FaaState.missing : FaaState.none;
    if (!c.ok) return FaaState.failed;
    return c.items.isEmpty ? FaaState.missing : FaaState.found;
  }

  /// Maker and model, best source first: the FAA record, the serial's maker code, the
  /// radio's MAC vendor. Instant: reads only what is here already.
  RidInfo describe(Drone d) {
    final serial = d.serial.trim();
    final idt = ridIdType(d.raw['idt']);
    final idtLabel = ridIdTypeLabel(idt).isNotEmpty ? ridIdTypeLabel(idt) : '${d.raw['id_type'] ?? ''}';
    // a CAA registration, a UUID or a session ID is not a CTA-2063 serial
    final ser = idt == null || idt == 1 ? RidSerial.parse(serial) : null;
    final code = ser?.code ?? '';
    var items = faaItems(d.raw['faa']);
    if (items.isEmpty && serial.isNotEmpty) items = _cache[_key(serial)]?.items ?? const [];
    if (items.isNotEmpty) {
      final i = items.first;
      return RidInfo(
        maker: '${i['makeName'] ?? ''}'.trim(),
        model: '${i['modelName'] ?? ''}'.trim(),
        source: RidSource.faa,
        code: code,
        idt: idtLabel,
        faaStatus: '${i['status'] ?? ''}',
      );
    }
    final mk = ser?.maker;
    if (mk != null) return RidInfo(maker: mk, source: RidSource.serial, code: code, idt: idtLabel);
    if (d.vendor.isNotEmpty) return RidInfo(maker: d.vendor, source: RidSource.vendor, code: code, idt: idtLabel);
    return RidInfo(code: code, idt: idtLabel);
  }

  /// Kicks the FAA lookup for [d] when it is due (setting on, ID type 1/2, not cached or
  /// the retry is up, none in flight). A cached answer lands on the drone at once. Cheap:
  /// call it from build. Listeners hear when a lookup lands.
  void ensure(Drone d) {
    final serial = _key(d.serial);
    if (serial.isEmpty || d.raw['faa'] is Map) return;
    final c = _cache[serial];
    if (c != null && c.ok) {
      _merge(d.mac, c);
      return;
    }
    if (!_faaLookup || !faaIdTypes.contains(ridIdType(d.raw['idt']))) return;
    if (c != null && _now() - c.ts < retryS) return;
    if (_air.containsKey(serial)) return;
    // the dashboard looks a new drone up on its own and broadcasts the record
    if (_onDashboard() && d.first > 0 && _model().now - d.first < dashboardGraceS) return;
    lookup(d);
  }

  /// Looks [d]'s serial up now (the FAA lookup button): skips the retry wait, not the
  /// one-in-flight rule. Resolves to the entry, or null with no serial.
  Future<FaaEntry?> lookup(Drone d, {bool force = false}) {
    final serial = _key(d.serial);
    if (serial.isEmpty) return Future.value(null);
    final air = _air[serial];
    if (air != null) return air;
    if (!force) {
      final c = _cache[serial];
      if (c != null && c.ok && (c.items.isNotEmpty || _now() - c.ts < missingRetryS)) {
        _merge(d.mac, c);
        return Future.value(c);
      }
    }
    final f = _run(d.mac, serial);
    _air[serial] = f;
    scheduleMicrotask(notifyListeners);
    return f;
  }

  Future<FaaEntry?> _run(String mac, String serial) async {
    FaaEntry e;
    try {
      await load();
      Map<String, dynamic>? faa;
      try {
        faa = await (_viaDashboard ?? _dashboardLookup)(mac, serial);
      } catch (_) {
        faa = null;
      }
      faa ??= await faaQuery(serial);
      e = FaaEntry(_now(), true, _trim(faa));
    } catch (_) {
      e = FaaEntry(_now(), false, null);
    }
    _cache[serial] = e;
    _air.remove(serial);
    if (e.ok) _merge(mac, e);
    try {
      await _save();
    } catch (_) {}
    notifyListeners();
    return e;
  }

  void _merge(String mac, FaaEntry e) {
    final m = _model();
    if (!m.drones.containsKey(mac.toUpperCase())) return;
    m.ingestDrone({
      'mac': mac,
      'faa':
          e.faa ??
          {
            'data': {'items': <Object>[]},
          },
    });
  }

  /// The record kept: the items' plain fields, as {data: {items}}.
  static Map<String, dynamic> _trim(Map<String, dynamic> faa) => {
    'data': {
      'items': [
        for (final i in faaItems(faa).take(4))
          {
            for (final e in i.entries)
              if (e.value is String || e.value is num || e.value is bool) e.key: e.value,
          },
      ],
    },
  };

  bool _onDashboard() {
    try {
      return App.I.link is Dashboard;
    } catch (_) {
      return false;
    }
  }

  Future<Map<String, dynamic>?> _dashboardLookup(String mac, String serial) async {
    final Dashboard dash;
    try {
      if (App.I.link is! Dashboard) return null;
      dash = App.I.dash;
    } catch (_) {
      return null;
    }
    if (dash.base.isEmpty) return null;
    // the same POST as App.I.dash.post, without its toasts: a quiet background lookup
    final r = await _client
        .post(
          dash.uri('/api/drones/${Uri.encodeComponent(mac)}/faa'),
          headers: {...dash.authHeaders, 'Content-Type': 'application/json'},
          body: jsonEncode({'serial': serial}),
        )
        .timeout(const Duration(seconds: 40));
    if (r.statusCode != 200) return null;
    final j = jsonDecode(utf8.decode(r.bodyBytes));
    if (j is! Map || j['ok'] != true || j['faa'] is! Map) return null;
    return (j['faa'] as Map).cast<String, dynamic>();
  }

  static const _hdr = {
    'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:137.0) Gecko/20100101 Firefox/137.0',
    'Accept': 'application/json, text/plain, */*',
    'Referer': 'https://uasdoc.faa.gov/listdocs',
    'client': 'external',
  };

  /// The FAA UAS DOC serial lookup straight from the phone (drones.py faa_lookup()).
  /// Throws on no answer; an answer with no record has empty items.
  Future<Map<String, dynamic>> faaQuery(String serial) async {
    var cookie = '';
    try {
      final r = await _client.get(Uri.https('uasdoc.faa.gov', '/listdocs'), headers: _hdr).timeout(const Duration(seconds: 20));
      cookie = cookieHeader(r.headers['set-cookie']);
    } catch (_) {
      // drones.py carries on without it too
    }
    final u = Uri.https('uasdoc.faa.gov', '/api/v1/serialNumbers', {
      'itemsPerPage': '8',
      'pageIndex': '0',
      'orderBy[0]': 'updatedAt',
      'orderBy[1]': 'DESC',
      'findBy': 'serialNumber',
      'serialNumber': serial,
    });
    final r = await _client.get(u, headers: {..._hdr, if (cookie.isNotEmpty) 'Cookie': cookie}).timeout(const Duration(seconds: 30));
    if (r.statusCode != 200) throw http.ClientException('FAA HTTP ${r.statusCode}', u);
    final j = jsonDecode(utf8.decode(r.bodyBytes));
    if (j is! Map) throw http.ClientException('FAA reply not an object', u);
    return j.cast<String, dynamic>();
  }

  /// A Cookie header from a (comma-joined, as package:http gives it) Set-Cookie header.
  static String cookieHeader(String? setCookie) {
    if (setCookie == null || setCookie.isEmpty) return '';
    return setCookie.split(RegExp(r',(?=\s*[^;,=\s]+=)')).map((c) => c.split(';').first.trim()).where((c) => c.contains('=')).join('; ');
  }

  /// Forget everything (tests, "clear data").
  @visibleForTesting
  void reset() {
    _cache.clear();
    _air.clear();
    _loading = null;
    _p = null;
    _faaLookup = true;
  }
}
