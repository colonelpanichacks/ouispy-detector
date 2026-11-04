// App updates over the air, no store. Two places can offer a newer build:
//  - your own dashboard: GET /api/mobile/latest -> {version, build, url, sha256}
//  - a public GitHub repo's releases (tags mobile-v<version>+<build>, an .apk asset, and
//    optionally a <name>.apk.sha256 asset), set in Settings. The app's own repo is private,
//    so its releases can't be read without a token; a releases-only public repo can.
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

const appVersion = '0.2.0';
const appBuild = 2;

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

  String repo = ''; // "owner/name" of a public releases repo, blank = none
  bool auto = true; // look once a day
  Release? found;
  String state = ''; // checking, up to date, downloading 42%, ...

  Future<void> load() async {
    final p = await SharedPreferences.getInstance();
    repo = p.getString('updateRepo') ?? '';
    auto = p.getBool('updateAuto') ?? true;
  }

  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString('updateRepo', repo.trim());
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
          .get(Uri.parse('https://api.github.com/repos/$repo/releases?per_page=10'), headers: {'Accept': 'application/vnd.github+json', 'User-Agent': 'OUI-SPY-Recon'})
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
          final s = await _client.get(Uri.parse('${sa['browser_download_url']}')).timeout(const Duration(seconds: 10));
          if (s.statusCode == 200) sum = s.body.trim().split(RegExp(r'\s+')).first;
        }
        return Release(m.group(1)!, int.parse(m.group(2)!), '${apk['browser_download_url']}', sum, 'GitHub $repo');
      }
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
      OtaUpdate().execute(r.url, destinationFilename: 'oui-spy-recon-${r.version}.apk', sha256checksum: r.sha256).listen((e) {
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
