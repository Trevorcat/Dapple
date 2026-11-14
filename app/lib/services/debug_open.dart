// Debug builds only: opens the editor on a given photo with a given category and parameter selected, so emulator
// screenshots do not depend on blind taps. Push a JSON file into the app's external files folder
//   adb push debug_open.json /sdcard/Android/data/com.dapple.dapple/files/debug_open.json
// with  {"photo":"<file name substring or index>","group":"light","row":"exposure","collapsed":false,"stock":"kodak_portra_400"}
// (group: an EGroup name, the panel's categories being light / color / style (= 胶片) / effects (= 质地) / crop; row: the panel
// layer - items, map, curve, hsl, grade, paper; "look":"warm" selects a style; "pad":{"light":[x,y,a]} sets a pad state before opening (x, y in -1..1,
// a in 0..1, 0.5 = 100 %); "hold":"light" shows the hold popup; all optional; {"close":true} goes back to the library; {"purgeCaptures":true} removes the
// in-app capture test shots from the library; {"zoom":[scale,x,y]} zooms about a screen point; {"import":["dbg/a.jpg"]} imports files pushed under the external files folder, removed again by purgeCaptures).
// The file is read once a second and deleted when it has been applied. Release builds never start this.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../model/pad_model.dart';
import '../model/params.dart';
import '../models.dart';
import '../native/dapple_session.dart';
import '../native/memory_bindings.dart';
import 'ingest.dart';
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
      if (j['memReport'] == true || j['memTrim'] is int) {
        // {"memTrim":0|1|2} runs dapple_memory_trim at that level, {"memReport":true} prints dapple_memory_report_json (memory measurements)
        final lv = j['memTrim'];
        if (lv is int) debugPrint('debug_open: memTrim $lv released ${(await MemoryBindings.trim(lv)) >> 20} MB');
        debugPrint('debug_open: memReport ${jsonEncode(MemoryBindings.report())}');
        return;
      }
      if (j['convert'] is String) {
        // {"convert":"/abs/photo.heic"}: the platform conversion the library does for such files, then an engine session on the result
        // (logs the colour space the engine detected); the PNG stays in the external files folder as converted.png
        final t = Stopwatch()..start();
        final out = await MediaBridge.toPng16(j['convert'] as String, p.join(dir.path, 'converted.png'));
        debugPrint('debug_open: converted to $out (${File(out).lengthSync()} bytes, ${t.elapsedMilliseconds} ms)');
        final sess = await DappleSession.open(out, longEdge: 2048);
        debugPrint('debug_open: engine info ${await sess.info()}');
        await sess.close();
        return;
      }
      final imp = j['import'];
      if (imp is List) {
        // {"import":["dbg/p1.jpg", ...]} (paths under the external files folder, pushed with adb): imported like a pick from the photo picker.
        // Their ids start with "cap:" so {"purgeCaptures":true} removes them (and their files) again.
        final now = DateTime.now().millisecondsSinceEpoch;
        final items = <PickedItem>[
          for (final x in imp.whereType<String>())
            if (File(p.join(dir.path, x)).existsSync())
              PickedItem(
                key: 'cap:dbg:${p.basename(x)}',
                name: p.basename(x),
                file: IncomingFile(id: 'cap:dbg:${p.basename(x)}', name: p.basename(x), addedMs: now, path: p.join(dir.path, x), size: File(p.join(dir.path, x)).lengthSync()),
              ),
        ];
        final old = app.pickBackend;
        app.pickBackend = (_, _) async => items;
        try {
          await app.importFromPicker();
        } finally {
          app.pickBackend = old;
        }
        debugPrint('debug_open: imported ${items.length}');
        return;
      }
      final nav = app.navigatorKey.currentState;
      final ctx = app.navigatorKey.currentContext;
      if (nav == null || ctx == null) return;
      nav.popUntil((r) => r.isFirst);
      if (j['purgeCaptures'] == true) debugPrint('debug_open: purged ${app.purgeCaptures()} captures');
      if (j['close'] == true || j['purgeCaptures'] == true) return;
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
      if (j['reset'] == true) app.applyEdit(photo.id, (_) => EditState.empty);
      final stock = j['stock'];
      if (stock is String) app.applyEdit(photo.id, (e) => e.copyWith(film: stock.isEmpty ? null : stock));
      final look = j['look'];
      if (look is String) app.applyEdit(photo.id, (e) => e.copyWith(look: look.isEmpty ? null : look));
      final pads = j['pad'];
      if (pads is Map) {
        final ctx = PadCtx(defaultTone: app.settings.toneStrength, defaultLookId: app.settings.defaultLookId);
        for (final c in PadCat.values) {
          final v = pads[c.name];
          if (v is List && v.length >= 3) {
            app.applyEdit(photo.id, (e) {
              var n = e;
              if (c == PadCat.film && n.film == null && app.film.stocks.isNotEmpty) n = n.copyWith(film: app.film.stocks.first.id);
              return applyPadState(n, c, PadState(x: (v[0] as num).toDouble(), y: (v[1] as num).toDouble(), a: (v[2] as num).toDouble()), ctx);
            });
          }
        }
      }
      if (!ctx.mounted) return;
      final z = j['zoom'];
      final zoom = z is List && z.length >= 3 ? ((z[0] as num).toDouble(), (z[1] as num).toDouble(), (z[2] as num).toDouble()) : null;
      openEditor(ctx, app, photo.id, initialGroup: g, initialRow: j['row'] as String?, initialCollapsed: j['collapsed'] == true, initialHold: j['hold'] as String?, initialZoom: zoom);
    } catch (e) {
      debugPrint('debug_open failed: $e');
    }
  }
}
