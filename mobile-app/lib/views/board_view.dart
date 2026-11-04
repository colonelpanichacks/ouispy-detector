// The board on the other end of the console: what it is (family, firmware, the Unified Blue
// mode, its mesh) and the live panel of the mode it runs, with that mode's controls.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/blue_modes.dart';
import '../theme.dart';
import 'common.dart';

class BoardView extends StatelessWidget {
  const BoardView({super.key});

  @override
  Widget build(BuildContext context) {
    final b = Board.I;
    return Scaffold(
      appBar: AppBar(
        titleSpacing: 0,
        title: Text('BOARD', style: T.label.copyWith(fontSize: 12, color: C.soft)),
      ),
      body: ListenableBuilder(
        listenable: b,
        builder: (context, _) => ListView(
          padding: const EdgeInsets.only(bottom: 30),
          children: [_IdCard(b), if (b.family == BoardFamily.recon) ..._recon(context, b), ..._modePanel(context, b)],
        ),
      ),
    );
  }

  List<Widget> _modePanel(BuildContext context, Board b) {
    final out = <Widget>[
      ...switch (b.modeId) {
        'foxhunter' => [_Fox(b)],
        'pcap' => [_Pcap(b)],
        'blesniff' => [_BleSniff(b)],
        'flock-wifi' => [_Flock(b)],
        'detector' => [_Detector(b)],
        'axeoff' => _axeoff(context, b),
        'skyspy' => _skyspy(context, b),
        _ => const <Widget>[],
      },
    ];
    if (b.meshDets.isNotEmpty || b.family == BoardFamily.ouispy || b.modeId == 'mesh') out.addAll(_meshFeed(context, b));
    if (!b.recognised || b.modeId == 'selector' || out.isEmpty) out.add(_Unknown(b));
    return out;
  }
}

// ------------------------------------------------------------------ shared bits

// Console times are this phone's clock (or the dashboard's), so no model offset is needed.
String _ago(double ts) {
  if (ts <= 0) return '-';
  final s = (DateTime.now().millisecondsSinceEpoch / 1000 - ts).round();
  if (s < 5) return 'just now';
  if (s < 60) return '${s}s ago';
  if (s < 3600) return '${s ~/ 60}m ago';
  return '${s ~/ 3600}h ${(s % 3600) ~/ 60}m ago';
}

class _Row extends StatelessWidget {
  const _Row({required this.title, this.sub = '', this.rssi, this.ts = 0, this.onTap, this.color = C.muted, this.icon});
  final String title, sub;
  final int? rssi;
  final double ts;
  final VoidCallback? onTap;
  final Color color;
  final IconData? icon;
  @override
  Widget build(BuildContext context) => InkWell(
    onTap: onTap,
    child: Padding(
      padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
      child: Row(
        children: [
          if (icon != null) ...[IconTile(icon!, color, size: 32), const SizedBox(width: 12)],
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(title, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.row.copyWith(fontSize: 14)),
                if (sub.isNotEmpty) ...[const SizedBox(height: 3), Text(sub, maxLines: 2, overflow: TextOverflow.ellipsis, style: T.code.copyWith(fontSize: 11.5))],
              ],
            ),
          ),
          const SizedBox(width: 8),
          Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              if (ts > 0) Text(clock(ts), style: T.code.copyWith(fontSize: 11)),
              if (rssi != null) ...[
                const SizedBox(height: 4),
                Row(
                  children: [
                    Text(
                      '$rssi',
                      style: T.code.copyWith(color: C.rssi(rssi), fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(width: 4),
                    RssiBar(rssi),
                  ],
                ),
              ],
            ],
          ),
        ],
      ),
    ),
  );
}

Widget _empty(String title, String body) => Panel(child: Text('$title. $body', style: T.meta.copyWith(fontSize: 13, height: 1.45)));

Widget _countWrap(Map<Object, int> m, {Color color = C.silk}) {
  final e = m.entries.toList()..sort((a, b) => b.value.compareTo(a.value));
  return Padding(
    padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
    child: Wrap(spacing: 6, runSpacing: 6, children: [for (final x in e) Tag('${x.key}  ${x.value}', color: color)]),
  );
}

Future<int?> _askInt(BuildContext context, String title, int? cur, String hint) async {
  final v = await askText(context, title, cur == null ? '' : '$cur', hint: hint);
  return v == null ? null : int.tryParse(v);
}

Widget _btns(List<Widget> bs) => Padding(
  padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
  child: Wrap(spacing: 8, runSpacing: 8, children: bs),
);

// ------------------------------------------------------------------ identity

class _IdCard extends StatelessWidget {
  const _IdCard(this.b);
  final Board b;
  @override
  Widget build(BuildContext context) {
    final mesh = b.meshOn == null
        ? 'unknown'
        : b.meshOn!
        ? [
            'on',
            if (b.meshRole.isNotEmpty) b.meshRole,
            if (b.meshCh != null) 'ch ${b.meshCh}',
            if (b.meshRate.isNotEmpty && b.meshRate != 'off') b.meshRate,
            if (b.meshState.isNotEmpty) b.meshState,
          ].join(' · ')
        : 'off';
    final c = b.recognised ? C.accent : C.muted;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(b.recognised ? b.summary : 'Not recognised yet', style: T.title.copyWith(fontSize: 22)),
              const SizedBox(height: 4),
              Text(
                !b.attached
                    ? 'No console attached.'
                    : b.lineCount == 0
                    ? 'Nothing heard on the console yet.'
                    : '${b.lineCount} lines, last ${_ago(b.lastLine)}.',
                style: T.meta.copyWith(fontSize: 13),
              ),
              const SizedBox(height: 10),
              Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  Tag(familyName(b.family), color: c),
                  if (b.chip.isNotEmpty) Tag(b.chip),
                  if (b.modeId.isNotEmpty) Tag(b.modeLabel, color: C.cyan),
                  if (b.pendingMode.isNotEmpty) Tag('Rebooting into ${modeName(b.pendingMode)}', color: C.amber),
                ],
              ),
            ],
          ),
        ),
        Group(
          title: 'Identity',
          trailing: b.probing ? const SizedBox(width: 12, height: 12, child: CircularProgressIndicator(strokeWidth: 1.5)) : null,
          children: [
            KV('Firmware', b.firmware.isEmpty ? '-' : b.firmware, monoValue: true),
            if (b.version.isNotEmpty) KV('Mode version', b.version, monoValue: true),
            KV('Mode', b.modeId.isEmpty ? '-' : '${b.modeLabel}  (${b.modeId})'),
            if (b.family == BoardFamily.ouispy) KV('Build kind', ouispyKindName(b.fwKind)),
            if (b.nodeId.isNotEmpty) KV('Node', b.nodeId, monoValue: true),
            KV('Mesh', mesh, color: b.meshOn == true ? C.green : C.text),
          ],
        ),
        _btns([Btn('Identify again', icon: Ph.magnifyingGlass, onTap: b.attached && !b.probing ? b.probe : null)]),
      ],
    );
  }
}

// ------------------------------------------------------------------ Recon

List<Widget> _recon(BuildContext context, Board b) => [
  if (b.meshOn == false)
    Panel(
      edge: C.amber,
      margin: const EdgeInsets.fromLTRB(16, 8, 16, 4),
      child: Text(
        'The mesh is off on this board. Detections reach the app only when USB FEED is turned on in the board\'s settings, or when the mesh is on.',
        style: T.body.copyWith(fontSize: 13.5),
      ),
    ),
  Group(
    title: 'Switch mode',
    children: [
      for (final (id, name, what) in blueModes)
        Tile(
          title: name,
          subtitle: Text(what, style: T.meta),
          titleColor: id == b.modeId ? C.accent : null,
          trailing: id == b.modeId ? const Tag('Running', color: C.accent) : const Icon(Ph.caretRight, size: 15, color: C.silkMark),
          onTap: id == b.modeId
              ? null
              : () async {
                  final ok = await confirm(
                    context,
                    'Switch to $name?',
                    'The board saves the mode and reboots into it. The USB link drops for a few seconds while it restarts.',
                    yes: 'Switch and reboot',
                    color: C.accent,
                  );
                  if (ok) await b.switchMode(id);
                },
        ),
    ],
  ),
];

// ------------------------------------------------------------------ Foxhunter

class _Fox extends StatelessWidget {
  const _Fox(this.b);
  final Board b;

  static String hint(int? r) {
    if (r == null) return 'No signal';
    if (r >= -50) return 'Very close';
    if (r >= -65) return 'Close';
    if (r >= -80) return 'Nearby';
    return 'Far';
  }

  @override
  Widget build(BuildContext context) {
    final f = b.fox;
    final live = f.state == 'tracking' && f.rssi != null;
    final sc = switch (f.state) {
      'tracking' => C.green,
      'lost' => C.red,
      'config' => C.amber,
      _ => C.muted,
    };
    final pts = f.history.oldestFirst;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionHead('Foxhunter', trailing: Tag(f.state.isEmpty ? 'waiting' : f.state, color: sc)),
        Panel(
          edge: live ? C.rssi(f.rssi) : null,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('TARGET', style: T.silk),
              const SizedBox(height: 4),
              Text(f.target.isEmpty ? 'Not known (set it on the board\'s AP)' : f.target, style: T.code.copyWith(fontSize: 13.5, color: C.soft)),
              const SizedBox(height: 14),
              Row(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Text(live ? '${f.rssi}' : '--', style: T.num.copyWith(fontSize: 64, color: live ? C.rssi(f.rssi) : C.faint, height: 1)),
                  const SizedBox(width: 6),
                  Padding(
                    padding: const EdgeInsets.only(bottom: 8),
                    child: Text('dBm', style: T.meta),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.end,
                      children: [
                        Text(
                          live ? hint(f.rssi) : (f.state == 'lost' ? 'Target lost' : 'Searching'),
                          textAlign: TextAlign.end,
                          style: T.h2.copyWith(color: live ? C.rssi(f.rssi) : C.muted),
                        ),
                        if (f.last > 0) Text(_ago(f.last), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta),
                      ],
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 14),
              SizedBox(
                height: 90,
                child: CustomPaint(painter: _Spark(pts.length > 150 ? pts.sublist(pts.length - 150) : pts), size: Size.infinite),
              ),
            ],
          ),
        ),
        if (f.events.isNotEmpty)
          Group(
            title: 'Events',
            children: [
              for (final e in f.events.items.take(10))
                _Row(
                  title: switch (e.what) {
                    'acquired' => 'Target acquired',
                    'reacquired' => 'Target reacquired',
                    _ => 'Target lost',
                  },
                  ts: e.ts,
                  icon: e.what == 'lost' ? Ph.eyeSlash : Ph.crosshair,
                  color: e.what == 'lost' ? C.red : C.green,
                ),
            ],
          ),
      ],
    );
  }
}

/// RSSI over time as bars, -100 dBm at the floor, -30 at the top.
class _Spark extends CustomPainter {
  _Spark(this.pts);
  final List<RssiPoint> pts;
  @override
  void paint(Canvas canvas, Size s) {
    final grid = Paint()
      ..color = C.silkLine
      ..strokeWidth = 1;
    for (final db in [-90, -70, -50]) {
      final y = s.height * (1 - (db + 100) / 70);
      canvas.drawLine(Offset(0, y), Offset(s.width, y), grid);
    }
    if (pts.isEmpty) return;
    const n = 150;
    final w = s.width / n;
    final start = n - pts.length;
    for (final (i, p) in pts.indexed) {
      final v = ((p.rssi + 100) / 70).clamp(0.02, 1.0);
      final h = s.height * v;
      final x = (start + i) * w;
      canvas.drawRect(Rect.fromLTWH(x, s.height - h, math.max(1, w - 1), h), Paint()..color = C.rssi(p.rssi).withValues(alpha: 0.85));
    }
  }

  @override
  bool shouldRepaint(_Spark o) => true;
}

// ------------------------------------------------------------------ PCAP

class _Pcap extends StatefulWidget {
  const _Pcap(this.b);
  final Board b;
  @override
  State<_Pcap> createState() => _PcapState();
}

class _PcapState extends State<_Pcap> {
  Set<int>? pick;

  @override
  Widget build(BuildContext context) {
    final b = widget.b;
    final p = b.pcap;
    final sel = pick ?? (p.hopMask == null ? <int>{} : BlueCmd.channelsOf(p.hopMask!));
    final st = p.status;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionHead('PCAP', trailing: Text('${p.total} frames', style: T.code)),
        if (st.isNotEmpty)
          Group(
            children: [
              KV('Radio', p.hopping ? 'Hopping, now ch ${st['chan']}' : 'Locked on ch ${st['chan']}'),
              KV('Hop mask', '${st['hopmask']}  (${BlueCmd.channelsOf(p.hopMask ?? 0).join(', ')})', monoValue: true),
              KV('Dwell', '${st['dwell']} ms'),
              KV('Frames', '${st['total']}  ·  ${st['pps']}/s  ·  dropped ${st['drop_pcap']}'),
            ],
          ),
        Padding(
          padding: const EdgeInsets.fromLTRB(18, 8, 16, 6),
          child: Text('HOP CHANNELS', style: T.silk),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
          child: Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              for (var c = 1; c <= 13; c++)
                Chip2(
                  '$c',
                  on: sel.contains(c),
                  onTap: () => setState(() {
                    final n = {...sel};
                    n.contains(c) ? n.remove(c) : n.add(c);
                    pick = n;
                  }),
                ),
            ],
          ),
        ),
        _btns([
          Btn(
            'Hop ${sel.length} ch  (0x${BlueCmd.hopMaskOf(sel).toRadixString(16).padLeft(4, '0')})',
            icon: Ph.shuffle,
            onTap: sel.isEmpty ? null : () => b.pcapHop(sel).then((_) => b.modeStatus()),
          ),
          Btn(
            'Lock channel',
            icon: Ph.lock,
            kind: BtnKind.ghost,
            onTap: () async {
              final ch = await _askInt(context, 'Lock on channel', p.channel, '1-14');
              if (ch != null) await b.pcapChannel(ch).then((_) => b.modeStatus());
            },
          ),
          Btn(
            'Dwell',
            icon: Ph.timer,
            kind: BtnKind.ghost,
            onTap: () async {
              final ms = await _askInt(context, 'Dwell per channel (ms)', p.dwell, '100-2000');
              if (ms != null) await b.pcapDwell(ms).then((_) => b.modeStatus());
            },
          ),
          Btn('Status', icon: Ph.info, kind: BtnKind.ghost, onTap: b.modeStatus),
        ]),
        if (p.byType.isNotEmpty) ...[const SectionHead('By type'), _countWrap(p.byKind, color: C.sky)],
        if (p.byChan.isNotEmpty) ...[
          const SectionHead('By channel'),
          _countWrap({for (final e in p.byChan.entries) 'ch ${e.key}': e.value}),
        ],
        const SectionHead('Frames'),
        if (p.frames.isEmpty)
          _empty('No frames yet', 'The board prints a line for each frame it captures.')
        else
          Panel(
            padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                for (final f in p.frames.items.take(80))
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2.5),
                    child: Text.rich(
                      TextSpan(
                        children: [
                          TextSpan(
                            text: '${clock(f.ts)} ',
                            style: const TextStyle(color: C.faint),
                          ),
                          TextSpan(
                            text: 'ch${f.ch.toString().padRight(3)}',
                            style: const TextStyle(color: C.silk),
                          ),
                          TextSpan(
                            text: '${f.rssi} ',
                            style: TextStyle(color: C.rssi(f.rssi)),
                          ),
                          TextSpan(
                            text: f.kind,
                            style: const TextStyle(color: C.sky),
                          ),
                          if (f.src.isNotEmpty)
                            TextSpan(
                              text: '  ${f.src} > ${f.dst}',
                              style: const TextStyle(color: C.soft),
                            ),
                          TextSpan(text: '  ${f.len}B'),
                          if (f.ssid.isNotEmpty)
                            TextSpan(
                              text: '  "${f.ssid}"',
                              style: const TextStyle(color: C.text),
                            ),
                        ],
                      ),
                      style: T.code.copyWith(fontSize: 11),
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
  }
}

// ------------------------------------------------------------------ BLE Sniff

class _BleSniff extends StatelessWidget {
  const _BleSniff(this.b);
  final Board b;
  @override
  Widget build(BuildContext context) {
    final s = b.ble;
    final st = s.status;
    final devs = s.byLast;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionHead('BLE Sniff', trailing: Text('${s.total} adverts', style: T.code)),
        if (st.isNotEmpty)
          Group(
            children: [
              KV('Scan', 'window ${st['scan_win']} ms  ·  interval ${st['scan_int']} ms'),
              KV('Adverts', '${st['total']}  ·  ${st['pps']}/s  ·  dropped ${st['drop_pcap']}'),
              if (st['state'] != null) KV('Session', '${st['state']}  ·  ${st['session_bytes']} bytes'),
            ],
          ),
        _btns([
          Btn(
            'Window',
            icon: Ph.timer,
            kind: BtnKind.ghost,
            onTap: () async {
              final v = await _askInt(context, 'Scan window (ms)', s.window, '10-2000');
              if (v != null) await b.bleWindow(v).then((_) => b.modeStatus());
            },
          ),
          Btn(
            'Interval',
            icon: Ph.clockClockwise,
            kind: BtnKind.ghost,
            onTap: () async {
              final v = await _askInt(context, 'Scan interval (ms)', s.interval, '20-4000');
              if (v != null) await b.bleInterval(v).then((_) => b.modeStatus());
            },
          ),
          Btn('Status', icon: Ph.info, kind: BtnKind.ghost, onTap: b.modeStatus),
        ]),
        SectionHead('Devices', trailing: Text('${devs.length}', style: T.code)),
        if (devs.isEmpty)
          _empty('No adverts yet', 'Each advert the board hears shows up here, grouped by address.')
        else
          Group(
            children: [
              for (final d in devs.take(40))
                _Row(
                  title: d.name.isNotEmpty ? d.name : (d.mfrName.isNotEmpty ? d.mfrName : d.mac),
                  sub: '${d.mac}  ${d.addrType}  ×${d.count}  best ${d.best}${d.svc.isNotEmpty ? '  svc ${d.svc}' : ''}',
                  rssi: d.rssi,
                  ts: d.last,
                  icon: Ph.bluetooth,
                  color: C.violet,
                  onTap: () => openDevice(context, d.mac),
                ),
            ],
          ),
        if (s.adverts.isNotEmpty) ...[
          const SectionHead('Adverts'),
          Panel(
            padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                for (final a in s.adverts.items.take(60))
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2.5),
                    child: Text.rich(
                      TextSpan(
                        children: [
                          TextSpan(
                            text: '${clock(a.ts)} ',
                            style: const TextStyle(color: C.faint),
                          ),
                          TextSpan(
                            text: 'ch${a.ch ?? '?'} ',
                            style: const TextStyle(color: C.silk),
                          ),
                          TextSpan(
                            text: '${a.rssi} ',
                            style: TextStyle(color: C.rssi(a.rssi)),
                          ),
                          TextSpan(
                            text: a.pdu,
                            style: const TextStyle(color: C.violet),
                          ),
                          TextSpan(
                            text: '  ${a.mac}',
                            style: const TextStyle(color: C.soft),
                          ),
                          if (a.name.isNotEmpty)
                            TextSpan(
                              text: '  "${a.name}"',
                              style: const TextStyle(color: C.text),
                            ),
                          if (a.mfrName.isNotEmpty) TextSpan(text: '  ${a.mfrName}'),
                        ],
                      ),
                      style: T.code.copyWith(fontSize: 11),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ],
    );
  }
}

// ------------------------------------------------------------------ Flock-You

class _Flock extends StatelessWidget {
  const _Flock(this.b);
  final Board b;
  @override
  Widget build(BuildContext context) {
    final f = b.flock;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionHead('Flock-You', trailing: Text('${f.dets.length} detections', style: T.code)),
        Group(
          title: 'Beep per tier',
          trailing: GestureDetector(
            onTap: b.flockConfig,
            child: Text('READ', style: T.silk.copyWith(color: C.accent)),
          ),
          children: [
            if (f.beepMask == null)
              Padding(
                padding: const EdgeInsets.all(16),
                child: Text('Tap READ to load the board\'s beep settings.', style: T.meta),
              ),
            if (f.beepMask != null)
              for (var t = 0; t < flockTierNames.length; t++)
                SwitchListTile(
                  dense: true,
                  title: Text('Tier $t · ${flockTierNames[t]}', style: T.row.copyWith(fontSize: 14)),
                  subtitle: Text(f.tiers.where((x) => x.tier == t).firstOrNull?.method ?? '', style: T.code.copyWith(fontSize: 11)),
                  value: f.tierBeeps(t),
                  onChanged: (v) => b.flockSetBeep(t, v),
                ),
          ],
        ),
        const SectionHead('Detections'),
        if (f.dets.isEmpty)
          _empty('Nothing yet', 'Flock cameras and their kin show up here as the board hears them.')
        else
          Group(
            children: [
              for (final d in f.dets.items.take(60))
                _Row(
                  title: d.ssid.isNotEmpty ? d.ssid : (d.name.isNotEmpty ? d.name : d.mac),
                  sub: '${d.mac}  ${d.method}  tier ${d.tier ?? '-'}${d.ble ? '' : '  ch ${d.channel}'}',
                  rssi: d.rssi,
                  ts: d.ts,
                  icon: d.ble ? Ph.bluetooth : Ph.wifiHigh,
                  color: d.ble ? C.violet : C.sky,
                  onTap: () => openDevice(context, d.mac),
                ),
            ],
          ),
      ],
    );
  }
}

// ------------------------------------------------------------------ Detector

class _Detector extends StatelessWidget {
  const _Detector(this.b);
  final Board b;

  Widget _det(BuildContext context, DetectorDet d) => _Row(
    title: d.alias.isNotEmpty ? d.alias : (d.name.isNotEmpty ? d.name : d.mac),
    sub: [d.mac, if (d.method.isNotEmpty) d.method, if (d.signature.isNotEmpty) d.signature, if (d.hits != null) '×${d.hits}'].join('  '),
    rssi: d.rssi,
    ts: d.ts,
    icon: Ph.bluetooth,
    color: C.violet,
    onTap: () => openDevice(context, d.mac),
  );

  @override
  Widget build(BuildContext context) {
    final s = b.det;
    final st = s.status;
    Future<void> clear(bool prev) async {
      final ok = await confirm(
        context,
        prev ? 'Clear the previous session?' : 'Clear live detections?',
        prev ? 'The board deletes the saved previous session from its flash.' : 'The board forgets the detections of this session.',
        yes: 'Clear',
      );
      if (ok) await b.detClear(prev: prev);
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionHead('Detector', trailing: s.dumping.isNotEmpty ? Tag('Dumping ${s.dumping}', color: C.amber) : null),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: ActionStrip([
            ('Dump live', Ph.downloadSimple, C.accent, () => b.detDump()),
            ('Dump prev', Ph.clockCounterClockwise, C.accent, () => b.detDump(prev: true)),
            ('Clear live', Ph.trash, C.red, () => clear(false)),
            ('Clear prev', Ph.trash, C.red, () => clear(true)),
            ('Status', Ph.info, C.text, b.modeStatus),
          ]),
        ),
        if (st.isNotEmpty)
          Group(
            children: [
              KV('Live', '${st['live_count']} devices'),
              KV('Previous', st['prev_exists'] == true ? '${st['prev_bytes']} bytes saved' : 'none'),
              if (st['uptime_s'] != null) KV('Up', dur(st['uptime_s'] as num)),
            ],
          ),
        const SectionHead('Live'),
        if (s.live.isEmpty)
          _empty('No detections yet', 'The board prints a line the first time it sees a device on its list.')
        else
          Group(children: [for (final d in s.live.items.take(60)) _det(context, d)]),
        if (s.dumpLive.isNotEmpty) Group(title: 'Dump · live · ${clock(s.dumpLiveTs)}', children: [for (final d in s.dumpLive.take(100)) _det(context, d)]),
        if (s.dumpPrev.isNotEmpty) Group(title: 'Dump · previous · ${clock(s.dumpPrevTs)}', children: [for (final d in s.dumpPrev.take(100)) _det(context, d)]),
      ],
    );
  }
}

// ------------------------------------------------------------------ AXEOFF, Sky Spy, the feed

List<Widget> _axeoff(BuildContext context, Board b) {
  final a = b.axe;
  return [
    SectionHead('AXEOFF', trailing: Tag(a.state.isEmpty ? 'idle' : a.state, color: a.state == 'TRACKING' ? C.red : C.muted)),
    if (a.dets.isEmpty)
      _empty('No Axon gear heard', 'Body cams, tasers and in-car units show up here.')
    else
      Group(
        children: [
          for (final d in a.dets.items.take(60))
            _Row(
              title: d.note.isNotEmpty ? d.note : d.type,
              sub: '${d.mac}  ${d.type}  ${d.match}',
              rssi: d.rssi,
              ts: d.ts,
              icon: d.ble ? Ph.bluetooth : Ph.wifiHigh,
              color: C.red,
              onTap: () => openDevice(context, d.mac),
            ),
        ],
      ),
  ];
}

List<Widget> _skyspy(BuildContext context, Board b) {
  final s = b.sky;
  final drones = s.byLast;
  return [
    SectionHead('Sky Spy', trailing: s.statusTs > 0 ? Text('${s.status['status'] ?? ''} · ${_ago(s.statusTs)}', style: T.code) : null),
    if (drones.isEmpty)
      _empty('No drones', 'Remote ID broadcasts show up here.')
    else
      Group(
        children: [
          for (final d in drones.take(40))
            _Row(
              title: d.basicId.isNotEmpty ? d.basicId : d.mac,
              sub: [
                d.mac,
                if (d.band.isNotEmpty) '${d.band}${d.channel != null ? ' ch ${d.channel}' : ''}',
                if (d.hasPos) '${d.lat!.toStringAsFixed(5)}, ${d.lon!.toStringAsFixed(5)}',
                if (d.alt != null) '${d.alt} m',
                '×${d.count}',
              ].join('  '),
              rssi: d.rssi,
              ts: d.last,
              icon: Ph.drone,
              color: C.hot,
            ),
        ],
      ),
  ];
}

List<Widget> _meshFeed(BuildContext context, Board b) => [
  SectionHead(b.family == BoardFamily.ouispy ? 'Detections' : 'Detections on the feed', trailing: Text('${b.meshDets.length}', style: T.code)),
  if (b.meshDets.isEmpty)
    _empty('Nothing yet', 'Detections this board prints show up here.')
  else
    Group(
      children: [
        for (final d in b.meshDets.items.take(60))
          _Row(
            title: d.desc.isNotEmpty ? d.desc : d.mac,
            sub: '${d.mac}  ${d.type}  ${d.node}',
            rssi: d.rssi,
            ts: d.ts,
            icon: d.dom == 'wifi' ? Ph.wifiHigh : Ph.bluetooth,
            color: C.dom(d.dom),
            onTap: () => openDevice(context, d.mac),
          ),
      ],
    ),
];

class _Unknown extends StatelessWidget {
  const _Unknown(this.b);
  final Board b;
  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      Padding(
        padding: const EdgeInsets.fromLTRB(20, 12, 20, 0),
        child: Text(
          b.modeId == 'selector'
              ? 'The board is in its mode selector. Join its Wi-Fi and pick a mode there; this board has no serial mode switch.'
              : 'Tap Identify again to ask the board what it is. Quiet modes (Foxhunter, Sky Spy, AXEOFF) are recognised once they print a detection.',
          style: T.body.copyWith(fontSize: 13.5),
        ),
      ),
      const SectionHead('Console'),
      if (b.lines.isEmpty)
        _empty('Silent', 'No lines from the board yet.')
      else
        Panel(
          padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (final l in b.lines.items.take(50))
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 2),
                  child: Text('${clock(l.ts)}  ${l.line}', style: T.code.copyWith(fontSize: 11)),
                ),
            ],
          ),
        ),
    ],
  );
}
