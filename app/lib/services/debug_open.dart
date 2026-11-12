// Debug builds only: opens the editor on a given photo with a given category and parameter selected, so emulator
// screenshots do not depend on blind taps. Push a JSON file into the app's external files folder
//   adb push debug_open.json /sdcard/Android/data/com.dapple.dapple/files/debug_open.json
// with  {"photo":"<file name substring or index>","group":"light","row":"exposure","collapsed":false,"stock":"kodak_portra_400"}
// (group: an EGroup name; row: a parameter row id; both optional; {"close":true} goes back to the library).
// The file is read once a second and deleted when it has been applied. Release builds never start this.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../model/params.dart';
import '../models.dart';
import '../screens/editor/editor_page.dart';
import '../state/app_state.dart';

class DebugOpen {
  static Timer? _timer;

  static void start(AppState app) {
    if (!kDebugMode || !Platform.isAndroid || _timer != null) return;
    _timer = Timer.periodic(const Duration(seconds: 1), (_) => _poll(app));
  }

  static Future<void> _poll(AppState app) async {
    try {
      final dir = await getExternalStorageDirectory();
      if (dir == null) return;
      final f = File(p.join(dir.path, 'debug_open.json'));
      if (!f.existsSync()) return;
      final text = await f.readAsString();
      await f.delete();
      final j = jsonDecode(text) as Map<String, dynamic>;
      final nav = app.navigatorKey.currentState;
      final ctx = app.navigatorKey.currentContext;
      if (nav == null || ctx == null) return;
      nav.popUntil((r) => r.isFirst);
      if (j['close'] == true) return;
      final list = app.editable;
      final sel = j['photo'];
      Photo? photo;
      if (sel is int && sel >= 0 && sel < list.length) photo = list[sel];
      if (sel is String) {
        for (final e in list) {
          if (e.name.contains(sel)) {
            photo = e;
            break;
          }
        }
      }
      photo ??= list.isEmpty ? null : list.first;
      if (photo == null) return;
      final g = j['group'] is String ? EGroup.values.where((e) => e.name == j['group']).firstOrNull : null;
      final stock = j['stock'];
      if (stock is String) app.applyEdit(photo.id, (e) => e.copyWith(film: stock.isEmpty ? null : stock));
      if (!ctx.mounted) return;
      openEditor(ctx, app, photo.id, initialGroup: g, initialRow: j['row'] as String?, initialCollapsed: j['collapsed'] == true);
    } catch (e) {
      debugPrint('debug_open failed: $e');
    }
  }
}
