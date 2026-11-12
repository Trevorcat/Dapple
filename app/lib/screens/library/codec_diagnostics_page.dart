import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../native/dapple_session.dart';
import '../../services/live_bridge.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import 'page_bar.dart';

/// Hidden diagnostics (long-press the version on 关于): what the device's codecs declare, the ladders that follow from it and
/// the one-time self-test results. Nothing here can be switched; the codec choice is automatic.
class CodecDiagnosticsPage extends StatefulWidget {
  const CodecDiagnosticsPage({super.key, this.loader, this.gpuLoader});

  /// Test hook: returns the report JSON (`run` = run missing self-tests, `force` = repeat all).
  final Future<String> Function({bool run, bool force})? loader;

  /// Test hook: returns the GPU status JSON (`dapple_gpu_status_json`); default reads it from the engine.
  final String? Function()? gpuLoader;

  @override
  State<CodecDiagnosticsPage> createState() => _CodecDiagnosticsPageState();
}

class _CodecDiagnosticsPageState extends State<CodecDiagnosticsPage> {
  Map<String, dynamic>? _r;
  String? _error;
  bool _busy = false;

  Future<String> _load({bool run = false, bool force = false}) =>
      (widget.loader ?? LiveBridge.codecReport)(run: run, force: force);

  @override
  void initState() {
    super.initState();
    unawaited(_refresh());
  }

  Future<void> _refresh({bool run = false, bool force = false}) async {
    setState(() => _busy = true);
    try {
      final j = await _load(run: run, force: force);
      if (!mounted) return;
      setState(() {
        _r = j.isEmpty ? null : (jsonDecode(j) as Map<String, dynamic>);
        _error = j.isEmpty ? '没有数据' : null;
        _busy = false;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = '$e';
        _busy = false;
      });
    }
  }

  List<String> _strs(Object? o) => [for (final x in (o as List? ?? const [])) '$x'];

  /// The GPU executor block: probe state, device and self-test, and how many frames each executor rendered (and why the CPU did).
  String _gpuText() {
    final String? raw;
    try {
      raw = (widget.gpuLoader ?? DappleSession.gpuStatusJson)();
    } catch (e) {
      return 'GPU\n  unavailable: $e\n';
    }
    if (raw == null || raw.isEmpty) return 'GPU\n  no data\n';
    final Map<String, dynamic> g;
    try {
      g = jsonDecode(raw) as Map<String, dynamic>;
    } catch (_) {
      return 'GPU\n  unreadable status\n';
    }
    final b = StringBuffer('GPU\n');
    if (g['compiled'] == false) {
      b.writeln('  not compiled into this build -> every frame is a CPU frame');
      return b.toString();
    }
    b.writeln('  mode ${g['mode']} · probe ${g['state']} · ${g['enabled'] == true ? 'in use' : 'not in use'}');
    if (g['available'] == true) {
      b.writeln('  ${g['api']} · ${g['device']}');
      b.writeln('  self-test ${g['selfTest']}');
    } else if (g['reason'] != null) {
      b.writeln('  ${g['reason']}');
    }
    if ((g['skipped'] ?? '').toString().isNotEmpty) b.writeln('  skipped/failed first: ${g['skipped']}');
    final ex = g['executors'];
    if (ex is Map) {
      b.writeln('  frames: GPU ${ex['gpuFrames']} · CPU ${ex['cpuFrames']} · last executor ${ex['api']}');
      b.writeln('  device lost ${ex['deviceLost']} · demotions ${ex['demotions']}');
      final un = ex['unsupported'];
      if (un is Map && un.isNotEmpty) {
        b.writeln('  CPU frames by reason');
        final e = un.entries.toList()..sort((a, c) => (c.value as num).compareTo(a.value as num));
        for (final x in e) {
          b.writeln('    ${x.value}  ${x.key}');
        }
      }
    }
    return b.toString();
  }

  String _text() {
    final r = _r;
    final gpu = _gpuText();
    if (r == null) return '$gpu\n${_error ?? ''}';
    final b = StringBuffer();
    b.write(gpu);
    b.writeln();
    b.writeln('${r['fingerprint']}');
    b.writeln('Android API ${r['sdk']} · ${r['abi']}');
    b.writeln();
    b.writeln('Self-tests');
    final tests = (r['selfTests'] as List? ?? const []);
    if (tests.isEmpty) b.writeln('  none yet');
    for (final t in tests) {
      b.writeln('  ${t['pass'] == true ? 'PASS' : 'FAIL'}  ${t['kind']}  ${t['codec']}  ${t['ms']} ms');
      b.writeln('        ${t['detail']}');
    }
    b.writeln();
    for (final k in const [
      'encodeLadder1080p30',
      'encodeLadder1080p30Hdr',
      'encodeLadder8Bit',
      'decodeLadderHevc10',
      'decodeLadderHevc8',
      'decodeLadderAvc8',
      'decodeLadderAvc422',
    ]) {
      b.writeln(k);
      final l = _strs(r[k]);
      if (l.isEmpty) b.writeln('  -');
      for (var i = 0; i < l.length; i++) {
        b.writeln('  ${i + 1}. ${l[i]}');
      }
    }
    b.writeln();
    b.writeln('Last jobs');
    final jobs = _strs(r['jobs']);
    if (jobs.isEmpty) b.writeln('  none');
    for (final j in jobs.reversed.take(10)) {
      b.writeln('  $j');
    }
    b.writeln();
    b.writeln('Declared codecs');
    for (final c in (r['codecs'] as List? ?? const [])) {
      b.writeln('${c['name']}  ${c['mime']}  ${c['type']}  ${c['hardware'] == true ? 'hw' : 'sw'}'
          '${c['hdrEditing'] == true ? '  hdr-editing' : ''}');
      b.writeln('    profiles ${_strs(c['profiles']).join(', ')}');
      b.writeln('    formats ${_strs(c['colorFormats']).join(', ')} · max ${c['maxSize']} · 1080p ${c['maxFps1080']} fps · ${c['performancePoints']} perf points');
    }
    return b.toString();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Scaffold(
      key: const ValueKey('codec-diagnostics'),
      backgroundColor: c.bg,
      body: Column(children: [
        PageBar('编解码器',
            leading: DIconButton(DI.back, label: '返回', onPressed: () => Navigator.of(context).maybePop()),
            trailing: TextAction('复制', size: 17, onPressed: () => Clipboard.setData(ClipboardData(text: _text())))),
        Expanded(
          child: ListView(padding: const EdgeInsets.all(Sp.l), children: [
            Row(children: [
              TextAction(_busy ? '检测中…' : '运行自检', size: 17, onPressed: _busy ? null : () => _refresh(run: true)),
              const SizedBox(width: Sp.l),
              TextAction('全部重测', size: 17, onPressed: _busy ? null : () => _refresh(force: true)),
            ]),
            const SizedBox(height: Sp.m),
            SelectableText(
              _text(),
              key: const ValueKey('codec-report'),
              style: TextStyle(fontFamily: 'monospace', fontSize: 12, height: 1.35, color: c.text),
            ),
          ]),
        ),
      ]),
    );
  }
}
