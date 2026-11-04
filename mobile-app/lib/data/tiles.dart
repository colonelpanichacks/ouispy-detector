// Map tiles from the dashboard's own tile endpoint (/tiles/<layer>/<z>/<x>/<y>, which
// proxies and caches them on the Mac), kept on the phone so the map works with no link.
// The phone never talks to a tile server itself.
import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';

class TileCache {
  TileCache._();
  static final TileCache I = TileCache._();
  Directory? _dir;
  final _client = http.Client();

  Future<Directory> dir() async {
    if (_dir != null) return _dir!;
    final base = await getApplicationSupportDirectory();
    _dir = Directory('${base.path}/tiles');
    await _dir!.create(recursive: true);
    return _dir!;
  }

  Future<File> file(String layer, int z, int x, int y) async => File('${(await dir()).path}/$layer/$z/$x/$y.img');

  /// The cached tile, else (when allowed) one from the dashboard, stored for later.
  Future<Uint8List?> get(String base, Map<String, String> headers, String layer, int z, int x, int y, {required bool online}) async {
    final f = await file(layer, z, x, y);
    if (await f.exists()) return f.readAsBytes();
    if (!online || base.isEmpty) return null;
    try {
      final r = await _client.get(Uri.parse('$base/tiles/$layer/$z/$x/$y'), headers: headers).timeout(const Duration(seconds: 15));
      // The dashboard refuses Esri's blank placeholders itself; anything tiny is one too.
      if (r.statusCode != 200 || r.bodyBytes.length < 200) return null;
      await f.parent.create(recursive: true);
      await f.writeAsBytes(r.bodyBytes, flush: false);
      return r.bodyBytes;
    } catch (_) {
      return null;
    }
  }

  Future<(int files, int bytes)> usage() async {
    var n = 0, b = 0;
    try {
      await for (final e in (await dir()).list(recursive: true)) {
        if (e is File) {
          n++;
          b += await e.length();
        }
      }
    } catch (_) {}
    return (n, b);
  }

  Future<void> clear() async {
    final d = await dir();
    if (await d.exists()) await d.delete(recursive: true);
    _dir = null;
  }

  /// Stores every tile of [bounds] from [zMin] to [zMax] (capped at [cap] tiles), for a
  /// map that works away from home. [progress] gets (done, total).
  Future<int> prefetch(String base, Map<String, String> headers, String layer, LatLngBoundsLike bounds, int zMin, int zMax,
      {int cap = 4000, void Function(int, int)? progress, bool Function()? cancelled}) async {
    final jobs = <(int, int, int)>[];
    for (var z = zMin; z <= zMax; z++) {
      final a = _xy(bounds.north, bounds.west, z), b = _xy(bounds.south, bounds.east, z);
      for (var x = a.$1; x <= b.$1; x++) {
        for (var y = a.$2; y <= b.$2; y++) {
          jobs.add((z, x, y));
          if (jobs.length >= cap) break;
        }
      }
    }
    var done = 0, got = 0;
    for (var i = 0; i < jobs.length; i += 4) {
      if (cancelled?.call() == true) break;
      final batch = jobs.sublist(i, math.min(i + 4, jobs.length));
      final r = await Future.wait(batch.map((j) => get(base, headers, layer, j.$1, j.$2, j.$3, online: true)));
      got += r.where((e) => e != null).length;
      done += batch.length;
      progress?.call(done, jobs.length);
    }
    return got;
  }

  static (int, int) _xy(double lat, double lon, int z) {
    final n = 1 << z;
    final x = ((lon + 180) / 360 * n).floor().clamp(0, n - 1);
    final r = lat * math.pi / 180;
    final y = ((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n).floor().clamp(0, n - 1);
    return (x, y);
  }
}

class LatLngBoundsLike {
  const LatLngBoundsLike(this.north, this.south, this.east, this.west);
  final double north, south, east, west;
}

class DashboardTileProvider extends TileProvider {
  DashboardTileProvider({required this.base, required this.authHeaders, required this.layer, required this.online});
  final String base;
  final Map<String, String> authHeaders;
  final String layer;
  final bool online;

  @override
  ImageProvider getImage(TileCoordinates coordinates, TileLayer options) => _TileImage(this, coordinates.z, coordinates.x, coordinates.y);
}

class _TileImage extends ImageProvider<_TileImage> {
  const _TileImage(this.p, this.z, this.x, this.y);
  final DashboardTileProvider p;
  final int z, x, y;

  @override
  Future<_TileImage> obtainKey(ImageConfiguration configuration) => SynchronousFuture(this);

  @override
  ImageStreamCompleter loadImage(_TileImage key, ImageDecoderCallback decode) =>
      MultiFrameImageStreamCompleter(codec: _load(decode), scale: 1);

  Future<ui.Codec> _load(ImageDecoderCallback decode) async {
    final b = await TileCache.I.get(p.base, p.authHeaders, p.layer, z, x, y, online: p.online);
    if (b == null) throw StateError('no tile');
    return decode(await ui.ImmutableBuffer.fromUint8List(b));
  }

  @override
  bool operator ==(Object other) => other is _TileImage && other.p.layer == p.layer && other.z == z && other.x == x && other.y == y;

  @override
  int get hashCode => Object.hash(p.layer, z, x, y);
}
