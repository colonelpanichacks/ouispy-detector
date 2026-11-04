// The look: a dark instrument panel. Quiet surfaces and hairlines carry the layout; neon is
// kept for what it means (live, alert, signal, a node's own colour), never for chrome.
// Type is Inter, with JetBrains Mono for MACs, IDs and numbers. Icons are Phosphor.
import 'package:flutter/material.dart';
import 'package:phosphor_flutter/phosphor_flutter.dart';

typedef Ph = PhosphorIconsRegular;
typedef PhF = PhosphorIconsFill;

class C {
  // ground and surfaces
  static const bg = Color(0xFF07090C);
  static const panel = Color(0xFF0E1116);
  static const panel2 = Color(0xFF151A21);
  static const line = Color(0xFF1A2028);
  static const line2 = Color(0xFF262E39);
  // text
  static const text = Color(0xFFE8EDF3);
  static const soft = Color(0xFFB4C0CE);
  static const muted = Color(0xFF7D8999);
  static const faint = Color(0xFF55606F);
  // the brand accent and the meaning colours
  static const accent = Color(0xFF2EE6D6);
  static const cyan = Color(0xFF38D5FF);
  static const green = Color(0xFF3DFF9A);
  static const lime = Color(0xFFA6F04A);
  static const amber = Color(0xFFFFB547);
  static const orange = Color(0xFFFF9447);
  static const red = Color(0xFFFF4D5E);
  static const hot = Color(0xFFFF3DA8);
  static const pink = Color(0xFFFF8AD0);
  static const violet = Color(0xFFA78BFA);
  static const purple = Color(0xFFC4B5FD);
  static const sky = Color(0xFF5CC8FF);
  static const me = Color(0xFF4DA3FF);
  // silkscreen: the board's white ink, printed grey so the content stays in front
  static const silk = Color(0xFF8C939C);
  static const silkLine = Color(0xFF2A3038);
  static const silkMark = Color(0xFF59616B);

  // A node's dashboard colour is an HSL hue; fall back to a rotating neon.
  static Color hue(int? h, [int i = 0]) {
    if (h == null || h < 0) return [hot, orange, green, violet, cyan][i % 5];
    return HSLColor.fromAHSL(1, h.toDouble() % 360, 0.9, 0.62).toColor();
  }

  static Color dom(String? d) => d == 'ble' ? violet : sky;

  static Color rssi(int? r) {
    if (r == null) return muted;
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
    _ => muted,
  };
}

const sans = 'Inter';
const mono = 'JetBrainsMono';

class T {
  static const title = TextStyle(fontFamily: sans, color: C.text, fontSize: 22, fontWeight: FontWeight.w600, letterSpacing: -0.4, height: 1.2);
  static const h2 = TextStyle(fontFamily: sans, color: C.text, fontSize: 17, fontWeight: FontWeight.w600, letterSpacing: -0.2);
  static const row = TextStyle(fontFamily: sans, color: C.text, fontSize: 15, fontWeight: FontWeight.w500, letterSpacing: -0.1);
  static const body = TextStyle(fontFamily: sans, color: C.soft, fontSize: 14, height: 1.4);
  static const meta = TextStyle(fontFamily: sans, color: C.muted, fontSize: 12.5);
  static const label = TextStyle(fontFamily: mono, color: C.silk, fontSize: 11, fontWeight: FontWeight.w600, letterSpacing: 1.8);
  static const silk = TextStyle(fontFamily: mono, color: C.silk, fontSize: 10.5, fontWeight: FontWeight.w500, letterSpacing: 1.2);
  static const code = TextStyle(fontFamily: mono, color: C.muted, fontSize: 12, letterSpacing: -0.2);
  static const num = TextStyle(
    fontFamily: sans,
    color: C.text,
    fontSize: 24,
    fontWeight: FontWeight.w600,
    letterSpacing: -0.6,
    fontFeatures: [FontFeature.tabularFigures()],
  );
}

ThemeData buildTheme() {
  final base = ThemeData.dark(useMaterial3: true);
  return base.copyWith(
    scaffoldBackgroundColor: C.bg,
    canvasColor: C.bg,
    colorScheme: const ColorScheme.dark(primary: C.accent, onPrimary: C.bg, secondary: C.accent, surface: C.panel, onSurface: C.text, error: C.red),
    textTheme: base.textTheme.apply(fontFamily: sans, bodyColor: C.text, displayColor: C.text),
    dividerColor: C.line,
    dividerTheme: const DividerThemeData(color: C.line, thickness: 1, space: 1),
    splashFactory: InkSparkle.splashFactory,
    appBarTheme: const AppBarTheme(
      backgroundColor: C.bg,
      surfaceTintColor: Colors.transparent,
      elevation: 0,
      scrolledUnderElevation: 0,
      foregroundColor: C.text,
      titleTextStyle: T.h2,
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: C.panel2,
      isDense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 13),
      labelStyle: T.meta,
      hintStyle: const TextStyle(color: C.faint, fontFamily: sans),
      border: OutlineInputBorder(borderRadius: BorderRadius.circular(12), borderSide: BorderSide.none),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: C.line),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: C.accent),
      ),
    ),
    snackBarTheme: SnackBarThemeData(
      backgroundColor: C.panel2,
      contentTextStyle: const TextStyle(fontFamily: sans, color: C.text, fontWeight: FontWeight.w500),
      behavior: SnackBarBehavior.floating,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: C.line2),
      ),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: C.panel,
      surfaceTintColor: Colors.transparent,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(20),
        side: const BorderSide(color: C.line),
      ),
      titleTextStyle: T.h2,
    ),
    bottomSheetTheme: const BottomSheetThemeData(
      backgroundColor: C.panel,
      surfaceTintColor: Colors.transparent,
      showDragHandle: true,
      dragHandleColor: C.line2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
    ),
    listTileTheme: const ListTileThemeData(iconColor: C.muted, titleTextStyle: T.row, subtitleTextStyle: T.meta),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.bg : C.muted),
      trackColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.accent : C.panel2),
      trackOutlineColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.accent : C.line2),
    ),
    progressIndicatorTheme: const ProgressIndicatorThemeData(color: C.accent, linearTrackColor: C.line),
  );
}

List<Shadow> glowOf(Color c, [double r = 8]) => [Shadow(color: c.withValues(alpha: 0.7), blurRadius: r)];

/// A board outline: a hairline of silk with corner marks, like a part's outline on the
/// silkscreen. The ground shows through; colour is for what is printed inside.
class Panel extends StatelessWidget {
  const Panel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(16),
    this.onTap,
    this.margin = const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
    this.edge,
  });
  final Widget child;
  final EdgeInsets padding;
  final EdgeInsets margin;
  final VoidCallback? onTap;
  final Color? edge; // something that matters now: the outline and marks take its colour
  @override
  Widget build(BuildContext context) => Padding(
    padding: margin,
    child: CustomPaint(
      foregroundPainter: _Marks(edge ?? C.silkMark),
      child: Material(
        color: C.bg.withValues(alpha: 0.72),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(3),
          side: BorderSide(color: edge?.withValues(alpha: 0.45) ?? C.silkLine),
        ),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          child: Padding(padding: padding, child: child),
        ),
      ),
    ),
  );
}

class _Marks extends CustomPainter {
  _Marks(this.c);
  final Color c;
  @override
  void paint(Canvas canvas, Size s) {
    const k = 7.0;
    final p = Paint()
      ..color = c
      ..strokeWidth = 1.4
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.square;
    for (final (x, y, dx, dy) in [(0.0, 0.0, 1.0, 1.0), (s.width, 0.0, -1.0, 1.0), (0.0, s.height, 1.0, -1.0), (s.width, s.height, -1.0, -1.0)]) {
      canvas.drawLine(Offset(x, y), Offset(x + dx * k, y), p);
      canvas.drawLine(Offset(x, y), Offset(x, y + dy * k), p);
    }
  }

  @override
  bool shouldRepaint(_Marks o) => o.c != c;
}

/// A titled group of rows in one outline, divided by silk hairlines.
class Group extends StatelessWidget {
  const Group({super.key, this.title, this.trailing, required this.children, this.edge});
  final String? title;
  final Widget? trailing;
  final List<Widget> children;
  final Color? edge;
  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      if (title != null) SectionHead(title!, trailing: trailing),
      Panel(
        padding: EdgeInsets.zero,
        edge: edge,
        margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
        child: Column(
          children: [
            for (final (i, c) in children.indexed) ...[if (i > 0) const Divider(indent: 16, endIndent: 16, color: C.silkLine), c],
          ],
        ),
      ),
    ],
  );
}

/// A silkscreen label with a rule running out to the right: NODES ———— 3 of 4.
class SectionHead extends StatelessWidget {
  const SectionHead(this.title, {super.key, this.trailing});
  final String title;
  final Widget? trailing;
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(18, 20, 16, 8),
    child: Row(
      children: [
        Text(title.toUpperCase(), style: T.label),
        const SizedBox(width: 10),
        Expanded(child: Container(height: 1, color: C.silkLine)),
        if (trailing != null) ...[const SizedBox(width: 10), trailing!],
      ],
    ),
  );
}

/// A list row: optional pad icon, title, subtitle, trailing.
class Tile extends StatelessWidget {
  const Tile({super.key, this.icon, this.color = C.muted, required this.title, this.subtitle, this.trailing, this.onTap, this.titleColor});
  final IconData? icon;
  final Color color;
  final String title;
  final Widget? subtitle;
  final Widget? trailing;
  final VoidCallback? onTap;
  final Color? titleColor;
  @override
  Widget build(BuildContext context) => InkWell(
    onTap: onTap,
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: Row(
        children: [
          if (icon != null) ...[IconTile(icon!, color), const SizedBox(width: 12)],
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: T.row.copyWith(color: titleColor),
                ),
                if (subtitle != null) ...[const SizedBox(height: 3), subtitle!],
              ],
            ),
          ),
          if (trailing != null) ...[const SizedBox(width: 10), trailing!],
          if (onTap != null && trailing == null) const Icon(Ph.caretRight, size: 15, color: C.silkMark),
        ],
      ),
    ),
  );
}

/// An icon in a ring, like a pad on the board.
class IconTile extends StatelessWidget {
  const IconTile(this.icon, this.color, {super.key, this.size = 36});
  final IconData icon;
  final Color color;
  final double size;
  @override
  Widget build(BuildContext context) => Container(
    width: size,
    height: size,
    decoration: BoxDecoration(
      shape: BoxShape.circle,
      border: Border.all(color: color.withValues(alpha: 0.5), width: 1.2),
    ),
    child: Icon(icon, size: size * 0.5, color: color),
  );
}

/// A small label in a printed box.
class Tag extends StatelessWidget {
  const Tag(this.label, {super.key, this.color = C.silk, this.icon});
  final String label;
  final Color color;
  final IconData? icon;
  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2.5),
    decoration: BoxDecoration(
      borderRadius: BorderRadius.circular(2),
      border: Border.all(color: color.withValues(alpha: 0.55)),
    ),
    child: Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (icon != null) ...[Icon(icon, size: 11, color: color), const SizedBox(width: 4)],
        Text(
          label.toUpperCase(),
          style: TextStyle(fontFamily: mono, color: color, fontSize: 10, fontWeight: FontWeight.w600, letterSpacing: 0.8, height: 1.15),
        ),
      ],
    ),
  );
}

/// A filter chip: a printed box, lit in the accent when on.
class Chip2 extends StatelessWidget {
  const Chip2(this.label, {super.key, this.on = false, this.onTap, this.color = C.accent, this.icon, this.count});
  final String label;
  final bool on;
  final VoidCallback? onTap;
  final Color color;
  final IconData? icon;
  final int? count;
  @override
  Widget build(BuildContext context) => Material(
    color: on ? color.withValues(alpha: 0.1) : C.bg.withValues(alpha: 0.72),
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(3),
      side: BorderSide(color: on ? color.withValues(alpha: 0.75) : C.silkLine),
    ),
    clipBehavior: Clip.antiAlias,
    child: InkWell(
      onTap: onTap,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 11, vertical: 8),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (icon != null) ...[Icon(icon, size: 14, color: on ? color : C.silk), const SizedBox(width: 6)],
            Text(
              label.toUpperCase(),
              style: TextStyle(fontFamily: mono, color: on ? color : C.soft, fontSize: 11.5, fontWeight: FontWeight.w600, letterSpacing: 1),
            ),
            if (count != null) ...[
              const SizedBox(width: 6),
              Text(
                '$count',
                style: TextStyle(fontFamily: mono, color: on ? color : C.silk, fontSize: 11.5, fontWeight: FontWeight.w500),
              ),
            ],
          ],
        ),
      ),
    ),
  );
}

/// A segmented control: printed cells, the chosen one lit with an accent rule under it.
class Seg<V> extends StatelessWidget {
  const Seg({super.key, required this.items, required this.value, required this.onChanged, this.color = C.accent});
  final List<(V, String)> items;
  final V value;
  final ValueChanged<V> onChanged;
  final Color color;
  @override
  Widget build(BuildContext context) => Container(
    decoration: BoxDecoration(
      color: C.bg.withValues(alpha: 0.72),
      borderRadius: BorderRadius.circular(3),
      border: Border.all(color: C.silkLine),
    ),
    child: IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (final (i, (v, l)) in items.indexed) ...[
            if (i > 0) Container(width: 1, color: C.silkLine),
            Expanded(
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: () => onChanged(v),
                child: AnimatedContainer(
                  duration: const Duration(milliseconds: 160),
                  padding: const EdgeInsets.symmetric(vertical: 10),
                  decoration: BoxDecoration(
                    color: v == value ? color.withValues(alpha: 0.08) : Colors.transparent,
                    border: Border(bottom: BorderSide(color: v == value ? color : Colors.transparent, width: 2)),
                  ),
                  alignment: Alignment.center,
                  child: Text(
                    l.toUpperCase(),
                    maxLines: 1,
                    overflow: TextOverflow.fade,
                    softWrap: false,
                    style: TextStyle(fontFamily: mono, fontSize: 11.5, fontWeight: FontWeight.w600, letterSpacing: 1.1, color: v == value ? color : C.silk),
                  ),
                ),
              ),
            ),
          ],
        ],
      ),
    ),
  );
}

enum BtnKind { filled, tonal, ghost }

/// A button: icon + label, in a printed outline.
class Btn extends StatelessWidget {
  const Btn(this.label, {super.key, this.icon, this.onTap, this.color = C.accent, this.kind = BtnKind.tonal, this.expand = false});
  final String label;
  final IconData? icon;
  final VoidCallback? onTap;
  final Color color;
  final BtnKind kind;
  final bool expand;
  @override
  Widget build(BuildContext context) {
    final fg = kind == BtnKind.filled ? C.bg : color;
    final bg = switch (kind) {
      BtnKind.filled => color,
      BtnKind.tonal => color.withValues(alpha: 0.08),
      BtnKind.ghost => Colors.transparent,
    };
    final side = switch (kind) {
      BtnKind.filled => BorderSide.none,
      BtnKind.tonal => BorderSide(color: color.withValues(alpha: 0.5)),
      BtnKind.ghost => const BorderSide(color: C.silkLine),
    };
    return Opacity(
      opacity: onTap == null ? 0.45 : 1,
      child: Material(
        color: bg,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(3), side: side),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
            child: Row(
              mainAxisSize: expand ? MainAxisSize.max : MainAxisSize.min,
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                if (icon != null) Icon(icon, size: 17, color: fg),
                if (icon != null && label.isNotEmpty) const SizedBox(width: 7),
                if (label.isNotEmpty)
                  Flexible(
                    child: Text(
                      label,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(fontFamily: sans, color: fg, fontSize: 14, fontWeight: FontWeight.w600),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// A status dot; live ones get a soft halo, like an LED.
class Dot extends StatelessWidget {
  const Dot(this.color, {super.key, this.size = 8, this.halo = false});
  final Color color;
  final double size;
  final bool halo;
  @override
  Widget build(BuildContext context) => Container(
    width: size,
    height: size,
    decoration: BoxDecoration(
      color: color,
      shape: BoxShape.circle,
      boxShadow: halo ? [BoxShadow(color: color.withValues(alpha: 0.55), blurRadius: 8, spreadRadius: 1)] : null,
    ),
  );
}

/// A label / value row for detail pages: the label printed in silk.
class KV extends StatelessWidget {
  const KV(this.k, this.v, {super.key, this.color = C.text, this.monoValue = false});
  final String k;
  final String v;
  final Color color;
  final bool monoValue;
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 11),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 124,
          child: Padding(
            padding: const EdgeInsets.only(top: 2),
            child: Text(k.toUpperCase(), style: T.silk),
          ),
        ),
        Expanded(
          child: SelectableText(
            v,
            style: monoValue
                ? TextStyle(fontFamily: mono, color: color, fontSize: 13, height: 1.4)
                : TextStyle(fontFamily: sans, color: color, fontSize: 14, height: 1.4),
          ),
        ),
      ],
    ),
  );
}

/// A readout: a silk label over a big number.
class Stat extends StatelessWidget {
  const Stat(this.label, this.value, {super.key, this.color = C.text, this.icon, this.sub});
  final String label, value;
  final Color color;
  final IconData? icon;
  final String? sub;
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(14, 12, 10, 12),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(label.toUpperCase(), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.silk),
        const SizedBox(height: 6),
        Text(
          value,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: T.num.copyWith(color: color),
        ),
        if (sub != null) Text(sub!, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.meta.copyWith(fontSize: 11.5)),
      ],
    ),
  );
}

/// Signal bars coloured by strength.
class RssiBar extends StatelessWidget {
  const RssiBar(this.rssi, {super.key});
  final int? rssi;
  @override
  Widget build(BuildContext context) {
    final r = rssi ?? -127;
    final n = r >= -60
        ? 4
        : r >= -72
        ? 3
        : r >= -84
        ? 2
        : r > -127
        ? 1
        : 0;
    final c = C.rssi(rssi);
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [for (var i = 0; i < 4; i++) Container(width: 3, height: 4.0 + i * 2.5, margin: const EdgeInsets.only(left: 1.5), color: i < n ? c : C.silkLine)],
    );
  }
}

/// An empty or error state: icon, title, a line of help, an optional action.
class Empty extends StatelessWidget {
  const Empty(this.title, {super.key, this.body, this.icon = Ph.circlesThree, this.color = C.muted, this.action});
  final String title;
  final String? body;
  final IconData icon;
  final Color color;
  final Widget? action;
  @override
  Widget build(BuildContext context) => Center(
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 36, vertical: 40),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          IconTile(icon, color, size: 56),
          const SizedBox(height: 14),
          Text(title, textAlign: TextAlign.center, style: T.h2.copyWith(fontSize: 16)),
          if (body != null) ...[const SizedBox(height: 6), Text(body!, textAlign: TextAlign.center, style: T.meta.copyWith(fontSize: 13.5, height: 1.45))],
          if (action != null) ...[const SizedBox(height: 16), action!],
        ],
      ),
    ),
  );
}

/// A row of actions as one printed strip: icon over a silk label, cells split by rules.
class ActionStrip extends StatelessWidget {
  const ActionStrip(this.items, {super.key});
  final List<(String, IconData, Color, VoidCallback?)> items;
  @override
  Widget build(BuildContext context) => Container(
    decoration: BoxDecoration(
      color: C.bg.withValues(alpha: 0.72),
      borderRadius: BorderRadius.circular(3),
      border: Border.all(color: C.silkLine),
    ),
    child: IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (final (i, (l, ic, c, f)) in items.indexed) ...[
            if (i > 0) Container(width: 1, color: C.silkLine),
            Expanded(
              child: InkWell(
                onTap: f,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 10),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(ic, size: 19, color: f == null ? C.faint : c),
                      const SizedBox(height: 5),
                      Text(
                        l.toUpperCase(),
                        maxLines: 1,
                        overflow: TextOverflow.fade,
                        softWrap: false,
                        style: T.silk.copyWith(fontSize: 9.5, letterSpacing: 0.8, color: c == C.text ? C.silk : c),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ],
        ],
      ),
    ),
  );
}
