// Debug builds on iOS only (the simulator has no adb): with a file Documents/DAPPLE_OPEN present (its content = panel group) the app adds every file of Documents/Photos to the library, waits for the first one to be
// developed and opens the editor on it. The content (light / color / style / effects / crop) selects the panel category.
// Release builds never start this. The real photo flow on iOS is the picker import / the Files app (docs/ios/README.md).
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import '../model/params.dart';
import '../screens/editor/editor_page.dart';
import '../native/dapple_bindings.dart';
import '../state/app_state.dart';
import 'ios_services.dart';

class IosDebugOpen {
  static Timer? _timer;
  static bool _added = false, _opened = false;

  static void start(AppState app) {
    if (!kDebugMode || !Platform.isIOS || _timer != null) return;
    _timer = Timer.periodic(const Duration(milliseconds: 700), (_) => _tick(app));
  }

  static String _group = '';
  static bool _armed = false;

  static Future<void> _tick(AppState app) async {
    try {
      if (!_armed) {
        // armed by a file in Documents (simctl cannot pass environment variables reliably): Documents/DAPPLE_OPEN, the content is the group
        final f = File('${(await getApplicationDocumentsDirectory()).path}/DAPPLE_OPEN');
        if (!f.existsSync()) return;
        _group = (await f.readAsString()).trim();
        _armed = true;
      }
      debugPrint('IosDebugOpen: ready=${app.ready} added=$_added editable=${app.editable.length} engine=${DappleBindings.instanceOrNull != null} err=${DappleBindings.loadError}');
      if (!app.ready) return;
      if (!_added) {
        _added = true;
        app.ingestFtpFiles(await IosDocumentsSource().scan());
        return;
      }
      if (_opened) return;
      final list = app.editable;
      if (list.isEmpty) return;
      final nav = app.navigatorKey.currentState;
      final ctx = app.navigatorKey.currentContext;
      if (nav == null || ctx == null || !ctx.mounted) return;
      _opened = true;
      final photo = list.first;
      final g = EGroup.values.where((e) => e.name == _group).firstOrNull;
      openEditor(ctx, app, photo.id, initialGroup: g);
    } catch (e) {
      debugPrint('IosDebugOpen: $e');
    }
  }
}
