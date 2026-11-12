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
      openEditor(ctx, app, photo.id, initialGroup: g);
    } catch (e) {
      debugPrint('IosDebugOpen: $e');
    }
  }
}
