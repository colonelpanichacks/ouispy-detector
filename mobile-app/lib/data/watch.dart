// The Detections area: what to watch for. Rules are the nodes' own filter types (OUI,
// MAC, NAME, CID, SVC, PROBE, BEACON; home-dashboard/filters.py) plus VENDOR, a
// phone-only match on the registered maker. The phone alerts on every match (model.dart
// watcher), and the same rules install on OUI-SPY nodes as their filter list, so the
// nodes beep and flash for them too.
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'lookup.dart';
import 'model.dart';

const filterTypes = ['OUI', 'MAC', 'NAME', 'CID', 'SVC', 'PROBE', 'BEACON', 'META'];
const typeCode = {'OUI': 0, 'MAC': 1, 'CID': 2, 'SVC': 3, 'NAME': 4, 'META': 5, 'PROBE': 6, 'BEACON': 7};
const hexLen = {'OUI': 6, 'PROBE': 6, 'BEACON': 6, 'MAC': 12, 'CID': 4, 'SVC': 4};
const typeLabel = {
  'OUI': 'BLE maker prefix',
  'MAC': 'Exact MAC',
  'NAME': 'BLE name contains',
  'CID': 'BLE company ID',
  'SVC': 'BLE service UUID',
  'PROBE': 'Wi-Fi probe prefix',
  'BEACON': 'Wi-Fi beacon prefix',
  'META': 'Meta composite',
  'VENDOR': 'Maker name contains',
};

class Rule {
  Rule(this.t, this.id, this.d, {this.group = ''});
  final String t, id, d;
  final String group; // the library vendor it came from, if any

  Map<String, dynamic> toJson() => {'t': t, 'id': id, 'd': d, if (group.isNotEmpty) 'g': group};
  static Rule from(Map o) => Rule('${o['t']}', '${o['id']}', '${o['d'] ?? ''}', group: '${o['g'] ?? ''}');

  /// The node can hold it (VENDOR is the phone's own).
  bool get forNodes => t != 'VENDOR';

  /// {t, id, d} as the firmware and the dashboard take it.
  Map<String, dynamic> get filter => {'t': t, 'id': id, 'd': d};

  /// Cleans a typed rule; null with a reason when it can't be one.
  static (Rule?, String?) parse(String t, String id, String d) {
    t = t.toUpperCase();
    var v = id.trim();
    if (hexLen.containsKey(t)) {
      v = v.replaceAll(RegExp(r'[:\-\s]'), '').toUpperCase();
      if ((t == 'CID' || t == 'SVC') && v.startsWith('0X')) v = v.substring(2);
      if (v.length != hexLen[t] || !RegExp(r'^[0-9A-F]+$').hasMatch(v)) return (null, '${typeLabel[t]} needs ${hexLen[t]} hex digits');
    } else if (v.isEmpty || v.length > (t == 'VENDOR' ? 48 : 16) || v.codeUnits.any((c) => c < 0x20 || c > 0x7E)) {
      return (null, '${typeLabel[t]} needs 1-${t == 'VENDOR' ? 48 : 16} plain characters');
    }
    final desc = String.fromCharCodes(d.codeUnits.where((c) => c >= 0x20 && c <= 0x7E));
    return (Rule(t, v, desc.length > 48 ? desc.substring(0, 48) : desc), null);
  }
}

class Vendor {
  Vendor(this.key, this.label, this.rows, this.meta);
  final String key, label;
  final List<Rule> rows;
  final Map<String, dynamic> meta;
}

class Watch extends ChangeNotifier {
  Watch._();
  static final I = Watch._();

  List<Rule> rules = [];
  List<Vendor> library = [];
  List<String> presets = [];
  SharedPreferences? _p;

  Future<void> load() async {
    _p = await SharedPreferences.getInstance();
    try {
      rules = [for (final r in jsonDecode(_p!.getString('watch') ?? '[]') as List) Rule.from(r as Map)];
    } catch (_) {}
    try {
      final j = jsonDecode(await rootBundle.loadString('assets/library.json')) as Map;
      presets = [for (final p in j['presets'] as List) '$p'];
      library = [
        for (final g in j['library'] as List)
          Vendor('${g['key']}', '${g['label']}', [for (final r in g['rows'] as List) Rule.from(r as Map)], ((g['meta'] as Map?) ?? const {}).cast<String, dynamic>()),
      ];
    } catch (_) {}
  }

  Future<void> _save() async {
    await _p?.setString('watch', jsonEncode([for (final r in rules) r.toJson()]));
    notifyListeners();
  }

  bool has(Rule r) => rules.any((x) => x.t == r.t && x.id.toUpperCase() == r.id.toUpperCase());

  Future<void> add(Iterable<Rule> rs) async {
    for (final r in rs) {
      if (!has(r)) rules.add(r);
    }
    await _save();
  }

  Future<void> remove(Iterable<Rule> rs) async {
    for (final r in rs) {
      rules.removeWhere((x) => x.t == r.t && x.id.toUpperCase() == r.id.toUpperCase());
    }
    await _save();
  }

  /// How much of a vendor is on the list: 0 none, 1 some, 2 all.
  int vendorState(Vendor v) {
    final n = v.rows.where(has).length;
    return n == 0 ? 0 : (n == v.rows.length ? 2 : 1);
  }

  /// The label of the first rule a detection matches, or null.
  String? match(Det d, Target? t) {
    if (rules.isEmpty) return null;
    final hex = d.mac.replaceAll(':', '').toUpperCase();
    final name = '${t?.name ?? ''} ${d.name} ${d.descr}'.toLowerCase();
    final info = t?.info ?? const {};
    String? vendor;
    for (final r in rules) {
      final id = r.id.toUpperCase();
      final hit = switch (r.t) {
        'MAC' => hex == id,
        'OUI' || 'PROBE' || 'BEACON' => hex.startsWith(id),
        'NAME' => name.contains(r.id.toLowerCase()),
        'VENDOR' => (vendor ??= (Lookup.I.vendor(d.mac) ?? '').toLowerCase()).contains(r.id.toLowerCase()),
        'CID' => '${info['cid'] ?? ''}'.toUpperCase().replaceFirst('0X', '').padLeft(4, '0') == id,
        'SVC' => [for (final u in (info['uuids'] as List? ?? const [])) '$u'.toUpperCase().replaceFirst('0X', '')].contains(id),
        _ => false,
      };
      if (hit) return r.d.isNotEmpty ? r.d : '${typeLabel[r.t]} ${r.id}';
    }
    return null;
  }
}

/// The firmware's filter record: u8 type, u8 flags (bit0 full MAC), u8 idLen, u8 descLen, id, desc.
Uint8List encodeFilters(List<Map<String, dynamic>> fs) {
  final b = BytesBuilder();
  final seen = <String>{};
  for (final f in fs) {
    final t = '${f['t']}'.toUpperCase();
    final id = '${f['id']}';
    if (!typeCode.containsKey(t) || !seen.add('$t|${id.toUpperCase()}')) continue;
    final ib = ascii.encode(id), db = ascii.encode('${f['d'] ?? ''}'.length > 48 ? '${f['d']}'.substring(0, 48) : '${f['d'] ?? ''}');
    b.add([typeCode[t]!, t == 'MAC' ? 1 : 0, ib.length, db.length, ...ib, ...db]);
  }
  return b.toBytes();
}
