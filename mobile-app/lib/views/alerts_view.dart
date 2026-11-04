// ALERTS: the Tab5's alert bar as a page. A live detection of a device not heard for
// 10 minutes is an alert (unless muted); acknowledge them, silence everything, or mute one.
import 'package:flutter/material.dart';

import '../main.dart';
import '../theme.dart';
import 'common.dart';

class AlertsView extends StatelessWidget {
  const AlertsView({super.key});
  @override
  Widget build(BuildContext context) {
    final m = App.I.m;
    final st = App.I.s;
    void flip(void Function() f) {
      f();
      st.save();
      m.changed();
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('Alerts'),
        actions: [
          if (m.alertsUnacked > 0)
            TextButton(
              onPressed: m.ackAlerts,
              child: const Text(
                'Dismiss all',
                style: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
              ),
            ),
        ],
      ),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) => Column(
          children: [
            ChipRow(
              children: [
                Chip2(
                  m.muteAll ? 'Silenced' : 'Silence all',
                  icon: m.muteAll ? Ph.speakerSlash : Ph.speakerHigh,
                  on: m.muteAll,
                  color: C.red,
                  onTap: () => m.setMuteAll(!m.muteAll),
                ),
                Chip2('Vibrate', icon: Ph.vibrate, on: st.vibrate, onTap: () => flip(() => st.vibrate = !st.vibrate)),
                Chip2('Notify', icon: Ph.bell, on: st.notify, onTap: () => flip(() => st.notify = !st.notify)),
              ],
            ),
            if (m.muted.isNotEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 0, 20, 6),
                child: Row(
                  children: [
                    const Icon(Ph.bellSlash, size: 14, color: C.muted),
                    const SizedBox(width: 6),
                    Text('${m.muted.length} muted ${m.muted.length == 1 ? 'device' : 'devices'}, listed in Settings', style: T.meta),
                  ],
                ),
              ),
            Expanded(
              child: m.alerts.isEmpty
                  ? const Empty('No alerts', body: 'A device the mesh has not heard for 10 minutes raises one when it shows up again.', icon: Ph.bell, color: C.green)
                  : ListView.separated(
                      itemCount: m.alerts.length,
                      separatorBuilder: (_, _) => const Divider(indent: 64),
                      itemBuilder: (_, i) {
                        final a = m.alerts[i];
                        return Dismissible(
                          key: ValueKey('${a.det.id}-${a.det.ts}-$i'),
                          direction: DismissDirection.endToStart,
                          background: Container(
                            color: C.red.withValues(alpha: 0.15),
                            alignment: Alignment.centerRight,
                            padding: const EdgeInsets.only(right: 22),
                            child: const Row(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Icon(Ph.bellSlash, color: C.red, size: 18),
                                SizedBox(width: 6),
                                Text(
                                  'Mute',
                                  style: TextStyle(fontFamily: sans, color: C.red, fontWeight: FontWeight.w600),
                                ),
                              ],
                            ),
                          ),
                          confirmDismiss: (_) async {
                            m.toggleMute(a.det.mac);
                            st.muted = m.muted;
                            st.save();
                            m.toast(m.muted.contains(a.det.mac) ? 'Muted ${a.det.mac}' : 'Unmuted ${a.det.mac}');
                            return false;
                          },
                          child: DetRow(a.det, alert: i < m.alertsUnacked),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}
