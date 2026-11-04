// CONSOLE: the backend's console (USB: the node's own serial; the dashboard: its serial
// console) with the live lines after it, coloured by kind like the dashboard (app.js
// CON_KINDS), and a line to send. Secrets (fleet keys, passwords) never show.
import 'dart:async';

import 'package:flutter/material.dart';

import '../data/backend.dart';
import '../main.dart';
import '../theme.dart';
import 'common.dart';

const _kinds = [
  ('TLM ', C.faint),
  ('RST ', C.cyan),
  ('RFL ', C.violet),
  ('CMD ', C.amber),
  ('ACK ', C.green),
  ('OK', C.green),
  ('ERR', C.red),
  ('OTA ', C.orange),
  ('[OTA]', C.orange),
  ('[MESH]', C.sky),
  ('[HEARTBEAT]', C.silkMark),
  ('DET ', C.text),
];

Color consoleColor(ConsoleLine l) {
  if (l.src == 'phone' || l.src == 'you') return C.accent;
  final s = l.line;
  for (final (p, c) in _kinds) {
    if (s.startsWith(p)) return c;
  }
  if (s.startsWith('{')) return s.contains('"upd":1') ? C.muted : C.text;
  return C.soft;
}

final _secretArgs = RegExp(r'^(\s*(?:r?meshkey|r?setkey|key)\s+)(\S+)(.*)$', caseSensitive: false);
final _secretKv = RegExp(r'''((?:pass(?:word)?|pwd|psk|meshkey|apikey|token|secret)"?\s*[=:]\s*"?)([^\s",}]+)''', caseSensitive: false);

/// The line with fleet-key arguments and password= / key: values masked.
String redact(String s) {
  final a = _secretArgs.firstMatch(s);
  if (a != null) {
    // `rmeshkey <id> <key>`: the last word is the key
    final words = '${a.group(2)}${a.group(3)}'.trim().split(RegExp(r'\s+'));
    s = '${a.group(1)}${words.length > 1 ? '${words.sublist(0, words.length - 1).join(' ')} ' : ''}****';
  }
  return s.replaceAllMapped(_secretKv, (m) => '${m.group(1)}****');
}

const _verbs = ['status', 'filters', 'rstatus all', 'ident 10', 'help'];

class ConsoleView extends StatefulWidget {
  const ConsoleView({super.key});
  @override
  State<ConsoleView> createState() => _ConsoleViewState();
}

class _ConsoleViewState extends State<ConsoleView> {
  final _scroll = ScrollController();
  final _send = TextEditingController();
  final _filter = TextEditingController();
  StreamSubscription<ConsoleLine>? _sub;
  Backend? _bound;
  List<ConsoleLine> _lines = [];
  final _held = <ConsoleLine>[];
  bool paused = false, stick = true;
  double _clearedAt = 0;

  @override
  void initState() {
    super.initState();
    _scroll.addListener(() {
      if (!_scroll.hasClients) return;
      final p = _scroll.position;
      final atEnd = p.pixels >= p.maxScrollExtent - 30;
      if (atEnd != stick) setState(() => stick = atEnd);
    });
    _bind();
  }

  /// The backend can switch (USB plugged in / out): follow App.I.link.
  void _bind() {
    final b = App.I.link;
    if (identical(b, _bound)) return;
    _sub?.cancel();
    _bound = b;
    _lines = b.console.ring.where((l) => l.ts > _clearedAt).toList();
    _sub = b.console.stream.listen((l) {
      if (paused) {
        _held.add(l);
        if (_held.length > 600) _held.removeRange(0, _held.length - 600);
        return;
      }
      setState(() => _add([l]));
    });
    _toEnd();
  }

  void _add(Iterable<ConsoleLine> ls) {
    _lines.addAll(ls);
    if (_lines.length > 800) _lines.removeRange(0, _lines.length - 800);
    if (stick) _toEnd();
  }

  void _toEnd() => WidgetsBinding.instance.addPostFrameCallback((_) {
    if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
  });

  @override
  void dispose() {
    _sub?.cancel();
    _scroll.dispose();
    _send.dispose();
    _filter.dispose();
    super.dispose();
  }

  Future<void> _sendLine([String? v]) async {
    final line = (v ?? _send.text).trim();
    if (line.isEmpty) return;
    final ok = await App.I.link.sendLine(line);
    if (!mounted) return;
    if (ok) {
      if (v == null) _send.clear();
      setState(() => stick = true);
      _toEnd();
    } else {
      App.I.m.toast('nothing is listening on the console');
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: App.I.m,
      builder: (context, _) {
        _bind();
        final q = _filter.text.trim().toLowerCase();
        final shown = q.isEmpty ? _lines : _lines.where((l) => l.line.toLowerCase().contains(q)).toList();
        return Scaffold(
          appBar: AppBar(
            title: Text('CONSOLE', style: T.label.copyWith(fontSize: 13, color: C.text)),
            actions: [
              IconButton(
                tooltip: paused ? 'Resume' : 'Pause',
                icon: Icon(paused ? Ph.play : Ph.pause, color: paused ? C.amber : C.silk),
                onPressed: () => setState(() {
                  paused = !paused;
                  if (!paused) {
                    _add(_held);
                    _held.clear();
                  }
                }),
              ),
              IconButton(
                tooltip: 'Clear',
                icon: const Icon(Ph.trash, color: C.silk),
                onPressed: () => setState(() {
                  _clearedAt = DateTime.now().millisecondsSinceEpoch / 1000;
                  _lines.clear();
                  _held.clear();
                }),
              ),
            ],
          ),
          body: Column(
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 6, 16, 4),
                child: TextField(
                  controller: _filter,
                  style: T.code.copyWith(color: C.text),
                  onChanged: (_) => setState(() {}),
                  decoration: InputDecoration(
                    hintText: 'Filter lines',
                    prefixIcon: const Icon(Ph.funnel, size: 16, color: C.muted),
                    suffixIcon: q.isEmpty
                        ? null
                        : IconButton(
                            icon: const Icon(Ph.x, size: 16, color: C.muted),
                            onPressed: () => setState(_filter.clear),
                          ),
                  ),
                ),
              ),
              Expanded(
                child: Container(
                  margin: const EdgeInsets.fromLTRB(16, 4, 16, 4),
                  decoration: BoxDecoration(
                    color: C.bg,
                    borderRadius: BorderRadius.circular(3),
                    border: Border.all(color: C.silkLine),
                  ),
                  child: Stack(
                    children: [
                      if (shown.isEmpty)
                        Empty(q.isEmpty ? 'No lines yet' : 'No line matches', body: q.isEmpty ? 'Console lines show here as the node prints them.' : null, icon: Ph.pulse)
                      else
                        ListView.builder(
                          controller: _scroll,
                          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                          itemCount: shown.length,
                          itemBuilder: (_, i) => _row(shown[i]),
                        ),
                      if (paused || !stick)
                        Positioned(
                          right: 8,
                          bottom: 8,
                          child: Row(
                            children: [
                              if (paused) Tag('Paused${_held.isEmpty ? '' : ' · ${_held.length} new'}', color: C.amber),
                              if (!stick) ...[
                                const SizedBox(width: 6),
                                Material(
                                  color: C.panel2,
                                  shape: const CircleBorder(side: BorderSide(color: C.silkLine)),
                                  child: IconButton(
                                    icon: const Icon(Ph.arrowDown, size: 16, color: C.accent),
                                    onPressed: () {
                                      setState(() => stick = true);
                                      _toEnd();
                                    },
                                  ),
                                ),
                              ],
                            ],
                          ),
                        ),
                    ],
                  ),
                ),
              ),
              ChipRow(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
                children: [for (final v in _verbs) Chip2(v, onTap: () => _sendLine(v))],
              ),
              SafeArea(
                top: false,
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(16, 4, 8, 8),
                  child: Row(
                    children: [
                      Expanded(
                        child: TextField(
                          controller: _send,
                          style: T.code.copyWith(color: C.text, fontSize: 13),
                          autocorrect: false,
                          enableSuggestions: false,
                          textInputAction: TextInputAction.send,
                          onSubmitted: (_) => _sendLine(),
                          decoration: InputDecoration(hintText: 'Send a line to ${App.I.link.label}'),
                        ),
                      ),
                      IconButton(
                        icon: const Icon(Ph.paperPlaneRight, color: C.accent),
                        onPressed: _sendLine,
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _row(ConsoleLine l) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 1.5),
    child: Text.rich(
      TextSpan(
        children: [
          TextSpan(
            text: '${clock(l.ts)} ',
            style: const TextStyle(color: C.faint),
          ),
          TextSpan(
            text: redact(l.line),
            style: TextStyle(color: consoleColor(l)),
          ),
        ],
      ),
      style: T.code.copyWith(fontSize: 11.5, height: 1.35),
    ),
  );
}
