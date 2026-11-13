// Debug builds (and --dart-define=DAPPLE_IOS_AUTOTEST=true builds) on iOS only (the simulator has no adb): with a file Documents/DAPPLE_OPEN present (its content = panel group) the app adds every file of Documents/Photos to the library, waits for the first one to be
// developed and opens the editor on it. The content (light / color / style / effects / crop) selects the panel category.
// Release builds never start this. The real photo flow on iOS is the picker import / the Files app (docs/ios/README.md).
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import '../model/params.dart';
import '../screens/editor/editor_page.dart';
import '../native/dapple_bindings.dart';
import '../state/app_state.dart';
import '../services/ingest.dart';
import '../services/perf_probe.dart' show OpenTiming;
import '../native/dapple_session.dart';
import '../services/gpu_view_backend.dart';
import 'ios_autotest.dart' show kIosAutotest;
import 'ios_services.dart';

class IosDebugOpen {
  static Timer? _timer;
  static bool _added = false, _opened = false;

  static void start(AppState app) {
    if (!(kDebugMode || kIosAutotest) || !Platform.isIOS || _timer != null) return;
    _timer = Timer.periodic(const Duration(milliseconds: 700), (_) => _tick(app));
  }

  static String _group = '';
  static Map<String, dynamic> _cfg = const {};
  static bool _armed = false;

  static Future<void> _tick(AppState app) async {
    try {
      if (!_armed) {
        // armed by a file in Documents (simctl cannot pass environment variables reliably): Documents/DAPPLE_OPEN, the content is the group
        final f = File('${(await getApplicationDocumentsDirectory()).path}/DAPPLE_OPEN');
        if (!f.existsSync()) return;
        final txt = (await f.readAsString()).trim();
        // plain text = panel group; JSON: {"group":"style","film":"<stock id>","open":"<name substring>","photokit":true,"documents":true}
        _cfg = txt.startsWith('{') ? (jsonDecode(txt) as Map<String, dynamic>) : {'group': txt, 'documents': true};
        _group = '${_cfg['group'] ?? ''}';
        _armed = true;
      }
      debugPrint('IosDebugOpen: ready=${app.ready} added=$_added editable=${app.editable.length} engine=${DappleBindings.instanceOrNull != null} err=${DappleBindings.loadError}');
      if (!app.ready) return;
      if (!_added) {
        _added = true;
        if (_cfg['documents'] != false) app.ingestFtpFiles(await IosDocumentsSource().scan());
        if (_cfg['photokit'] == true) {
          // PhotoKit end to end: list (any age), the app then copies the originals out of the library when it develops them
          final files = await MediaBridge.queryNew(limit: 50, sinceSec: 0);
          debugPrint('IosDebugOpen: PhotoKit lists ${files.length}: ${files.map((f) => f.name).join(', ')}');
          app.ingestFtpFiles(files);
        }
        return;
      }
      if (_opened) return;
      final docs0 = (await getApplicationDocumentsDirectory()).path;
      final list = app.editable;
      if (list.isEmpty) return;
      final nav = app.navigatorKey.currentState;
      final ctx = app.navigatorKey.currentContext;
      if (nav == null || ctx == null || !ctx.mounted) return;
      _opened = true;
      final want = '${_cfg['open'] ?? ''}';
      final photo = want.isEmpty ? list.first : list.firstWhere((e) => e.name.contains(want), orElse: () => list.first);
      final stock = _cfg['film'];
      if (stock is String && stock.isNotEmpty) app.applyEdit(photo.id, (e) => e.copyWith(film: stock));
      final g = EGroup.values.where((e) => e.name == _group).firstOrNull;
      OpenTiming.legacy = _cfg['legacy'] == true;
      OpenTiming.holdRealMs = (_cfg['holdReal'] as num?)?.toInt() ?? 0;
      OpenTiming.sink = (l) => File('$docs0/open_timing.txt').writeAsStringSync('$l\n', mode: FileMode.append, flush: true);
      openEditor(ctx, app, photo.id, initialGroup: g);
      if (_cfg['probeView'] == true) {
        Future<void>.delayed(const Duration(seconds: 8), () async {
          final b = await GpuViewBackend.start(640, 480);
          debugPrint('IosDebugOpen: probe GpuViewBackend.start -> ${b == null ? 'null' : 'texture ${b.textureId}'} supported=${GpuViewBackend.supported}');
          if (b != null) {
            // frames through the native view: a session on the sample, a few frames at 1080 long edge, counters from the view
            final docs = (await getApplicationDocumentsDirectory()).path;
            final sample = File('$docs/Photos/sample.jpg').existsSync() ? 'sample.jpg' : 'DAPPLE_sample_01.jpg';
            final ses = await DappleSession.open('$docs/Photos/$sample', longEdge: 2048);
            b.bindSession(ses.address);
            for (var i = 0; i < 6; i++) {
              b.submit({'look': 'std', 'lookStrength': 100, 'toneStrength': 60, 'exposure': -30.0 + i * 12}, longEdge: 1080, flags: 0);
              await Future<void>.delayed(const Duration(milliseconds: 400));
            }
            await Future<void>.delayed(const Duration(seconds: 1));
            debugPrint('IosDebugOpen: probe view info ${jsonEncode(b.info())}');
            File('$docs/probe_log.txt').writeAsStringSync('${jsonEncode({'textureId': b.textureId, 'info': b.info(), 'gpu': jsonDecode(DappleSession.gpuStatusJson() ?? '{}')})}\n');
            b.bindSession(0);
            await ses.close();
          }
          b?.dispose();
        });
      }
    } catch (e) {
      debugPrint('IosDebugOpen: $e');
    }
  }
}
