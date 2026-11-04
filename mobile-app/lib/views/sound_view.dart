// SOUND: the Tab5's SOUND page on the phone. Which events make a sound, a test of each,
// the volume, and the find-it beep. Silence all (Alerts, Settings) overrides the lot.
import 'package:flutter/material.dart';

import '../data/sounds.dart';
import '../main.dart';
import '../theme.dart';

class SoundView extends StatefulWidget {
  const SoundView({super.key});
  @override
  State<SoundView> createState() => _SoundViewState();
}

class _SoundViewState extends State<SoundView> {
  final s = App.I.s;
  bool? allowed;

  @override
  void initState() {
    super.initState();
    Sounds.I.notificationsAllowed().then((v) {
      if (mounted) setState(() => allowed = v);
    });
  }

  void _set(void Function() f) {
    setState(f);
    s.save();
    App.I.m.changed();
  }

  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    return ReconPage(
      title: 'Sound',
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) {
          final live = s.alertSound && !m.muteAll;
          return ListView(
            padding: const EdgeInsets.only(bottom: 40),
            children: [
              if (m.muteAll)
                _note(
                  Ph.speakerSlash,
                  C.red,
                  'Silence all is on: nothing sounds, buzzes or notifies until it is off.',
                  action: Btn('Turn off', color: C.red, onTap: () => m.setMuteAll(false)),
                ),
              if (allowed == false)
                _note(
                  Ph.bellSlash,
                  C.amber,
                  'Android is blocking this app\'s notifications, and the sounds play through them. Allow notifications for OUI-SPY Recon in the system settings.',
                ),
              Group(children: [_row(Ph.speakerHigh, 'All sounds', 'Off: nothing on this list sounds', s.alertSound, (v) => s.alertSound = v, master: true)]),
              Group(
                title: 'Events',
                children: [
                  _row(Ph.bellRinging, 'New device', 'A device not heard for 10 minutes', s.sndDetect, (v) => s.sndDetect = v, test: Sfx.detect, enabled: live),
                  _row(Ph.eye, 'Watched device', 'A watch-list device turns up', s.sndWatch, (v) => s.sndWatch = v, test: Sfx.watch, enabled: live),
                  _row(Ph.drone, 'Drone appears', 'First sighting this session', s.sndDrone, (v) => s.sndDrone = v, test: Sfx.drone, enabled: live),
                  _row(Ph.arrowFatLinesDown, 'Node down', 'A node drops off the mesh', s.sndNodeDown, (v) => s.sndNodeDown = v, test: Sfx.nodeDown, enabled: live),
                  _row(Ph.arrowFatLinesUp, 'Node back', 'A node comes back', s.sndNodeUp, (v) => s.sndNodeUp = v, test: Sfx.nodeUp, enabled: live),
                  _row(Ph.crosshair, 'Proximity beep', 'A click with each find-it pulse', s.proxBeep, (v) => s.proxBeep = v, enabled: live),
                ],
              ),
              SectionHead(
                'Volume',
                trailing: Text('${Sounds.I.vol}', style: T.code.copyWith(color: C.amber)),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 6),
                child: Seg<int>(
                  color: C.amber,
                  items: const [(20, '20'), (40, '40'), (60, '60'), (80, '80'), (100, '100')],
                  value: Sounds.I.vol,
                  onChanged: (v) => _set(() => s.volume = v),
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 14, 20, 0),
                child: Text(
                  'The tones are the Tab5\'s, played as notification sounds, so they follow the phone\'s notification volume and Do Not Disturb; '
                  'this volume scales them within it. A node down or back sounds once per 30 s at most, and only plays a jingle. '
                  'A lost node being seen always sounds while sounds are on. The proximity beep is the system click, heard only with touch sounds on.',
                  style: T.meta.copyWith(height: 1.45),
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  Widget _note(IconData i, Color c, String text, {Widget? action}) => Panel(
    edge: c,
    margin: const EdgeInsets.fromLTRB(16, 8, 16, 4),
    padding: const EdgeInsets.all(14),
    child: Row(
      children: [
        Icon(i, size: 18, color: c),
        const SizedBox(width: 10),
        Expanded(
          child: Text(text, style: T.meta.copyWith(color: C.soft, height: 1.4)),
        ),
        if (action != null) ...[const SizedBox(width: 10), action],
      ],
    ),
  );

  Widget _row(IconData i, String t, String sub, bool v, void Function(bool) f, {Sfx? test, bool enabled = true, bool master = false}) {
    final on = v && (enabled || master);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 6, 6, 6),
      child: Row(
        children: [
          IconTile(i, on ? (master ? C.green : C.accent) : C.muted),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  t,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: T.row.copyWith(color: enabled || master ? C.text : C.faint),
                ),
                const SizedBox(height: 2),
                Text(sub, maxLines: 2, overflow: TextOverflow.ellipsis, style: T.meta),
              ],
            ),
          ),
          if (test != null)
            TextButton(
              onPressed: App.I.m.muteAll ? null : () => Sounds.I.test(test),
              style: TextButton.styleFrom(minimumSize: const Size(48, 36), padding: const EdgeInsets.symmetric(horizontal: 8)),
              child: Text(
                'TEST',
                style: T.silk.copyWith(color: App.I.m.muteAll ? C.faint : C.accent, fontWeight: FontWeight.w600),
              ),
            ),
          Switch(value: v, onChanged: (x) => _set(() => f(x))),
        ],
      ),
    );
  }
}
