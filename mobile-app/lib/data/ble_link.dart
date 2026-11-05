// A node over Bluetooth LE: the same console lines as USB (usb_link.dart), carried by the
// Nordic UART service, so the app is the dashboard with no cable (and on iPhones, which
// can't open USB serial at all). The node side is a GATT server with:
//   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX      6E400002-...  write (without response): lines in, "\n"-terminated
//   TX      6E400003-...  notify: every line the console prints, "\n"-terminated, chunked
// OUI-SPY (src/) and OUI-SPY Recon Blue (the display S3) both get it
// (docs/FIRMWARE-NEEDS.md). Bonded pairing; the fleet key never crosses it.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import 'usb_link.dart';

export 'package:flutter_reactive_ble/flutter_reactive_ble.dart' show BleStatus;

final nusService = Uuid.parse('6E400001-B5A3-F393-E0A9-E50E24DCCA9E');
final nusRx = Uuid.parse('6E400002-B5A3-F393-E0A9-E50E24DCCA9E');
final nusTx = Uuid.parse('6E400003-B5A3-F393-E0A9-E50E24DCCA9E');

final _ble = FlutterReactiveBle();

/// A board found nearby.
class BleBoard {
  BleBoard(this.id, this.name, this.rssi);
  final String id, name;
  final int rssi;
}

/// OUI-SPY boards in range for [wait]: ones offering the console service, or named OUI-SPY.
Future<List<BleBoard>> scanBoards({Duration wait = const Duration(seconds: 6)}) async {
  final found = <String, BleBoard>{};
  StreamSubscription<DiscoveredDevice>? sub;
  try {
    sub = _ble.scanForDevices(withServices: const [], scanMode: ScanMode.lowLatency, requireLocationServicesEnabled: false).listen((d) {
      final named = d.name.toUpperCase().contains('OUI-SPY') || d.name.toUpperCase().contains('OUI SPY') || d.name.toUpperCase().startsWith('RECON');
      if (named || d.serviceUuids.contains(nusService)) found[d.id] = BleBoard(d.id, d.name.isEmpty ? d.id : d.name, d.rssi);
    }, onError: (_) {});
    await Future.delayed(wait);
  } catch (_) {
  } finally {
    await sub?.cancel();
  }
  return found.values.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
}

/// Whether Bluetooth is usable here at all (off, unauthorised or missing: false).
Future<BleStatus> bleStatus() async {
  try {
    return await _ble.statusStream.firstWhere((s) => s != BleStatus.unknown).timeout(const Duration(seconds: 3));
  } catch (_) {
    return BleStatus.unknown;
  }
}

class BleLink implements LineLink {
  BleLink(this.id, this.label);
  final String id, label;
  StreamSubscription<ConnectionStateUpdate>? _conn;
  StreamSubscription<List<int>>? _rx;
  final _lines = StreamController<String>.broadcast();
  final _state = StreamController<bool>.broadcast();
  final _buf = <int>[];
  bool _open = false, _want = false;
  int _mtu = 23;
  Timer? _retry;

  @override
  String get name => label;
  @override
  bool get open => _open;
  @override
  Stream<String> get lines => _lines.stream;
  @override
  Stream<bool> get state => _state.stream;

  QualifiedCharacteristic _c(Uuid c) => QualifiedCharacteristic(serviceId: nusService, characteristicId: c, deviceId: id);

  @override
  Future<bool> start() async {
    _want = true;
    final done = Completer<bool>();
    _conn = _ble
        .connectToDevice(
          id: id,
          servicesWithCharacteristicsToDiscover: {
            nusService: [nusRx, nusTx],
          },
          connectionTimeout: const Duration(seconds: 15),
        )
        .listen(
          (u) async {
            if (u.connectionState == DeviceConnectionState.connected) {
              try {
                if (Platform.isAndroid) _mtu = await _ble.requestMtu(deviceId: id, mtu: 247);
                _rx = _ble.subscribeToCharacteristic(_c(nusTx)).listen(_bytes, onError: (_) => _drop());
                _open = true;
                _state.add(true);
                await send(''); // a newline, as on USB: wakes a console that went quiet
                if (!done.isCompleted) done.complete(true);
              } catch (_) {
                _drop();
                if (!done.isCompleted) done.complete(false);
              }
            } else if (u.connectionState == DeviceConnectionState.disconnected) {
              _drop();
              if (!done.isCompleted) done.complete(false);
            }
          },
          onError: (_) {
            _drop();
            if (!done.isCompleted) done.complete(false);
          },
        );
    return done.future;
  }

  void _bytes(List<int> data) {
    for (final b in data) {
      if (b == 10) {
        final l = utf8.decode(_buf, allowMalformed: true).trim();
        _buf.clear();
        if (l.isNotEmpty) _lines.add(l);
      } else if (b != 13) {
        _buf.add(b);
        if (_buf.length > 65536) _buf.clear();
      }
    }
  }

  void _drop() {
    final was = _open;
    _open = false;
    _rx?.cancel();
    _rx = null;
    if (was) _state.add(false);
    // reconnect while wanted: the board went out of range or rebooted
    if (_want) {
      _retry?.cancel();
      _retry = Timer(const Duration(seconds: 5), () async {
        if (!_want || _open) return;
        await _conn?.cancel();
        await start();
      });
    }
  }

  @override
  Future<void> stop() async {
    _want = false;
    _retry?.cancel();
    await _rx?.cancel();
    await _conn?.cancel();
    _open = false;
  }

  @override
  Future<void> send(String line) async {
    if (!_open) return;
    final b = utf8.encode('${line.trim()}\n');
    final chunk = (_mtu - 3).clamp(20, 244);
    for (var i = 0; i < b.length; i += chunk) {
      final part = b.sublist(i, i + chunk > b.length ? b.length : i + chunk);
      try {
        await _ble.writeCharacteristicWithoutResponse(_c(nusRx), value: part);
      } catch (_) {
        await _ble.writeCharacteristicWithResponse(_c(nusRx), value: part);
      }
    }
  }
}
