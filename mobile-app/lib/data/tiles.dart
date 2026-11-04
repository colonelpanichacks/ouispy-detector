// Map tiles: free, keyless layers (mapLayers below), through the dashboard's own tile
// endpoint (/tiles/<layer>/<z>/<x>/<y>, which proxies and caches them on the Mac) when it
// is connected and has the layer, else straight from the provider. Every tile is kept on
// the phone, so the map works with no link at all.
import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';

class MapLayer {
  const MapLayer(this.id, this.name, this.url, {this.maxZoom = 19, this.bulk = true, this.labels, this.credit = ''});
  final String id, name, url;
  final int maxZoom;

  /// The provider allows saving an area for offline use (OSM's and OpenTopoMap's policies don't).
  final bool bulk;

  /// A reference layer drawn on top (names and roads over imagery).
  final String? labels;
  final String credit;

  String tile(int z, int x, int y) => url.replaceAll('{z}', '$z').replaceAll('{x}', '$x').replaceAll('{y}', '$y');
}

const _esri = 'https://server.arcgisonline.com/ArcGIS/rest/services';
const _usgs = 'https://basemap.nationalmap.gov/arcgis/rest/services';

/// Free map layers, no keys and no accounts.
const mapLayers = <MapLayer>[
  MapLayer('esri-sat', 'Satellite', '$_esri/World_Imagery/MapServer/tile/{z}/{y}/{x}', credit: 'Esri, Maxar, Earthstar'),
  MapLayer('esri-hybrid', 'Satellite + labels', '$_esri/World_Imagery/MapServer/tile/{z}/{y}/{x}', labels: 'esri-labels', credit: 'Esri, Maxar'),
  MapLayer('esri-dark', 'Dark', '$_esri/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}', maxZoom: 16, labels: 'esri-dark-labels', credit: 'Esri'),
  MapLayer('esri-darkgray', 'Dark, no labels', '$_esri/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}', maxZoom: 16, credit: 'Esri'),
  MapLayer('esri-topo', 'Topographic', '$_esri/World_Topo_Map/MapServer/tile/{z}/{y}/{x}', credit: 'Esri'),
  MapLayer('esri-street', 'Streets', '$_esri/World_Street_Map/MapServer/tile/{z}/{y}/{x}', credit: 'Esri'),
  MapLayer('usgs-sat', 'USGS imagery (US)', '$_usgs/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}', maxZoom: 16, credit: 'USGS'),
  MapLayer('usgs-topo', 'USGS topo (US)', '$_usgs/USGSTopo/MapServer/tile/{z}/{y}/{x}', maxZoom: 16, credit: 'USGS'),
  MapLayer('osm', 'OpenStreetMap', 'https://tile.openstreetmap.org/{z}/{x}/{y}.png', bulk: false, credit: 'OpenStreetMap contributors'),
  MapLayer('opentopo', 'OpenTopoMap', 'https://tile.opentopomap.org/{z}/{x}/{y}.png', maxZoom: 17, bulk: false, credit: 'OpenTopoMap, OSM contributors'),
  MapLayer('carto-dark', 'Carto dark', 'https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png', maxZoom: 20, credit: 'CARTO, OSM contributors'),
  MapLayer('carto-light', 'Carto light', 'https://basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png', maxZoom: 20, credit: 'CARTO, OSM contributors'),
];

/// Overlays drawn over a base layer (never picked on their own).
const _overlays = <MapLayer>[
  MapLayer('esri-labels', 'Labels', '$_esri/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}'),
  MapLayer('esri-dark-labels', 'Labels', '$_esri/Canvas/World_Dark_Gray_Reference/MapServer/tile/{z}/{y}/{x}', maxZoom: 16),
];

MapLayer layerById(String id) => [...mapLayers, ..._overlays].firstWhere((l) => l.id == id, orElse: () => mapLayers.first);

const _ua = {'User-Agent': 'OUI-SPY-Recon/0.1 (Android; +https://github.com/colonelpanichacks)'};

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
    if (!online) return null;
    // the dashboard first (it caches for the whole fleet), then the provider itself
    final b =
        (base.isNotEmpty && dashLayers.contains(layer) ? await _fetch(Uri.parse('$base/tiles/$layer/$z/$x/$y'), headers) : null) ??
        await _fetch(Uri.parse(layerById(layer).tile(z, x, y)), _ua);
    if (b == null) return null;
    try {
      await f.parent.create(recursive: true);
      await f.writeAsBytes(b, flush: false);
    } catch (_) {}
    return b;
  }

  /// The layers the connected dashboard serves (/api/tiles).
  Set<String> dashLayers = {};

  Future<Uint8List?> _fetch(Uri u, Map<String, String> headers) async {
    try {
      final r = await _client.get(u, headers: headers).timeout(const Duration(seconds: 15));
      // Esri answers a missing tile with a tiny blank placeholder: not a tile
      if (r.statusCode != 200 || r.bodyBytes.length < 200) return null;
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
  Future<int> prefetch(
    String base,
    Map<String, String> headers,
    String layer,
    LatLngBoundsLike bounds,
    int zMin,
    int zMax, {
    int cap = 4000,
    void Function(int, int)? progress,
    bool Function()? cancelled,
  }) async {
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
  ImageStreamCompleter loadImage(_TileImage key, ImageDecoderCallback decode) => MultiFrameImageStreamCompleter(codec: _load(decode), scale: 1);

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
