// What is on the other end of the console, and what it is saying. Reads a Backend's
// console lines and recognises the board (an OUI-SPY mesh detector, an OUI-SPY Recon running
// Unified Blue, the legacy bare-XIAO Unified Blue wrapper) and the Unified Blue mode it runs,
// then keeps each mode's live data in bounded rings.
//
// Line formats (read-only references):
//   Recon:  oui-spy-recon-blue/lib/oui_mesh/src/rb_mesh.cpp (TLM kind:"reconblue", app, mstate;
//           RST fw "Nov04 21:21:25R"; `blue <id>`; the mesh detection line)
//   modes:  oui-spy-recon-blue/lib/blue_modes/src/raw/*.cpp (detector, foxhunter, flock-you,
//           pcap, blesniff, skyspy, axeoff), sky-spy-c5/src/main.cpp (5 GHz drones)
//   legacy: oui-spy-unified-blue/src/main.cpp (banner, FINAL BOOT MODE N)
//   mesh:   ouispy-detector src/main.cpp (TLM kind, fw suffix W/Y/S/G)
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';

import 'backend.dart';

enum BoardFamily { unknown, ouispy, recon, legacyUnified }

String familyName(BoardFamily f) => switch (f) {
  BoardFamily.ouispy => 'OUI-SPY mesh detector',
  BoardFamily.recon => 'OUI-SPY Recon',
  BoardFamily.legacyUnified => 'Unified Blue (legacy)',
  BoardFamily.unknown => 'Unknown',
};

/// The Unified Blue modes: id (as `blue <id>` and TLM `app` spell it), name, one line.
const blueModes = <(String, String, String)>[
  ('detector', 'Detector', 'BLE alert tool for specific devices'),
  ('foxhunter', 'Foxhunter', 'RSSI proximity tracker for one MAC'),
  ('flock-wifi', 'Flock-You', 'Promiscuous 2.4 GHz surveillance sniffer'),
  ('pcap', 'PCAP', 'Passive Wi-Fi packet capture'),
  ('skyspy', 'Sky Spy', 'Drone Remote ID monitor'),
  ('blesniff', 'BLE Sniff', 'Passive BLE advertising capture'),
  ('axeoff', 'AXEOFF', 'Headless Axon detector'),
  ('mesh', 'Mesh node', 'OUI-SPY Mesh relay or home, no scanning'),
];

/// The legacy wrapper numbers its modes (0 is its selector AP).
const legacyModeIds = ['selector', 'detector', 'foxhunter', 'flock-wifi', 'pcap', 'skyspy', 'blesniff', 'axeoff'];

String modeName(String id) {
  if (id.isEmpty) return '';
  if (id == 'selector') return 'Mode selector';
  for (final (i, n, _) in blueModes) {
    if (i == id) return n;
  }
  return id;
}

/// What an OUI-SPY mesh detector build is, from its TLM kind.
String ouispyKindName(String k) => switch (k) {
  'wardrive' => 'Wardriver',
  'gpswd' => 'GPS wardriver',
  'gps' => 'GPS node',
  'skyspy' => 'Sky-Spy node',
  'detector' => 'Mesh detector',
  _ => k,
};

/// From the build stamp's last letter (FleetNode.kind): R = Recon, else a mesh detector kind.
String kindFromFw(String fw) {
  if (fw.isEmpty) return '';
  return switch (fw[fw.length - 1]) {
    'R' => 'reconblue',
    'W' => 'wardrive',
    'Y' => 'gpswd',
    'S' => 'skyspy',
    'G' => 'gps',
    _ => 'detector',
  };
}

/// A fixed-size log, oldest first inside; [items] gives newest first.
class Ring<T> {
  Ring([this.cap = 500]);
  final int cap;
  final _l = <T>[];
  void add(T v) {
    _l.add(v);
    if (_l.length > cap) _l.removeRange(0, _l.length - cap);
  }

  int get length => _l.length;
  bool get isEmpty => _l.isEmpty;
  bool get isNotEmpty => _l.isNotEmpty;
  T? get last => _l.isEmpty ? null : _l.last;
  Iterable<T> get items => _l.reversed;
  List<T> get oldestFirst => List.unmodifiable(_l);
  void clear() => _l.clear();
}

final _pwRe = RegExp(r'\b(pass(?:word|wd)?|pwd|psk|appass|ap_pass)\b(\s*[:=]\s*)(.*)$', caseSensitive: false);

/// Anything printed after "password" / "pass" is cut, so an AP password never lands in the app.
String redact(String l) => l.replaceFirstMapped(_pwRe, (m) => '${m[1]}${m[2]}***');

// ---------------------------------------------------------------- per-mode records

class RssiPoint {
  RssiPoint(this.ts, this.rssi);
  final double ts;
  final int rssi;
}

class FoxEvent {
  FoxEvent(this.ts, this.what);
  final double ts;
  final String what; // acquired | reacquired | lost
}

class FoxState {
  String target = '';
  String state = ''; // searching | tracking | lost | config
  int? rssi;
  double last = 0;
  final history = Ring<RssiPoint>(500);
  final events = Ring<FoxEvent>(100);
  void clear() {
    target = '';
    state = '';
    rssi = null;
    last = 0;
    history.clear();
    events.clear();
  }
}

class PcapFrame {
  PcapFrame({
    required this.ts,
    required this.ch,
    required this.rssi,
    this.type = '',
    this.sub = '',
    this.src = '',
    this.dst = '',
    this.bssid = '',
    this.len = 0,
    this.ssid = '',
  });
  final double ts;
  final int ch, rssi, len;
  final String type, sub, src, dst, bssid, ssid;
  String get kind => sub.isEmpty ? type : '$type/$sub';
}

class PcapState {
  final frames = Ring<PcapFrame>(500);
  final byType = <String, int>{};
  final byKind = <String, int>{};
  final byChan = <int, int>{};
  int total = 0;
  Map<String, dynamic> status = {};
  double statusTs = 0;
  void clear() {
    frames.clear();
    byType.clear();
    byKind.clear();
    byChan.clear();
    total = 0;
    status = {};
    statusTs = 0;
  }

  int? get channel => (status['chan'] as num?)?.toInt();
  int? get dwell => (status['dwell'] as num?)?.toInt();
  int? get hopMask {
    final h = status['hopmask'];
    if (h is num) return h.toInt();
    if (h is String) return int.tryParse(h.replaceFirst(RegExp('^0[xX]'), ''), radix: 16);
    return null;
  }

  bool get hopping => '${status['mode'] ?? ''}'.toUpperCase() == 'HOP';
}

class Advert {
  Advert({
    required this.ts,
    this.ch,
    required this.rssi,
    required this.pdu,
    required this.addrType,
    required this.mac,
    this.name = '',
    this.svc = '',
    this.mfrId,
    this.mfrName = '',
  });
  final double ts;
  final int? ch;
  final int rssi;
  final String pdu, addrType, mac, name, svc, mfrName;
  final int? mfrId;
}

class BleDev {
  BleDev(this.mac);
  final String mac;
  String addrType = '', name = '', mfrName = '', svc = '';
  int? mfrId;
  int count = 0;
  int best = -127, rssi = -127;
  double first = 0, last = 0;
}

class BleSniffState {
  final adverts = Ring<Advert>(500);
  final devices = <String, BleDev>{};
  int total = 0;
  Map<String, dynamic> status = {};
  double statusTs = 0;
  void clear() {
    adverts.clear();
    devices.clear();
    total = 0;
    status = {};
    statusTs = 0;
  }

  int? get window => (status['scan_win'] as num?)?.toInt();
  int? get interval => (status['scan_int'] as num?)?.toInt();

  /// Devices by most recently heard.
  List<BleDev> get byLast => devices.values.toList()..sort((a, b) => b.last.compareTo(a.last));
}

class FlockDet {
  FlockDet(this.ts, this.raw);
  final double ts;
  final Map<String, dynamic> raw;
  String get method => _s(raw['detection_method']);
  int? get tier => _i(raw['detection_tier']);
  String get protocol => _s(raw['protocol']);
  String get mac => _s(raw['mac_address']).toUpperCase();
  String get oui => _s(raw['oui']);
  String get name => _s(raw['device_name']);
  int? get rssi => _i(raw['rssi']);
  int? get channel => _i(raw['channel']);
  int? get freq => _i(raw['frequency']);
  String get ssid => _s(raw['ssid']);
  bool get ble => protocol == 'ble';
}

class FlockTier {
  FlockTier(this.tier, this.method, this.beep);
  final int tier;
  final String method;
  final bool beep;
}

/// Flock-You's tiers (flockyou_promiscious.cpp TIER_*), the label when no config came yet.
const flockTierNames = ['SSID', 'OUI echo (addr1/addr3)', 'OUI (addr2)', 'Wildcard probe', 'Probe IE signature'];

class FlockState {
  final dets = Ring<FlockDet>(500);
  int? beepMask;
  int? ouiCount;
  List<FlockTier> tiers = [];
  double configTs = 0;
  void clear() {
    dets.clear();
    beepMask = null;
    ouiCount = null;
    tiers = [];
    configTs = 0;
  }

  bool tierBeeps(int t) => beepMask != null && (beepMask! >> t) & 1 == 1;
}

class DetectorDet {
  DetectorDet(this.ts, this.raw);
  final double ts;
  final Map<String, dynamic> raw;
  String get mac => _s(raw['mac_address'] ?? raw['mac']).toUpperCase();
  int? get rssi => _i(raw['rssi']);
  int? get rssiMin => _i(raw['rssi_min']);
  int? get rssiMax => _i(raw['rssi_max']);
  String get method => _s(raw['match_method'] ?? raw['detection_method']);
  String get addrType => _s(raw['addr_type']);
  String get name => _s(raw['device_name']).isNotEmpty ? _s(raw['device_name']) : _s(raw['local_name']);
  String get alias => _s(raw['alias']);
  String get signature => _s(raw['matched_signature']);
  int? get companyId => _i(raw['company_id']);
  int? get serviceUuid => _i(raw['service_uuid']);
  int? get hits => _i(raw['hit_count']);
  int? get firstMs => _i(raw['first_seen_ms']);
  int? get lastMs => _i(raw['last_seen_ms']);
  String get replay => _s(raw['replay_source']);
}

class DetectorState {
  final live = Ring<DetectorDet>(500);
  List<DetectorDet> dumpLive = [], dumpPrev = [];
  double dumpLiveTs = 0, dumpPrevTs = 0;
  String dumping = ''; // live | prev while a dump is streaming
  final _buf = <DetectorDet>[];
  Map<String, dynamic> status = {};
  double statusTs = 0;
  void clear() {
    live.clear();
    dumpLive = [];
    dumpPrev = [];
    dumpLiveTs = dumpPrevTs = 0;
    dumping = '';
    _buf.clear();
    status = {};
    statusTs = 0;
  }
}

class AxeDet {
  AxeDet(this.ts, this.mac, this.rssi, this.type, this.match, this.note);
  final double ts;
  final String mac, type, match, note;
  final int? rssi;
  bool get ble => type.startsWith('BLE');
}

class AxeState {
  final dets = Ring<AxeDet>(500);
  String state = ''; // IDLE | TRACKING
  double stateTs = 0;
  void clear() {
    dets.clear();
    state = '';
    stateTs = 0;
  }
}

class SkyDrone {
  SkyDrone(this.mac);
  final String mac;
  int? rssi;
  double? lat, lon, plat, plon;
  int? alt, channel;
  String basicId = '', band = '', src = '';
  int count = 0;
  double first = 0, last = 0;
  bool get hasPos => lat != null && lon != null && !(lat == 0 && lon == 0);
}

class SkyState {
  final drones = <String, SkyDrone>{};
  final log = Ring<SkyDrone>(500);
  Map<String, dynamic> status = {};
  double statusTs = 0;
  void clear() {
    drones.clear();
    log.clear();
    status = {};
    statusTs = 0;
  }

  List<SkyDrone> get byLast => drones.values.toList()..sort((a, b) => b.last.compareTo(a.last));
}

/// A mesh detection line ({mac, rssi, type, desc, node, dom}): Recon prints it in every
/// scanning mode once the USB feed or the mesh is on, the mesh detector always.
class MeshDet {
  MeshDet(this.ts, this.raw);
  final double ts;
  final Map<String, dynamic> raw;
  String get mac => _s(raw['mac']).toUpperCase();
  int? get rssi => _i(raw['rssi']);
  String get type => _s(raw['type']);
  String get match => _s(raw['match']);
  String get desc => _s(raw['desc']);
  String get node => _s(raw['node']);
  String get dom => _s(raw['dom']);
}

// ---------------------------------------------------------------- the commands

/// The exact console lines each mode reads.
class BlueCmd {
  static const status = 'status';
  static const version = 'CMD:VERSION';
  static const getConfig = '{"cmd":"get_config"}';
  static String pcapChan(int ch) => 'CMD:CHAN ${ch.clamp(1, 14)}';

  /// Channel n is bit n-1 (the PCAP dashboard's hopmask).
  static int hopMaskOf(Iterable<int> channels) {
    var m = 0;
    for (final c in channels) {
      if (c >= 1 && c <= 14) m |= 1 << (c - 1);
    }
    return m;
  }

  static Set<int> channelsOf(int mask) => {
    for (var c = 1; c <= 14; c++)
      if ((mask >> (c - 1)) & 1 == 1) c,
  };
  static String pcapHop(int mask) => 'CMD:HOP 0x${(mask & 0x3FFF).toRadixString(16).padLeft(4, '0')}';
  static String pcapDwell(int ms) => 'CMD:DWELL ${ms.clamp(100, 2000)}';
  static String bleWindow(int ms) => 'CMD:WINDOW ${ms.clamp(10, 2000)}';
  static String bleInterval(int ms) => 'CMD:INTERVAL ${ms.clamp(20, 4000)}';
  static const modeStatus = 'CMD:STATUS';
  static const dumpLive = 'CMD:DUMP_LIVE', dumpPrev = 'CMD:DUMP_PREV', clearLive = 'CMD:CLEAR_LIVE', clearPrev = 'CMD:CLEAR_PREV';
  static String setBeep(int tier, bool on) => '{"cmd":"set_beep","tier":$tier,"on":${on ? 1 : 0}}';
  static String setBeepMask(int mask) => '{"cmd":"set_beep_mask","mask":${mask & 0x1F}}';

  /// Recon: run a Unified Blue mode (the board saves it and reboots). Never a bare `blue`:
  /// that saves an empty mode.
  static String blue(String id) {
    if (!blueModes.any((m) => m.$1 == id)) throw ArgumentError('unknown Unified Blue mode "$id"');
    return 'blue $id';
  }

  static String mesh(bool on, String role, int ch) {
    if (role != 'home' && role != 'remote') throw ArgumentError('role is home or remote');
    return 'mesh ${on ? 1 : 0} $role ${ch.clamp(1, 11)}';
  }
}

// ---------------------------------------------------------------- the board

String _s(Object? v) => v == null ? '' : '$v';
int? _i(Object? v) => v is num ? v.toInt() : (v is String ? int.tryParse(v) : null);
double? _d(Object? v) => v is num ? v.toDouble() : (v is String ? double.tryParse(v) : null);

final _pcapRe = RegExp(r'^\[Ch(\d+) RSSI(-?\d+)dBm\] (?:SHORT len=(\d+)|([^/\s]+)/(\S+) src=(\S+) dst=(\S+) bssid=(\S+) len=(\d+)(?: ssid=(.*))?)\s*$');
final _bleRe = RegExp(r'^\[Ch(\d+|\?) RSSI(-?\d+)dBm\] ([A-Z_?]+) ([\w-]+):([0-9a-fA-F:]{17})(?: name="(.*?)")?(?: svc=(\S+))?(?: mfr=([0-9A-Fa-f]{4})\(([^)]*)\))?\s*$');
final _foxRssi = RegExp(r'^RSSI: (-?\d+) dBm');
final _foxDebug = RegExp(r'^DEBUG: (?:Target detected|Beep ON), RSSI: (-?\d+)');
final _foxTarget = RegExp(r'^(?:Received target MAC|Target MAC): ?([0-9A-Fa-f:]{17})');
final _verRe = RegExp(r'^OUI-SPY (BLE DETECTOR|PCAP|BLESNIFF) (\S+)');
final _legacyBanner = RegExp(r'OUI SPY UNIFIED FIRMWARE v?(\S+)');
final _legacyMode = RegExp(r'\[OUI-SPY\] (?:FINAL BOOT MODE: |\*\*\* BOOTING INTO FIRMWARE MODE |REBOOTING INTO MODE )(\d+)');
final _legacyStart = RegExp(r'>>> STARTING .* \(mode (\d+)\) <<<');
final _modeMgr = RegExp(r"^\[MODEMGR\] starting '([\w-]+)'");
final _okBlue = RegExp(r'^OK blue ([\w-]+)');
final _okMesh = RegExp(r'^OK mesh (\d) (\w+) ch(\d+)');
const _axeTypes = {'BLE_CID', 'BLE_SVC', 'BLE_MAC', 'WIFI_PROBE', 'WIFI_BEACON'};

class Board extends ChangeNotifier {
  Board({double Function()? clock}) : _clock = clock ?? (() => DateTime.now().millisecondsSinceEpoch / 1000);
  static final I = Board();
  final double Function() _clock;

  Backend? _b;
  StreamSubscription<ConsoleLine>? _sub;
  Timer? _notify;
  bool _dirty = false;

  // what it is
  BoardFamily family = BoardFamily.unknown;
  String chip = '';
  String firmware = ''; // the build stamp (RST/TLM fw) or the banner version
  String version = ''; // a mode's own version (CMD:VERSION)
  String fwKind = ''; // reconblue | detector | wardrive | gpswd | gps | skyspy
  String modeId = '';
  String nodeId = '';
  String pendingMode = ''; // `OK blue X — rebooting` seen, the new mode not yet heard
  // its mesh
  bool? meshOn;
  String meshRole = '', meshRate = '', meshState = '';
  int? meshCh;
  // the console
  final lines = Ring<ConsoleLine>(200); // redacted
  double lastLine = 0, lastTlm = 0;
  int lineCount = 0;
  bool probing = false;
  double probedAt = 0;

  // the modes
  final fox = FoxState();
  final pcap = PcapState();
  final ble = BleSniffState();
  final flock = FlockState();
  final det = DetectorState();
  final axe = AxeState();
  final sky = SkyState();
  final meshDets = Ring<MeshDet>(500);

  String get modeLabel => modeName(modeId);
  bool get attached => _b != null;
  bool get recognised => family != BoardFamily.unknown || modeId.isNotEmpty;

  /// A one-line "what is this": the family and its mode or kind.
  String get summary {
    final f = family == BoardFamily.unknown && modeId.isNotEmpty ? 'Unified Blue' : familyName(family);
    if (family == BoardFamily.ouispy) return fwKind.isEmpty ? f : '$f · ${ouispyKindName(fwKind)}';
    return modeId.isEmpty ? f : '$f · ${modeName(modeId)}';
  }

  void attach(Backend b) {
    if (identical(b, _b)) return;
    detach();
    reset();
    _b = b;
    // catch up on what was printed before we listened (the board doesn't reset on open)
    for (final c in b.console.ring.toList()) {
      _take(c);
    }
    _sub = b.console.stream.listen(_take);
    _changed(now: true);
  }

  void detach() {
    _sub?.cancel();
    _sub = null;
    _b = null;
  }

  /// Forget everything about the board (a new one may be plugged in).
  void reset() {
    family = BoardFamily.unknown;
    chip = firmware = version = fwKind = modeId = nodeId = pendingMode = '';
    meshOn = null;
    meshRole = meshRate = meshState = '';
    meshCh = null;
    lines.clear();
    lastLine = lastTlm = probedAt = 0;
    lineCount = 0;
    for (final c in [fox.clear, pcap.clear, ble.clear, flock.clear, det.clear, axe.clear, sky.clear, meshDets.clear]) {
      c();
    }
    _changed(now: true);
  }

  @override
  void dispose() {
    detach();
    _notify?.cancel();
    super.dispose();
  }

  void _take(ConsoleLine c) {
    if (c.src == 'phone') return; // our own commands echoed into the console
    feed(c.line, ts: c.ts, src: c.src);
  }

  // Lines arrive by the hundred a second in PCAP: coalesce the repaints.
  void _changed({bool now = false}) {
    _dirty = true;
    if (now) {
      _notify?.cancel();
      _notify = null;
      _dirty = false;
      notifyListeners();
      return;
    }
    _notify ??= Timer(const Duration(milliseconds: 250), () {
      _notify = null;
      if (_dirty) {
        _dirty = false;
        notifyListeners();
      }
    });
  }

  // ------------------------------------------------------------ sending

  Future<bool> send(String line) async {
    final b = _b;
    if (b == null) return false;
    return b.sendLine(line);
  }

  /// Ask the board what it is: `status` (Recon / mesh: an RST line), `CMD:VERSION` (Detector,
  /// PCAP, BLE Sniff), `{"cmd":"get_config"}` (Flock-You), 400 ms apart. The rest are
  /// recognised from what they print.
  Future<void> probe() async {
    if (probing) return;
    probing = true;
    probedAt = _clock();
    _changed(now: true);
    try {
      for (final (i, l) in [BlueCmd.status, BlueCmd.version, BlueCmd.getConfig].indexed) {
        if (i > 0) await Future<void>.delayed(const Duration(milliseconds: 400));
        await send(l);
      }
      await Future<void>.delayed(const Duration(milliseconds: 1200));
    } finally {
      probing = false;
      _changed(now: true);
    }
  }

  Future<bool> modeStatus() => send(BlueCmd.modeStatus);
  Future<bool> pcapChannel(int ch) => send(BlueCmd.pcapChan(ch));
  Future<bool> pcapHop(Iterable<int> channels) {
    final m = BlueCmd.hopMaskOf(channels);
    return m == 0 ? Future.value(false) : send(BlueCmd.pcapHop(m));
  }

  Future<bool> pcapDwell(int ms) => send(BlueCmd.pcapDwell(ms));
  Future<bool> bleWindow(int ms) => send(BlueCmd.bleWindow(ms));
  Future<bool> bleInterval(int ms) => send(BlueCmd.bleInterval(ms));
  Future<bool> detDump({bool prev = false}) => send(prev ? BlueCmd.dumpPrev : BlueCmd.dumpLive);
  Future<bool> detClear({bool prev = false}) async {
    final ok = await send(prev ? BlueCmd.clearPrev : BlueCmd.clearLive);
    if (ok) {
      if (prev) {
        det.dumpPrev = [];
      } else {
        det.live.clear();
        det.dumpLive = [];
      }
      _changed(now: true);
    }
    return ok;
  }

  Future<bool> flockConfig() => send(BlueCmd.getConfig);
  Future<bool> flockSetBeep(int tier, bool on) => send(BlueCmd.setBeep(tier, on));
  Future<bool> flockSetBeepMask(int mask) => send(BlueCmd.setBeepMask(mask));

  /// Recon only: saves the mode and reboots into it.
  Future<bool> switchMode(String id) => send(BlueCmd.blue(id));
  Future<bool> meshProvision(bool on, String role, int ch) => send(BlueCmd.mesh(on, role, ch));

  // ------------------------------------------------------------ reading

  /// One console line in; public so tests (and a replay) can feed lines straight.
  void feed(String raw, {double? ts, String src = 'serial'}) {
    final t = ts ?? _clock();
    final l = raw.trimRight();
    if (l.isEmpty) return;
    lastLine = t;
    lineCount++;
    lines.add(ConsoleLine(t, src, redact(l)));
    try {
      _parse(l.trimLeft(), t);
    } catch (_) {
      // a torn or foreign line
    }
    _changed();
  }

  void _setMode(String id) {
    if (id.isEmpty || id == modeId) return;
    modeId = id;
    if (pendingMode == id) pendingMode = '';
  }

  void _family(BoardFamily f) {
    // a Recon or a legacy wrapper is a stronger claim than a guess from a mode's lines
    if (family == f) return;
    family = f;
    if (chip.isEmpty || chip.startsWith('ESP32-S3')) {
      chip = switch (f) {
        BoardFamily.recon => 'ESP32-S3 (OUI-SPY Recon)',
        BoardFamily.legacyUnified => 'ESP32-S3 (XIAO)',
        BoardFamily.ouispy => fwKind == 'gpswd' ? 'ESP32-S3 (AtomS3)' : 'ESP32-S3 (XIAO)',
        BoardFamily.unknown => chip,
      };
    }
  }

  void _parse(String l, double t) {
    if (l.startsWith('TLM ')) return _tlm(_json(l.substring(4)), t);
    if (l.startsWith('RST ')) return _rst(_json(l.substring(4)));
    if (l.startsWith('RID ')) return _drone(_json(l.substring(4)), t, rid: true);
    if (l.startsWith('{')) {
      final end = l.lastIndexOf('}');
      if (end < 0) return;
      final note = l.substring(end + 1).trim();
      return _jsonLine(_json(l.substring(0, end + 1)), note, t);
    }
    if (l.startsWith('[Ch')) {
      var m = _pcapRe.firstMatch(l);
      if (m != null) return _pcapFrame(m, t);
      m = _bleRe.firstMatch(l);
      if (m != null) return _advert(m, t);
      return;
    }
    if (l.startsWith('BEGIN_DUMP ')) {
      _setMode('detector');
      det.dumping = l.contains(' prev') ? 'prev' : 'live';
      det._buf.clear();
      return;
    }
    if (l.startsWith('END_DUMP ')) {
      final which = l.contains(' prev') ? 'prev' : 'live';
      if (which == 'prev') {
        det.dumpPrev = List.of(det._buf);
        det.dumpPrevTs = t;
      } else {
        det.dumpLive = List.of(det._buf);
        det.dumpLiveTs = t;
      }
      det._buf.clear();
      det.dumping = '';
      return _changed(now: true);
    }
    // foxhunter
    var m = _foxRssi.firstMatch(l) ?? _foxDebug.firstMatch(l);
    if (m != null) {
      _setMode('foxhunter');
      final r = int.parse(m[1]!);
      fox.rssi = r;
      fox.last = t;
      if (fox.state != 'tracking') fox.state = 'tracking';
      fox.history.add(RssiPoint(t, r));
      return;
    }
    if (l.startsWith('TARGET ACQUIRED') || l.startsWith('TARGET REACQUIRED')) {
      _setMode('foxhunter');
      fox.state = 'tracking';
      fox.events.add(FoxEvent(t, l.startsWith('TARGET RE') ? 'reacquired' : 'acquired'));
      return _changed(now: true);
    }
    if (l.startsWith('TARGET LOST')) {
      _setMode('foxhunter');
      fox.state = 'lost';
      fox.rssi = null;
      fox.events.add(FoxEvent(t, 'lost'));
      return _changed(now: true);
    }
    m = _foxTarget.firstMatch(l);
    if (m != null) {
      fox.target = m[1]!.toUpperCase();
      return;
    }
    if (l.contains('STARTING FOXHUNT TRACKING MODE') || l.startsWith('FOXHUNT REALTIME')) {
      _setMode('foxhunter');
      fox.state = 'searching';
      return;
    }
    if (l.contains('STARTING FOXHUNT CONFIG MODE')) {
      _setMode('foxhunter');
      fox.state = 'config';
      return;
    }
    if (l.contains('OUI-SPY FOXHUNT MODE')) return _setMode('foxhunter');
    // axeoff
    if (l.startsWith('[axeoff]') || l.startsWith('=== AXEOFF')) return _setMode('axeoff');
    if (l.startsWith('[state] ')) {
      _setMode('axeoff');
      if (l.contains('-> TRACKING')) axe.state = 'TRACKING';
      if (l.contains('-> IDLE')) axe.state = 'IDLE';
      axe.stateTs = t;
      return _changed(now: true);
    }
    // sky spy, flock-you
    if (l.startsWith('[SKY-SPY]')) return _setMode('skyspy');
    if (l.startsWith('[flockyou]')) return _setMode('flock-wifi');
    // versions
    m = _verRe.firstMatch(l);
    if (m != null) {
      _setMode(switch (m[1]) {
        'PCAP' => 'pcap',
        'BLESNIFF' => 'blesniff',
        _ => 'detector',
      });
      version = m[2]!;
      return _changed(now: true);
    }
    // the legacy wrapper
    m = _legacyBanner.firstMatch(l);
    if (m != null) {
      _family(BoardFamily.legacyUnified);
      firmware = 'Unified Blue v${m[1]}';
      return;
    }
    m = _legacyMode.firstMatch(l) ?? _legacyStart.firstMatch(l);
    if (m != null) {
      _family(BoardFamily.legacyUnified);
      final i = int.parse(m[1]!);
      if (i >= 0 && i < legacyModeIds.length) _setMode(legacyModeIds[i]);
      return _changed(now: true);
    }
    if (l.contains('OUI SPY - Firmware Selector') || l.startsWith('[SELECTOR]')) {
      _family(BoardFamily.legacyUnified);
      return _setMode('selector');
    }
    m = _modeMgr.firstMatch(l);
    if (m != null) return _setMode(m[1]!);
    // Recon console replies
    m = _okBlue.firstMatch(l);
    if (m != null) {
      _family(BoardFamily.recon);
      pendingMode = m[1]!;
      return _changed(now: true);
    }
    m = _okMesh.firstMatch(l);
    if (m != null) {
      meshOn = m[1] == '1';
      meshRole = m[2]!;
      meshCh = int.parse(m[3]!);
      return _changed(now: true);
    }
    if (l.contains('a Recon Blue node') || l.contains('(Recon Blue)')) return _family(BoardFamily.recon);
    if (l.startsWith('[MESH] paused')) {
      meshState = 'paused';
      return;
    }
    if (l.contains('Hardware: Xiao ESP32 S3') && chip.isEmpty) chip = 'ESP32-S3 (XIAO)';
  }

  Map<String, dynamic> _json(String s) => (jsonDecode(s) as Map).cast<String, dynamic>();

  void _tlm(Map<String, dynamic> j, double t) {
    lastTlm = t;
    final kind = _s(j['kind']);
    nodeId = _s(j['id']).toUpperCase();
    if (kind == 'reconblue') {
      _family(BoardFamily.recon);
      fwKind = kind;
      final app = _s(j['app']);
      if (app.isNotEmpty) _setMode(app);
      meshState = _s(j['mstate']);
    } else {
      fwKind = kind.isNotEmpty ? kind : fwKind;
      _family(BoardFamily.ouispy);
      if (fwKind == 'gpswd') chip = 'ESP32-S3 (AtomS3)';
    }
    final role = _s(j['role']);
    // role is solo when the mesh is disabled; mesh:1 says it is up right now
    meshOn = role.isNotEmpty ? role != 'solo' : (_i(j['mesh']) ?? 0) == 1;
    meshRole = role;
    meshCh = _i(j['ch']) ?? meshCh;
    final rate = _s(j['rate']);
    if (rate.isNotEmpty) meshRate = rate;
    _changed(now: true);
  }

  void _rst(Map<String, dynamic> j) {
    final node = _s(j['node']).toUpperCase();
    // RST lines also answer `rstatus` for other nodes: only this board's own counts
    if (nodeId.isNotEmpty && node != nodeId) return;
    final fw = _s(j['fw']);
    if (fw.isEmpty) return;
    nodeId = node;
    firmware = fw;
    final k = kindFromFw(fw);
    if (k == 'reconblue') {
      fwKind = k;
      _family(BoardFamily.recon);
    } else {
      fwKind = k;
      _family(BoardFamily.ouispy);
    }
    meshRole = _s(j['role']).isNotEmpty ? _s(j['role']) : meshRole;
    meshCh = _i(j['ch']) ?? meshCh;
    _changed(now: true);
  }

  void _jsonLine(Map<String, dynamic> j, String note, double t) {
    final ev = _s(j['event']);
    if (ev == 'config') {
      _setMode('flock-wifi');
      flock.beepMask = _i(j['beep_mask']);
      flock.ouiCount = _i(j['oui_count']);
      flock.tiers = [
        for (final x in (j['tiers'] as List? ?? const []))
          if (x is Map) FlockTier(_i(x['tier']) ?? 0, _s(x['method']), (_i(x['beep']) ?? 0) == 1),
      ];
      flock.configTs = t;
      return _changed(now: true);
    }
    if (ev == 'detection') {
      if (j.containsKey('detection_tier')) {
        _setMode('flock-wifi');
        flock.dets.add(FlockDet(t, j));
        return;
      }
      _setMode('detector');
      final d = DetectorDet(t, j);
      if (det.dumping.isNotEmpty) {
        det._buf.add(d);
      } else if (d.replay.isEmpty || d.replay == 'live') {
        det.live.add(d);
      } else {
        det._buf.add(d); // a dump line seen without its BEGIN (opened mid-dump)
      }
      return;
    }
    if (j.containsKey('drone_lat')) return _drone(j, t);
    final mac = _s(j['mac']);
    if (mac.isEmpty) {
      if (j.containsKey('hopmask')) {
        _setMode('pcap');
        pcap.status = j;
        pcap.statusTs = t;
        return _changed(now: true);
      }
      if (j.containsKey('scan_win')) {
        _setMode('blesniff');
        ble.status = j;
        ble.statusTs = t;
        return _changed(now: true);
      }
      if (_s(j['mode']) == 'ble_detector') {
        _setMode('detector');
        det.status = j;
        det.statusTs = t;
        return _changed(now: true);
      }
      if (j.containsKey('status')) {
        _setMode('skyspy');
        sky.status = j;
        sky.statusTs = t;
        if (_s(j['src']) == 'c5') chip = 'ESP32-C5';
        return;
      }
      return;
    }
    // the mesh detection line (Recon in any scanning mode, the mesh detector)
    if (j.containsKey('desc') || j.containsKey('node')) {
      final d = MeshDet(t, j);
      final prev = meshDets.items.take(30).where((x) => x.mac == d.mac && t - x.ts < 3).firstOrNull;
      if (prev == null || j['upd'] == 1) meshDets.add(d);
      return;
    }
    final type = _s(j['type']);
    if (_axeTypes.contains(type)) {
      _setMode('axeoff');
      axe.dets.add(AxeDet(t, mac.toUpperCase(), _i(j['rssi']), type, _s(j['match']), note));
      if (axe.state.isEmpty) axe.state = 'TRACKING';
      return;
    }
    if (j.containsKey('alias') && !j.containsKey('type')) {
      // the Detector's plain hit line; the event line for the same MAC carries more
      _setMode('detector');
      final up = mac.toUpperCase();
      final dup = det.live.items.take(20).any((x) => x.mac == up && t - x.ts < 3);
      if (!dup) det.live.add(DetectorDet(t, j));
      return;
    }
  }

  void _drone(Map<String, dynamic> j, double t, {bool rid = false}) {
    final mac = _s(j['mac']).toUpperCase();
    if (mac.isEmpty) return;
    _setMode('skyspy');
    final d = sky.drones.putIfAbsent(mac, () => SkyDrone(mac)..first = t);
    d.count++;
    d.last = t;
    d.rssi = _i(j['rssi']) ?? d.rssi;
    d.lat = _d(j['drone_lat'] ?? j['lat']) ?? d.lat;
    d.lon = _d(j['drone_long'] ?? j['lon']) ?? d.lon;
    d.alt = _i(j['drone_altitude'] ?? j['alt']) ?? d.alt;
    d.plat = _d(j['pilot_lat'] ?? j['plat']) ?? d.plat;
    d.plon = _d(j['pilot_long'] ?? j['plon']) ?? d.plon;
    final id = _s(j['basic_id'] ?? j['id']);
    if (id.isNotEmpty) d.basicId = id;
    if (j['band'] != null) d.band = _s(j['band']);
    d.channel = _i(j['channel']) ?? d.channel;
    if (j['src'] != null) d.src = _s(j['src']);
    if (d.src == 'c5') chip = 'ESP32-C5';
    if (sky.drones.length > 200) {
      final old = sky.byLast.last;
      sky.drones.remove(old.mac);
    }
    sky.log.add(
      SkyDrone(mac)
        ..rssi = d.rssi
        ..lat = d.lat
        ..lon = d.lon
        ..alt = d.alt
        ..basicId = d.basicId
        ..band = d.band
        ..channel = d.channel
        ..last = t,
    );
  }

  void _pcapFrame(RegExpMatch m, double t) {
    _setMode('pcap');
    final ch = int.parse(m[1]!);
    final f = m[3] != null
        ? PcapFrame(ts: t, ch: ch, rssi: int.parse(m[2]!), type: 'SHORT', len: int.parse(m[3]!))
        : PcapFrame(ts: t, ch: ch, rssi: int.parse(m[2]!), type: m[4]!, sub: m[5]!, src: m[6]!, dst: m[7]!, bssid: m[8]!, len: int.parse(m[9]!), ssid: m[10] ?? '');
    pcap.frames.add(f);
    pcap.total++;
    pcap.byType[f.type] = (pcap.byType[f.type] ?? 0) + 1;
    pcap.byKind[f.kind] = (pcap.byKind[f.kind] ?? 0) + 1;
    pcap.byChan[ch] = (pcap.byChan[ch] ?? 0) + 1;
  }

  void _advert(RegExpMatch m, double t) {
    _setMode('blesniff');
    final a = Advert(
      ts: t,
      ch: int.tryParse(m[1]!),
      rssi: int.parse(m[2]!),
      pdu: m[3]!,
      addrType: m[4]!,
      mac: m[5]!.toUpperCase(),
      name: m[6] ?? '',
      svc: m[7] ?? '',
      mfrId: m[8] == null ? null : int.parse(m[8]!, radix: 16),
      mfrName: m[9] ?? '',
    );
    ble.adverts.add(a);
    ble.total++;
    final d = ble.devices.putIfAbsent(a.mac, () => BleDev(a.mac)..first = t);
    d.count++;
    d.rssi = a.rssi;
    if (a.rssi > d.best) d.best = a.rssi;
    d.last = t;
    d.addrType = a.addrType;
    if (a.name.isNotEmpty) d.name = a.name;
    if (a.svc.isNotEmpty) d.svc = a.svc;
    if (a.mfrId != null) {
      d.mfrId = a.mfrId;
      d.mfrName = a.mfrName;
    }
    if (ble.devices.length > 500) {
      final old = ble.devices.values.reduce((x, y) => x.last <= y.last ? x : y);
      ble.devices.remove(old.mac);
    }
  }
}
