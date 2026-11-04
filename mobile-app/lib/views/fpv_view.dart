// FPV: the Recon's 5.8 GHz receiver as a screen of the phone, mirroring the display's own
// app (app_video.cpp): the live picture, its channel and signal, picture mode, smoothing,
// scan / sweep / hop, the 6x8 channel map (which the Recon's shell can't even open), and
// what was found, with snapshots and recordings when switched on.
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../data/fpv.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

class FpvView extends StatelessWidget {
  const FpvView({super.key});

  @override
  Widget build(BuildContext context) {
    final f = Fpv.I;
    return ReconPage(
      title: 'FPV  5.8 GHZ',
      actions: const [],

      body: ListenableBuilder(
        listenable: f,
        builder: (context, _) {
          final i = f.info;
          final video = i?.video == true && f.frame != null;
          return ListView(
            padding: const EdgeInsets.only(bottom: 30),
            children: [
              // the picture, or what the receiver is doing instead
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                child: AspectRatio(
                  aspectRatio: fpvW / fpvH,
                  child: Container(
                    decoration: BoxDecoration(
                      color: Colors.black,
                      borderRadius: BorderRadius.circular(3),
                      border: Border.all(color: video ? C.green.withValues(alpha: 0.6) : C.silkLine),
                    ),
                    clipBehavior: Clip.antiAlias,
                    child: video
                        ? RawImage(image: f.frame, fit: BoxFit.fill, filterQuality: FilterQuality.low)
                        : Center(
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Text(
                                  i == null ? 'WAITING FOR THE RECEIVER' : (i.scanning ? (i.hop ? 'HOPPING' : (i.survey ? 'SWEEPING' : 'SCANNING')) : 'NO SIGNAL'),
                                  style: T.label.copyWith(color: C.amber),
                                ),
                                const SizedBox(height: 8),
                                Text(i?.channelName ?? '--', style: T.num.copyWith(fontSize: 44)),
                                Text(i == null || i.mhz == 0 ? '' : '${i.mhz} MHZ', style: T.silk),
                                if (i == null) ...[
                                  const SizedBox(height: 10),
                                  Padding(
                                    padding: const EdgeInsets.symmetric(horizontal: 24),
                                    child: Text(
                                      'Plug in an OUI SPY Recon. Its display passes the receiver\'s picture to the phone once its firmware has the FPV link.',
                                      textAlign: TextAlign.center,
                                      style: T.meta,
                                    ),
                                  ),
                                ],
                              ],
                            ),
                          ),
                  ),
                ),
              ),
              // the readout under it: channel, signal, picture mode, frame rate
              Panel(
                margin: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                padding: EdgeInsets.zero,
                child: IntrinsicHeight(
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Expanded(child: Stat('Channel', i?.channelName ?? '--', sub: i == null || i.mhz == 0 ? null : '${i.mhz} MHz${i.pal ? '  PAL' : '  NTSC'}')),
                      Container(width: 1, color: C.silkLine),
                      Expanded(
                        child: Stat(
                          'Signal',
                          i == null ? '--' : '${(i.level * 100).round()}%',
                          color: i == null ? C.text : C.rssi(-95 + (i.level * 60).round()),
                          sub: i == null ? null : 'gain ${i.gain}  clip ${i.clip}%',
                        ),
                      ),
                      Container(width: 1, color: C.silkLine),
                      Expanded(
                        child: Stat(
                          'Picture',
                          i == null ? '--' : const ['AUTO', 'FINE', 'RAW'][i.mode.clamp(0, 2)],
                          sub: video ? '${f.fps.toStringAsFixed(1)} fps' : (i?.errorText ?? ''),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 6, 16, 0),
                child: ActionStrip([
                  ('Prev', Ph.caretLeft, C.text, () => f.step(-1)),
                  ('Next', Ph.caretRight, C.text, () => f.step(1)),
                  ('Scan', Ph.magnifyingGlass, C.text, f.scan),
                  ('Hop', Ph.shuffle, C.text, f.hop),
                  ('Stop', Ph.stop, C.text, f.stop),
                ]),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: ActionStrip([
                  (
                    'Snapshot',
                    Ph.camera,
                    C.text,
                    f.frame == null
                        ? null
                        : () async {
                            final p = await f.snapshot();
                            App.I.m.toast(p == null ? 'no picture to save' : 'saved ${p.split('/').last}');
                          },
                  ),
                  (f.record ? 'Recording' : 'Record', f.record ? PhF.record : Ph.record, f.record ? C.red : C.text, () => f.setRecord(!f.record)),
                  (f.snapOnLock ? 'Snap on lock' : 'No snaps', Ph.aperture, f.snapOnLock ? C.accent : C.silk, () => f.setSnapOnLock(!f.snapOnLock)),
                  ((i?.smoothing ?? true) ? 'Smooth' : 'Sharp', Ph.waveSine, C.text, () => f.smooth(!(i?.smoothing ?? true))),
                ]),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: Seg<int>(items: const [(0, 'Auto: fastest'), (1, 'Fine: best'), (2, 'Raw: 8 bit')], value: i?.mode ?? 0, onChanged: f.mode),
              ),
              SectionHead(
                'Channel map',
                trailing: GestureDetector(
                  onTap: f.sweep,
                  child: Text('SWEEP', style: T.silk.copyWith(color: C.accent)),
                ),
              ),
              _ChannelMap(f),
              SectionHead('Found', trailing: Text('${f.dets.length}', style: T.silk)),
              if (f.dets.isEmpty)
                Padding(
                  padding: const EdgeInsets.fromLTRB(20, 0, 20, 0),
                  child: Text('FPV transmitters the receiver locks onto show here, and in the Drones feed.', style: T.meta),
                )
              else
                Panel(
                  padding: EdgeInsets.zero,
                  margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
                  child: Column(
                    children: [
                      for (final (k, d) in f.dets.take(40).indexed) ...[if (k > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), FpvDetRow(d)],
                    ],
                  ),
                ),
            ],
          );
        },
      ),
    );
  }
}

/// One FPV find: channel, frequency, strength, when, and its snapshot if one was taken.
class FpvDetRow extends StatelessWidget {
  const FpvDetRow(this.d, {super.key});
  final FpvDet d;
  @override
  Widget build(BuildContext context) => InkWell(
    onTap: () => Fpv.I.tune(d.channel),
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
      child: Row(
        children: [
          IconTile(Ph.videoCamera, C.orange),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Text('FPV  ${d.name}', style: T.row),
                    const SizedBox(width: 8),
                    const Tag('5.8 GHz analog', color: C.orange),
                    if (d.snapshot != null) ...[const SizedBox(width: 6), const Icon(Ph.camera, size: 14, color: C.silk)],
                  ],
                ),
                const SizedBox(height: 3),
                Text('${d.mhz} MHz${d.node.isNotEmpty ? '  ·  ${App.I.m.nodeName(d.node)}' : ''}', style: T.code),
              ],
            ),
          ),
          Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Text(clock(d.ts), style: T.code.copyWith(fontSize: 11.5)),
              const SizedBox(height: 4),
              Text(
                '${(d.level * 100).round()}%',
                style: T.code.copyWith(color: C.rssi(-95 + (d.level * 60).round()), fontWeight: FontWeight.w600),
              ),
            ],
          ),
        ],
      ),
    ),
  );
}

/// 6 bands x 8 channels: fill by power from the last sweep, green where it had sync,
/// a frame on the tuned one. Tap a cell to tune it.
class _ChannelMap extends StatelessWidget {
  const _ChannelMap(this.f);
  final Fpv f;
  @override
  Widget build(BuildContext context) => Panel(
    margin: const EdgeInsets.fromLTRB(16, 0, 16, 4),
    padding: const EdgeInsets.all(8),
    child: Column(
      children: [
        for (var b = 0; b < 6; b++)
          Row(
            children: [
              SizedBox(width: 22, child: Text(fpvBands[b], style: T.silk)),
              for (var c = 0; c < 8; c++)
                Expanded(
                  child: GestureDetector(
                    onTap: () => f.tune(b * 8 + c),
                    child: Builder(
                      builder: (_) {
                        final i = b * 8 + c;
                        final s = f.survey[i];
                        final tuned = f.info?.channel == i;
                        final lvl = s?.level ?? 0;
                        final sync = s != null && s.video;
                        return Container(
                          height: 30,
                          margin: const EdgeInsets.all(2),
                          decoration: BoxDecoration(
                            color: s == null ? C.panel : (sync ? C.green : (s.error == 7 ? C.hot : C.accent)).withValues(alpha: 0.12 + lvl * 0.6),
                            borderRadius: BorderRadius.circular(2),
                            border: Border.all(color: tuned ? C.text : C.silkLine, width: tuned ? 1.5 : 1),
                          ),
                          alignment: Alignment.center,
                          child: Text('${c + 1}', style: T.code.copyWith(fontSize: 11, color: s == null ? C.faint : C.text)),
                        );
                      },
                    ),
                  ),
                ),
            ],
          ),
        const SizedBox(height: 4),
        Text(
          f.survey.isEmpty ? 'Tap SWEEP to measure every channel.' : 'Green: video with sync. Magenta: a neighbour\'s carrier. Tap a channel to tune it.',
          style: T.meta.copyWith(fontSize: 11.5),
        ),
      ],
    ),
  );
}

/// A small live picture for other screens (the Drones feed).
class FpvThumb extends StatelessWidget {
  const FpvThumb({super.key, this.width = 112});
  final double width;
  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: Fpv.I,
    builder: (_, _) {
      final ui.Image? img = Fpv.I.frame;
      return Container(
        width: width,
        height: width * fpvH / fpvW,
        decoration: BoxDecoration(
          color: Colors.black,
          borderRadius: BorderRadius.circular(2),
          border: Border.all(color: C.silkLine),
        ),
        clipBehavior: Clip.antiAlias,
        child: img == null ? const Icon(Ph.videoCameraSlash, color: C.faint, size: 20) : RawImage(image: img, fit: BoxFit.fill),
      );
    },
  );
}
