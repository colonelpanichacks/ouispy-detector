// The offline lookup tables the Tab5 reads off its card (home-dashboard/lookupdb.py
// writes assets/lookup.ldb): IEEE OUI vendors, Bluetooth SIG company IDs, 16-bit
// service UUIDs and GAP appearances, so a device's record reads the same with no network.
//
// Format: 16-byte header {"OSLD1\n\0\0", u32 count, u32 names offset}, then count x
// {u8 table, 3 pad, u32 key, u32 name offset, u32 name length} sorted by (table, key),
// then the names. Little-endian.
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/services.dart';

class Lookup {
  Lookup._();
  static final I = Lookup._();
  static const oui = 1, company = 2, uuid16 = 3, appearance = 4;

  ByteData? _b;
  int _n = 0, _names = 0;

  Future<void> load() async {
    try {
      final d = await rootBundle.load('assets/lookup.ldb');
      if (d.lengthInBytes < 16 || utf8.decode(d.buffer.asUint8List(d.offsetInBytes, 5)) != 'OSLD1') return;
      _b = d;
      _n = d.getUint32(8, Endian.little);
      _names = d.getUint32(12, Endian.little);
    } catch (_) {}
  }

  String? _get(int table, int key) {
    final b = _b;
    if (b == null) return null;
    var lo = 0, hi = _n - 1;
    while (lo <= hi) {
      final mid = (lo + hi) >> 1;
      final o = 16 + mid * 16;
      final t = b.getUint8(o), k = b.getUint32(o + 4, Endian.little);
      final c = t != table ? t.compareTo(table) : k.compareTo(key);
      if (c == 0) {
        final off = b.getUint32(o + 8, Endian.little), len = b.getUint32(o + 12, Endian.little);
        return utf8.decode(b.buffer.asUint8List(b.offsetInBytes + _names + off, len), allowMalformed: true);
      }
      if (c < 0) {
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    return null;
  }

  /// The registered maker for a MAC, or null (also null for a random address).
  String? vendor(String mac) {
    final h = mac.replaceAll(RegExp('[^0-9A-Fa-f]'), '');
    if (h.length < 6) return null;
    return _get(oui, int.parse(h.substring(0, 6), radix: 16));
  }

  /// A locally administered (random) address: bit 1 of the first octet.
  static bool randomMac(String mac) {
    final h = mac.replaceAll(RegExp('[^0-9A-Fa-f]'), '');
    return h.length >= 2 && (int.parse(h.substring(0, 2), radix: 16) & 2) != 0;
  }

  String? companyName(int id) => _get(company, id);
  String? uuidName(int id) => _get(uuid16, id);
  String? appearanceName(int v) => _get(appearance, v) ?? _get(appearance, v & ~0x3F);
}
