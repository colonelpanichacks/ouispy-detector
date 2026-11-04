// Writes the alert tones (lib/data/tones.dart) into android/app/src/main/res/raw.
// Run from mobile-app/: dart run tool/gen_tones.dart
import 'dart:io';

import 'package:ouispy_mesh/data/tones.dart';

void main() {
  final dir = Directory('android/app/src/main/res/raw')..createSync(recursive: true);
  for (final s in Sfx.values) {
    for (final v in volumes) {
      File('${dir.path}/${toneName(s, v)}.wav').writeAsBytesSync(toneWav(tunes[s]!, v));
    }
  }
  // Release builds shrink resources; nothing in code names these, so keep them.
  File('${dir.path}/keep.xml').writeAsStringSync(
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<resources xmlns:tools="http://schemas.android.com/tools" tools:keep="@raw/sfx_*" />\n',
  );
  stdout.writeln('wrote ${Sfx.values.length * volumes.length} tones to ${dir.path}');
}
