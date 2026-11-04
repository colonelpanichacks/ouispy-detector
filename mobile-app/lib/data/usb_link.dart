// A node on the phone's USB port (Android, USB OTG): its console as lines, the same
// stream the home dashboard reads (home-dashboard/sources.py).
//
// The XIAO ESP32-S3 is USB-Serial/JTAG (VID 0x303A, CDC ACM, 115200). Its DTR/RTS lines
// drive reset and boot, and setting them one at a time makes a reset pulse (AGENTS.md).
// usb_serial's CDC driver asserts both in one SET_CONTROL_LINE_STATE on open and drops
// both in one on close, so this only ever opens and closes: never setDTR / setRTS.
// Like the dashboard it sends a newline on open and reopens after 12 s of silence (a
// stalled CDC after an abrupt close wakes up that way).
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:usb_serial/usb_serial.dart';

/// A node's console, as lines. USB today; BLE implements the same.
abstract class LineLink {
  String get name;
  bool get open;
  Stream<String> get lines;
  Stream<bool> get state;
  Future<bool> start();
  Future<void> stop();
  Future<void> send(String line);
}

const espVid = 0x303A;

class UsbLink implements LineLink {
  UsbLink(this.device);
  final UsbDevice device;
  UsbPort? _port;
  StreamSubscription<Uint8List>? _sub;
  final _lines = StreamController<String>.broadcast();
  final _state = StreamController<bool>.broadcast();
  final _buf = BytesBuilder(copy: false);
  Timer? _watch;
  DateTime _heard = DateTime.now();
  bool _want = false;

  /// Every OUI-SPY board on the USB port (Espressif's VID).
  static Future<List<UsbDevice>> boards() async {
    if (!Platform.isAndroid) return [];
    try {
      return [
        for (final d in await UsbSerial.listDevices())
          if (d.vid == espVid) d,
      ];
    } catch (_) {
      return [];
    }
  }

  static Stream<UsbEvent>? get events => Platform.isAndroid ? UsbSerial.usbEventStream : null;

  @override
  String get name => device.productName ?? device.deviceName;
  @override
  bool get open => _port != null;
  @override
  Stream<String> get lines => _lines.stream;
  @override
  Stream<bool> get state => _state.stream;

  @override
  Future<bool> start() async {
    _want = true;
    final ok = await _open();
    _watch ??= Timer.periodic(const Duration(seconds: 3), (_) async {
      if (!_want) return;
      if (_port == null || DateTime.now().difference(_heard).inSeconds > 12) {
        await _close();
        await _open();
      }
    });
    return ok;
  }

  Future<bool> _open() async {
    try {
      final p = await device.create(UsbSerial.CDC); // asks the user for USB permission the first time
      if (p == null || !await p.open()) return false;
      await p.setPortParameters(115200, UsbPort.DATABITS_8, UsbPort.STOPBITS_1, UsbPort.PARITY_NONE);
      _port = p;
      _heard = DateTime.now();
      _sub = p.inputStream?.listen(_rx, onDone: () => _close(), onError: (_) => _close());
      await p.write(Uint8List.fromList([10]));
      _state.add(true);
      return true;
    } catch (_) {
      await _close();
      return false;
    }
  }

  void _rx(Uint8List data) {
    _heard = DateTime.now();
    for (final b in data) {
      if (b == 10) {
        final l = utf8.decode(_buf.takeBytes(), allowMalformed: true).trim();
        if (l.isNotEmpty) _lines.add(l);
      } else if (b != 13) {
        _buf.addByte(b);
        if (_buf.length > 65536) _buf.clear(); // no newline in 64 KB: junk
      }
    }
  }

  Future<void> _close() async {
    await _sub?.cancel();
    _sub = null;
    final p = _port;
    _port = null;
    if (p != null) {
      try {
        await p.close();
      } catch (_) {}
      _state.add(false);
    }
  }

  @override
  Future<void> stop() async {
    _want = false;
    _watch?.cancel();
    _watch = null;
    await _close();
  }

  @override
  Future<void> send(String line) async {
    final p = _port;
    if (p == null) return;
    await p.write(Uint8List.fromList(utf8.encode('${line.trim()}\n')));
  }
}
