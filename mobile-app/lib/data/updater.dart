// App updates over the air, no store. Two places can offer a newer build:
//  - your own dashboard: GET /api/mobile/latest -> {version, build, url, sha256}
//  - a GitHub repo's releases (tags mobile-v<version>+<build>, an .apk asset and its
//    <name>.apk.sha256), set in Settings. A private repo (the test channel on
//    oui-spy-recon-development) needs a read-only token the user types in; it is kept in
//    the app's private storage and only ever sent to api.github.com.
// Android downloads the APK, checks its sha256 when one is published and opens the
// installer (the same signing key is needed for it to update in place). iOS builds update
// through TestFlight or a rebuild.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:ota_update/ota_update.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'dashboard.dart';

const appVersion = '0.2.1';
const appBuild = 3;

class Release {
  Release(this.version, this.build, this.url, this.sha256, this.from);
  final String version, url, from;
  final int build;
  final String? sha256;
}

class Updater extends ChangeNotifier {
  Updater._();
  static final I = Updater._();
  final _client = http.Client();

  String repo = ''; // "owner/name" of a releases repo, blank = none
  String token = ''; // a read-only GitHub token for a private repo; never shown or logged
  bool auto = true; // look once a day
  Release? found;
  String state = ''; // checking, up to date, downloading 42%, ...

  Future<void> load() async {
    final p = await SharedPreferences.getInstance();
    repo = p.getString('updateRepo') ?? 'colonelpanichacks/oui-spy-recon-development';
    token = p.getString('updateToken') ?? '';
    auto = p.getBool('updateAuto') ?? true;
  }

  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString('updateRepo', repo.trim());
    await p.setString('updateToken', token.trim());
    await p.setBool('updateAuto', auto);
    notifyListeners();
  }

  /// The daily look, if on and due.
  Future<void> maybeAuto(Dashboard dash) async {
    if (!auto) return;
    final p = await SharedPreferences.getInstance();
    final last = p.getInt('updateLast') ?? 0;
    final now = DateTime.now().millisecondsSinceEpoch;
    if (now - last < const Duration(hours: 20).inMilliseconds) return;
    await p.setInt('updateLast', now);
    await check(dash);
  }

  Future<Release?> check(Dashboard dash) async {
    state = 'checking';
    notifyListeners();
    final rs = <Release>[];
    // asked quietly: a dashboard without app builds answers 404, which is no link error
    Object? j;
    if (dash.base.isNotEmpty) {
      try {
        final r = await _client.get(Uri.parse('${dash.base}/api/mobile/latest'), headers: dash.authHeaders).timeout(const Duration(seconds: 8));
        if (r.statusCode == 200) j = jsonDecode(r.body);
      } catch (_) {}
    }
    if (j is Map && j['build'] is num && '${j['url'] ?? ''}'.isNotEmpty) {
      final u = '${j['url']}';
      rs.add(Release('${j['version'] ?? ''}', (j['build'] as num).toInt(), u.startsWith('http') ? u : '${dash.base}$u', j['sha256'] as String?, 'your dashboard'));
    }
    if (repo.contains('/')) {
      final g = await _github(repo);
      if (g != null) rs.add(g);
    }
    rs.sort((a, b) => b.build.compareTo(a.build));
    final best = rs.firstOrNull;
    found = best != null && best.build > appBuild ? best : null;
    state = found != null ? 'version ${found!.version} is ready (from ${found!.from})' : (rs.isEmpty ? 'no update source answered' : 'up to date');
    notifyListeners();
    return found;
  }

  Future<Release?> _github(String repo) async {
    try {
      final r = await _client
          .get(Uri.parse('https://api.github.com/repos/$repo/releases?per_page=10'), headers: _gh('application/vnd.github+json'))
          .timeout(const Duration(seconds: 10));
      if (r.statusCode != 200) return null;
      for (final rel in jsonDecode(r.body) as List) {
        if (rel['draft'] == true || rel['prerelease'] == true) continue;
        final m = RegExp(r'^mobile-v([\d.]+)\+(\d+)$').firstMatch('${rel['tag_name']}');
        if (m == null) continue;
        final assets = [for (final a in rel['assets'] as List) a as Map];
        final apk = assets.where((a) => '${a['name']}'.endsWith('.apk')).firstOrNull;
        if (apk == null) continue;
        String? sum;
        final sa = assets.where((a) => '${a['name']}' == '${apk['name']}.sha256').firstOrNull;
        if (sa != null) {
          final u = await _assetUrl(sa);
          if (u != null) {
            final s = await _client.get(Uri.parse(u)).timeout(const Duration(seconds: 10));
            if (s.statusCode == 200) sum = s.body.trim().split(RegExp(r'\s+')).first;
          }
        }
        // the APK's own link, resolved when it is installed (a private repo's is short-lived)
        return Release(m.group(1)!, int.parse(m.group(2)!), '${apk['url']}', sum, 'GitHub $repo');
      }
    } catch (_) {}
    return null;
  }

  Map<String, String> _gh(String accept) => {'Accept': accept, 'User-Agent': 'OUI-SPY-Recon', if (token.isNotEmpty) 'Authorization': 'Bearer $token'};

  /// A release asset's download link. Through the API (so a private repo works with the
  /// token), stopping at GitHub's redirect: the signed link it points at needs no token,
  /// and must not be sent one.
  Future<String?> _assetUrl(Map a) async {
    try {
      final rq = http.Request('GET', Uri.parse('${a['url']}'))
        ..followRedirects = false
        ..headers.addAll(_gh('application/octet-stream'));
      final r = await _client.send(rq).timeout(const Duration(seconds: 15));
      await r.stream.drain<void>();
      if (r.statusCode == 302 || r.statusCode == 301) return r.headers['location'];
      if (r.statusCode == 200) return '${a['browser_download_url']}';
    } catch (_) {}
    return null;
  }

  /// Downloads and opens the installer (Android).
  Future<void> install(Release r) async {
    if (!Platform.isAndroid) {
      state = 'iPhone builds update through TestFlight or a rebuild';
      notifyListeners();
      return;
    }
    try {
      var url = r.url;
      if (url.startsWith('https://api.github.com/')) {
        url = await _assetUrl({'url': url, 'browser_download_url': url}) ?? '';
        if (url.isEmpty) {
          state = "GitHub didn't hand over the download (is the token still valid?)";
          notifyListeners();
          return;
        }
      }
      OtaUpdate().execute(url, destinationFilename: 'oui-spy-recon-${r.version}.apk', sha256checksum: r.sha256).listen((e) {
        state = switch (e.status) {
          OtaStatus.DOWNLOADING => 'downloading ${e.value}%',
          OtaStatus.INSTALLING => 'opening the installer',
          OtaStatus.CHECKSUM_ERROR => 'the download did not match its checksum: not installed',
          OtaStatus.PERMISSION_NOT_GRANTED_ERROR => 'allow installs from this app (Android asks once)',
          _ => e.status == OtaStatus.INSTALLATION_DONE ? 'installed' : 'update failed (${e.value ?? e.status.name})',
        };
        notifyListeners();
      });
    } catch (e) {
      state = 'update failed';
      notifyListeners();
    }
  }
}
