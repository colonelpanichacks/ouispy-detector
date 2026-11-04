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
    return Scaffold(
      appBar: AppBar(title: const Glow('ALERTS', color: C.hot, size: 17, spacing: 2)),
      body: ListenableBuilder(
        listenable: m,
        builder: (context, _) => Column(children: [
          PillRow(children: [
            Pill('ACK ALL ${m.alertsUnacked}', color: C.green, on: m.alertsUnacked > 0, onTap: m.ackAlerts),
            Pill(m.muteAll ? 'SILENCED' : 'SILENCE ALL', color: m.muteAll ? C.red : C.amber, on: m.muteAll, onTap: () => m.setMuteAll(!m.muteAll)),
            Pill(App.I.s.vibrate ? 'VIBRATE ON' : 'VIBRATE OFF', color: App.I.s.vibrate ? C.cyan : C.red, onTap: () {
              App.I.s.vibrate = !App.I.s.vibrate;
              App.I.s.save();
              m.changed();
            }),
            Pill(App.I.s.notify ? 'NOTIFY ON' : 'NOTIFY OFF', color: App.I.s.notify ? C.purple : C.red, onTap: () {
              App.I.s.notify = !App.I.s.notify;
              App.I.s.save();
              m.changed();
            }),
          ]),
          if (m.muted.isNotEmpty)
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 6),
              child: Text('${m.muted.length} devices muted (Settings lists them)', style: const TextStyle(color: C.amber, fontSize: 12)),
            ),
          Expanded(
            child: m.alerts.isEmpty
                ? const Empty('No alerts since the app started.', color: C.green)
                : ListView.builder(
                    itemCount: m.alerts.length,
                    itemBuilder: (_, i) {
                      final a = m.alerts[i];
                      return Dismissible(
                        key: ValueKey('${a.det.id}-${a.det.ts}-$i'),
                        direction: DismissDirection.endToStart,
                        background: Container(
                          color: C.red.withValues(alpha: 0.2),
                          alignment: Alignment.centerRight,
                          padding: const EdgeInsets.only(right: 20),
                          child: const Text('MUTE DEVICE', style: TextStyle(color: C.red, fontWeight: FontWeight.w800)),
                        ),
                        confirmDismiss: (_) async {
                          m.toggleMute(a.det.mac);
                          App.I.s.muted = m.muted;
                          App.I.s.save();
                          m.toast(m.muted.contains(a.det.mac) ? 'muted ${a.det.mac}' : 'unmuted ${a.det.mac}');
                          return false;
                        },
                        child: DetRow(a.det, alert: i < m.alertsUnacked),
                      );
                    },
                  ),
          ),
        ]),
      ),
    );
  }
}
