// The Tab5's neon look, for touch: same palette (tab5-dashboard/src/ui.cpp), black ground,
// glowing cyan / pink / amber / green accents. Meaningful states always get a colour.
import 'package:flutter/material.dart';

class C {
  static const bg = Color(0xFF06080B);
  static const panel = Color(0xFF0C0F14);
  static const panel2 = Color(0xFF10141B);
  static const line = Color(0xFF1B222C);
  static const line2 = Color(0xFF2B3542);
  static const text = Color(0xFFDBE4EE);
  static const soft = Color(0xFF9ECFFF);
  static const muted = Color(0xFF8593A6);
  static const faint = Color(0xFF5B6879);
  static const accent = Color(0xFF4ECDC4);
  static const cyan = Color(0xFF22D3EE);
  static const amber = Color(0xFFFBBF24);
  static const orange = Color(0xFFFFB020);
  static const red = Color(0xFFFF4D6D);
  static const pink = Color(0xFFF0ABFC);
  static const hot = Color(0xFFFF2BD6);
  static const green = Color(0xFF39FF88);
  static const lime = Color(0xFF7DFF3A);
  static const purple = Color(0xFFC4B5FD);
  static const violet = Color(0xFFA78BFA);
  static const sky = Color(0xFF7DD3FC);
  static const me = Color(0xFF4DA3FF);

  // A node's dashboard colour is an HSL hue; fall back to a rotating neon.
  static Color hue(int? h, [int i = 0]) {
    if (h == null || h < 0) return [hot, orange, green, violet, cyan][i % 5];
    return HSLColor.fromAHSL(1, h.toDouble() % 360, 0.95, 0.62).toColor();
  }

  static Color dom(String? d) => d == 'ble' ? purple : sky;

  static Color rssi(int? r) {
    if (r == null) return amber;
    if (r >= -60) return green;
    if (r >= -75) return lime;
    if (r >= -85) return amber;
    return red;
  }

  static Color droneTag(String? t) => switch (t) {
        'friendly' => green,
        'neutral' => sky,
        'suspicious' => amber,
        'hostile' => red,
        _ => violet,
      };
}

const mono = 'monospace';

ThemeData buildTheme() {
  final base = ThemeData.dark(useMaterial3: true);
  return base.copyWith(
    scaffoldBackgroundColor: C.bg,
    canvasColor: C.bg,
    colorScheme: const ColorScheme.dark(
      primary: C.accent,
      secondary: C.hot,
      surface: C.panel,
      error: C.red,
    ),
    textTheme: base.textTheme.apply(bodyColor: C.text, displayColor: C.text),
    dividerColor: C.line,
    appBarTheme: const AppBarTheme(backgroundColor: C.bg, elevation: 0, foregroundColor: C.text),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: C.panel2,
      isDense: true,
      labelStyle: const TextStyle(color: C.muted),
      hintStyle: const TextStyle(color: C.faint),
      enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.line2)),
      focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: const BorderSide(color: C.accent, width: 1.5)),
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: C.panel2,
      contentTextStyle: TextStyle(color: C.accent, fontWeight: FontWeight.w600),
      behavior: SnackBarBehavior.floating,
    ),
    dialogTheme: const DialogThemeData(backgroundColor: C.panel),
    bottomSheetTheme: const BottomSheetThemeData(backgroundColor: C.panel),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.green : C.red),
      trackColor: WidgetStateProperty.resolveWith(
          (s) => (s.contains(WidgetState.selected) ? C.green : C.red).withValues(alpha: 0.25)),
    ),
  );
}

List<Shadow> glowOf(Color c, [double r = 8]) => [Shadow(color: c.withValues(alpha: 0.75), blurRadius: r)];

/// Text with the Tab5's glow.
class Glow extends StatelessWidget {
  const Glow(this.text, {super.key, this.color = C.accent, this.size = 15, this.weight = FontWeight.w700, this.spacing = 0.5, this.font});
  final String text;
  final Color color;
  final double size;
  final FontWeight weight;
  final double spacing;
  final String? font;
  @override
  Widget build(BuildContext context) => Text(text,
      overflow: TextOverflow.ellipsis,
      style: TextStyle(color: color, fontSize: size, fontWeight: weight, letterSpacing: spacing, fontFamily: font, shadows: glowOf(color)));
}

/// A dark panel with a neon edge.
class Panel extends StatelessWidget {
  const Panel({super.key, required this.child, this.color = C.line2, this.glow = false, this.padding = const EdgeInsets.all(12), this.onTap, this.margin = const EdgeInsets.symmetric(horizontal: 12, vertical: 5)});
  final Widget child;
  final Color color;
  final bool glow;
  final EdgeInsets padding;
  final EdgeInsets margin;
  final VoidCallback? onTap;
  @override
  Widget build(BuildContext context) {
    final box = Container(
      margin: margin,
      decoration: BoxDecoration(
        color: C.panel,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: glow ? color : color.withValues(alpha: 0.45), width: glow ? 1.4 : 1),
        boxShadow: glow ? [BoxShadow(color: color.withValues(alpha: 0.28), blurRadius: 14)] : null,
      ),
      child: Padding(padding: padding, child: child),
    );
    if (onTap == null) return box;
    return GestureDetector(behavior: HitTestBehavior.opaque, onTap: onTap, child: box);
  }
}

/// A pill: filled when on, outlined when off.
class Pill extends StatelessWidget {
  const Pill(this.label, {super.key, this.color = C.accent, this.on = true, this.onTap, this.small = false});
  final String label;
  final Color color;
  final bool on;
  final bool small;
  final VoidCallback? onTap;
  @override
  Widget build(BuildContext context) {
    final p = Container(
      padding: EdgeInsets.symmetric(horizontal: small ? 7 : 11, vertical: small ? 2 : 6),
      decoration: BoxDecoration(
        color: on ? color.withValues(alpha: 0.16) : Colors.transparent,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: on ? color : color.withValues(alpha: 0.4)),
        boxShadow: on ? [BoxShadow(color: color.withValues(alpha: 0.3), blurRadius: 8)] : null,
      ),
      child: Text(label,
          style: TextStyle(
              color: on ? color : color.withValues(alpha: 0.7),
              fontSize: small ? 10.5 : 12.5,
              fontWeight: FontWeight.w700,
              letterSpacing: 0.8)),
    );
    if (onTap == null) return p;
    return GestureDetector(onTap: onTap, child: p);
  }
}

class Dot extends StatelessWidget {
  const Dot(this.color, {super.key, this.size = 9});
  final Color color;
  final double size;
  @override
  Widget build(BuildContext context) => Container(
        width: size,
        height: size,
        decoration: BoxDecoration(color: color, shape: BoxShape.circle, boxShadow: [BoxShadow(color: color.withValues(alpha: 0.8), blurRadius: 6)]),
      );
}

/// A label / value row for detail pages.
class KV extends StatelessWidget {
  const KV(this.k, this.v, {super.key, this.color = C.text, this.monoValue = false});
  final String k;
  final String v;
  final Color color;
  final bool monoValue;
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          SizedBox(width: 118, child: Text(k.toUpperCase(), style: const TextStyle(color: C.faint, fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 0.8))),
          Expanded(child: SelectableText(v, style: TextStyle(color: color, fontSize: 13.5, fontFamily: monoValue ? mono : null))),
        ]),
      );
}

class SectionHead extends StatelessWidget {
  const SectionHead(this.title, {super.key, this.color = C.accent, this.trailing});
  final String title;
  final Color color;
  final Widget? trailing;
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 6),
        child: Row(children: [
          Container(width: 3, height: 14, color: color, margin: const EdgeInsets.only(right: 8)),
          Expanded(child: Glow(title, color: color, size: 12.5, spacing: 1.6)),
          ?trailing,
        ]),
      );
}

/// Signal bars coloured by strength.
class RssiBar extends StatelessWidget {
  const RssiBar(this.rssi, {super.key});
  final int? rssi;
  @override
  Widget build(BuildContext context) {
    final r = rssi ?? -127;
    final n = r >= -60 ? 4 : r >= -72 ? 3 : r >= -84 ? 2 : r > -127 ? 1 : 0;
    final c = C.rssi(rssi);
    return Row(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.end, children: [
      for (var i = 0; i < 4; i++)
        Container(
          width: 3.5,
          height: 4.0 + i * 3,
          margin: const EdgeInsets.only(right: 1.5),
          color: i < n ? c : c.withValues(alpha: 0.18),
        ),
    ]);
  }
}

class Empty extends StatelessWidget {
  const Empty(this.text, {super.key, this.color = C.amber});
  final String text;
  final Color color;
  @override
  Widget build(BuildContext context) => Center(
      child: Padding(padding: const EdgeInsets.all(32), child: Text(text, textAlign: TextAlign.center, style: TextStyle(color: color, fontSize: 14, height: 1.4))));
}
