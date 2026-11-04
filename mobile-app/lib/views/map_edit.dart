// Drawing on the map, as the Tab5 and the dashboard do (marks.cpp / marks.js): pins with a
// name, colour and icon; areas tapped out corner by corner; nodes moved or placed where
// the phone stands. Marks go to the dashboard (POST /api/marks) or, when the app is the
// dashboard, are kept on the phone.
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:geolocator/geolocator.dart';

import '../data/model.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';
import 'fleet_view.dart';

/// The Tab5's pin icons (marks.cpp), drawn with Phosphor.
const markIcons = <String, IconData>{
  'pin': PhF.mapPin,
  'flag': PhF.flag,
  'target': PhF.crosshair,
  'house': PhF.house,
  'car': PhF.car,
  'person': PhF.person,
  'drone': PhF.drone,
  'camera': PhF.camera,
  'warning': PhF.warning,
  'star': PhF.star,
  'camp': PhF.tent,
  'radio': PhF.radio,
};

/// A mark id as the dashboard takes them: 4-24 of [0-9A-Za-z_-].
String newMarkId() => 'p${DateTime.now().millisecondsSinceEpoch.toRadixString(36)}${math.Random().nextInt(1296).toRadixString(36)}';

/// Name, colour and (for pins) icon; save or delete. Returns true when something changed.
Future<bool> editMark(BuildContext context, Map<String, dynamic> mark, {bool isNew = false}) async {
  final name = TextEditingController(text: '${mark['name'] ?? ''}');
  var hue = (mark['hue'] as num?)?.toInt() ?? 190;
  var icon = '${mark['icon'] ?? 'pin'}';
  final pin = mark['kind'] == 'pin';
  final r = await showModalBottomSheet<String>(
    context: context,
    isScrollControlled: true,
    builder: (c) => StatefulBuilder(
      builder: (c, set) => Padding(
        padding: EdgeInsets.fromLTRB(16, 0, 16, 16 + MediaQuery.viewInsetsOf(c).bottom),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(isNew ? (pin ? 'NEW PIN' : 'NEW AREA') : (pin ? 'PIN' : 'AREA'), style: T.label),
            const SizedBox(height: 10),
            TextField(
              controller: name,
              autofocus: isNew,
              maxLength: 40,
              style: T.row,
              decoration: const InputDecoration(labelText: 'Name', counterText: ''),
            ),
            const SizedBox(height: 14),
            Text('COLOUR', style: T.silk),
            const SizedBox(height: 8),
            SizedBox(
              height: 30,
              child: ListView(
                scrollDirection: Axis.horizontal,
                children: [
                  for (var h = 0; h < 360; h += 10)
                    GestureDetector(
                      onTap: () => set(() => hue = h),
                      child: Container(
                        width: 26,
                        margin: const EdgeInsets.only(right: 4),
                        decoration: BoxDecoration(
                          color: C.hue(h),
                          borderRadius: BorderRadius.circular(3),
                          border: Border.all(color: h == hue ? C.text : Colors.transparent, width: 2),
                        ),
                      ),
                    ),
                ],
              ),
            ),
            if (pin) ...[
              const SizedBox(height: 14),
              Text('ICON', style: T.silk),
              const SizedBox(height: 8),
              Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  for (final e in markIcons.entries)
                    GestureDetector(
                      onTap: () => set(() => icon = e.key),
                      child: Container(
                        width: 42,
                        height: 42,
                        decoration: BoxDecoration(
                          borderRadius: BorderRadius.circular(3),
                          border: Border.all(color: e.key == icon ? C.hue(hue) : C.silkLine, width: e.key == icon ? 2 : 1),
                        ),
                        child: Icon(e.value, color: e.key == icon ? C.hue(hue) : C.silk, size: 22),
                      ),
                    ),
                ],
              ),
            ],
            const SizedBox(height: 16),
            Row(
              children: [
                if (!isNew) ...[Btn('Delete', icon: Ph.trash, color: C.red, kind: BtnKind.ghost, onTap: () => Navigator.pop(c, 'delete')), const SizedBox(width: 8)],
                Expanded(
                  child: Btn('Save', icon: Ph.check, kind: BtnKind.filled, expand: true, onTap: () => Navigator.pop(c, 'save')),
                ),
              ],
            ),
          ],
        ),
      ),
    ),
  );
  if (r == null) return false;
  if (r == 'delete') {
    if (!context.mounted || !await confirm(context, 'Delete ${mark['name'] ?? 'this mark'}?', 'It goes from every map.', yes: 'Delete')) return false;
    await App.I.link.addMark({'id': mark['id'], 'delete': true});
    return true;
  }
  final n = name.text.trim();
  await App.I.link.addMark({...mark, 'id': mark['id'] ?? newMarkId(), 'name': n.isEmpty ? (pin ? 'Pin' : 'Area') : n, 'hue': hue, if (pin) 'icon': icon});
  return true;
}

/// Where the phone is (asks for location the first time); null if it can't say.
Future<Position?> phonePosition() async {
  try {
    if (!await Geolocator.isLocationServiceEnabled()) return null;
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) p = await Geolocator.requestPermission();
    if (p == LocationPermission.denied || p == LocationPermission.deniedForever) return null;
    return await Geolocator.getCurrentPosition(
      locationSettings: const LocationSettings(accuracy: LocationAccuracy.high, timeLimit: Duration(seconds: 20)),
    );
  } catch (_) {
    return null;
  }
}

/// A node's card on the map: what it is, and moving, placing, colouring it. [tracking]
/// (a GPS node's trail shown, or null when it has none) adds TRACK; [locked] lights LOCK.
/// Returns move / here / colour / settings / unplace / track / lock.
Future<String?> nodeCard(BuildContext context, FleetNode n, {bool? tracking, bool locked = false}) => showModalBottomSheet<String>(
  context: context,
  builder: (c) => SafeArea(
    child: Padding(
      padding: const EdgeInsets.fromLTRB(4, 0, 4, 12),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
            child: Row(
              children: [
                Dot(C.hue(n.hue), size: 10),
                const SizedBox(width: 10),
                Expanded(child: Text(n.label, style: T.title.copyWith(fontSize: 20))),
                Tag(n.online ? 'Online' : (n.silent ? 'Silent' : 'Offline'), color: n.online ? C.green : (n.silent ? C.amber : C.red)),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 10),
            child: Text('${n.id}  ·  ${n.isHome ? 'home' : n.role}  ·  ${n.kind}', style: T.code),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                Expanded(
                  child: Btn('Move', icon: Ph.arrowsOutCardinal, kind: BtnKind.ghost, color: C.text, expand: true, onTap: () => Navigator.pop(c, 'move')),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Btn('Here', icon: Ph.gpsFix, kind: BtnKind.ghost, color: C.text, expand: true, onTap: () => Navigator.pop(c, 'here')),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Btn('Colour', icon: Ph.palette, kind: BtnKind.ghost, color: C.hue(n.hue), expand: true, onTap: () => Navigator.pop(c, 'colour')),
                ),
              ],
            ),
          ),
          const SizedBox(height: 8),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                Expanded(
                  child: Btn('Settings', icon: Ph.sliders, kind: BtnKind.ghost, color: C.text, expand: true, onTap: () => Navigator.pop(c, 'settings')),
                ),
                const SizedBox(width: 8),
                if (n.hasPos)
                  Expanded(
                    child: Btn('Off the map', icon: Ph.mapPinSimpleArea, kind: BtnKind.ghost, color: C.silk, expand: true, onTap: () => Navigator.pop(c, 'unplace')),
                  ),
              ],
            ),
          ),
          if (n.hasPos) ...[
            const SizedBox(height: 8),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Row(
                children: [
                  if (tracking != null) ...[
                    Expanded(
                      child: Btn(
                        tracking ? 'Track on' : 'Track off',
                        icon: Ph.path,
                        kind: tracking ? BtnKind.tonal : BtnKind.ghost,
                        color: tracking ? C.green : C.silk,
                        expand: true,
                        onTap: () => Navigator.pop(c, 'track'),
                      ),
                    ),
                    const SizedBox(width: 8),
                  ],
                  Expanded(
                    child: Btn(
                      locked ? 'Locked' : 'Lock',
                      icon: locked ? PhF.lockSimple : Ph.lockSimple,
                      kind: locked ? BtnKind.tonal : BtnKind.ghost,
                      color: locked ? C.hot : C.text,
                      expand: true,
                      onTap: () => Navigator.pop(c, 'lock'),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ],
      ),
    ),
  ),
);

/// Runs a node card's choice. 'move' is handed back to the map (the next tap places it).
Future<bool> nodeAction(BuildContext context, FleetNode n, String? a) async {
  switch (a) {
    case 'here':
      final p = await phonePosition();
      if (p == null) {
        App.I.m.toast("the phone can't tell where it is (location off or not allowed)");
        return false;
      }
      await App.I.link.nodeSet(n.id, {'lat': p.latitude, 'lon': p.longitude});
      return true;
    case 'unplace':
      await App.I.link.nodeSet(n.id, {'lat': null, 'lon': null});
      return true;
    case 'colour':
      if (!context.mounted) return false;
      final h = await showModalBottomSheet<int>(
        context: context,
        builder: (c) => SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(18, 0, 18, 18),
            child: Wrap(
              spacing: 12,
              runSpacing: 12,
              children: [
                for (final h in nodeHues)
                  GestureDetector(
                    onTap: () => Navigator.pop(c, h),
                    child: Container(
                      width: 40,
                      height: 40,
                      decoration: BoxDecoration(
                        color: C.hue(h),
                        shape: BoxShape.circle,
                        border: Border.all(color: h == n.hue ? C.text : C.bg, width: 3),
                      ),
                    ),
                  ),
                GestureDetector(
                  onTap: () => Navigator.pop(c, -1),
                  child: Container(
                    width: 40,
                    height: 40,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: Border.all(color: C.silkLine),
                    ),
                    child: const Icon(Ph.x, size: 18, color: C.silk),
                  ),
                ),
              ],
            ),
          ),
        ),
      );
      if (h == null) return false;
      await App.I.link.nodeSet(n.id, {'color': h < 0 ? null : h});
      return true;
    case 'settings':
      if (context.mounted) await Navigator.of(context).push(MaterialPageRoute(builder: (_) => NodeDetail(id: n.id)));
      return false;
  }
  return false;
}

/// A drone's card on the map: what it is, how high and fast, its pilot; LOCK on the drone or
/// the pilot (the map keeps it centred), its track on / off, past FLIGHTS, DETAILS (its page).
/// Returns lockDrone / lockPilot / track / flights / details.
Future<String?> droneCard(BuildContext context, Drone d, {required bool lockedDrone, required bool lockedPilot, required bool tracking}) {
  final m = App.I.m;
  final live = m.now - d.last < droneLiveS;
  final facts = [
    if (d.hagl != null) '${d.hagl!.round()} m AGL' else if (d.alt != null) '${d.alt!.round()} m MSL',
    if (d.spd != null) '${d.spd!.toStringAsFixed(1)} m/s',
    if (d.pilotDist != null) 'pilot ${d.pilotDist} m',
    if (d.rssi != null) '${d.rssi} dBm',
    live ? 'live' : 'seen ${ago(d.last)}',
  ];
  return showModalBottomSheet<String>(
    context: context,
    builder: (c) => SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Icon(PhF.drone, size: 18, color: live ? C.hot : C.soft),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(d.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.title.copyWith(fontSize: 20)),
                ),
                Tag(d.tag, color: C.droneTag(d.tag)),
              ],
            ),
            const SizedBox(height: 4),
            Text([if (d.maker.isNotEmpty) d.maker, d.mac].join('  ·  '), style: T.code),
            const SizedBox(height: 4),
            Text(facts.join('  ·  '), style: T.meta),
            const SizedBox(height: 12),
            ActionStrip([
              (lockedDrone ? 'Locked' : 'Lock drone', lockedDrone ? PhF.lockSimple : Ph.lockSimple, C.hot, d.hasPos ? () => Navigator.pop(c, 'lockDrone') : null),
              (lockedPilot ? 'Locked' : 'Lock pilot', lockedPilot ? PhF.user : Ph.user, C.amber, d.hasPilot ? () => Navigator.pop(c, 'lockPilot') : null),
              (tracking ? 'Track on' : 'Track off', Ph.path, tracking ? C.green : C.text, () => Navigator.pop(c, 'track')),
              ('Flights', Ph.clockCounterClockwise, C.sky, () => Navigator.pop(c, 'flights')),
              ('Details', Ph.info, C.text, () => Navigator.pop(c, 'details')),
            ]),
          ],
        ),
      ),
    ),
  );
}
