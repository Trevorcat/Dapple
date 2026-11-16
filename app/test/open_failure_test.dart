import 'dart:io';

import 'package:dapple/models.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/services/editor_engine.dart';
import 'package:dapple/services/library_prune.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;

import 'support/harness.dart';

Photo _raw(String id, {String? path, PhotoStatus status = PhotoStatus.done, PhotoKind kind = PhotoKind.photo}) =>
    Photo(id: id, name: '$id.raf', addedMs: 1, path: path, status: status, kind: kind);

void main() {
  test('open errors are told apart: gone against unreadable', () {
    expect(classifySourceError('找不到原始文件'), SourceFailure.gone);
    expect(classifySourceError(const PathNotFoundException('/x', OSError('No such file or directory', 2))), SourceFailure.gone);
    expect(classifySourceError('java.io.FileNotFoundException: /sdcard/a.arw'), SourceFailure.gone);
    expect(classifySourceError('cannot decode'), SourceFailure.unreadable);
    expect(classifySourceError('Permission denied'), SourceFailure.unreadable);
  });

  testWidgets('the session cache remembers why an open failed, and forgets it on the next try', (tester) async {
    final app = AppState.demo();
    final ph = _raw('a');
    app.sessions.testAvailable = true;
    var err = '找不到原始文件';
    app.sessions.testOpener = (_) async => throw err;
    expect(await app.sessions.get(ph), isNull);
    expect(app.sessions.failureOf('a'), SourceFailure.gone);
    err = 'cannot decode';
    expect(await app.sessions.get(ph), isNull);
    expect(app.sessions.failureOf('a'), SourceFailure.unreadable);
    app.sessions.testOpener = (_) async => null; // a null that is not an error leaves no failure behind
    await app.sessions.get(ph);
    expect(app.sessions.failureOf('a'), isNull);
  });

  testWidgets('the engine stops waiting when the source is gone', (tester) async {
    final app = AppState.demo();
    final ph = _raw('a');
    app.simulatePhotoForTest(ph);
    app.sessions.testAvailable = true;
    app.sessions.testOpener = (_) async => throw '找不到原始文件';
    final engine = EditorEngine(app);
    engine.attach(ph, neighbors: const []);
    expect(engine.previewOnly, isTrue);
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 1200)));
    await tester.pump(const Duration(milliseconds: 100));
    expect(engine.openFailure, SourceFailure.gone);
    expect(engine.previewOnly, isFalse, reason: 'no spinner once the open has failed');
    engine.dispose();
    await tester.pump(const Duration(seconds: 5));
  });

  for (final gone in [true, false]) {
    testWidgets('the editor says so and offers 从图库移除 (${gone ? 'source gone' : 'cannot decode'})', (tester) async {
      useWindow(tester, phoneSize);
      final app = AppState.demo();
      final ph = _raw('a');
      app.simulatePhotoForTest(ph);
      app.sessions.testAvailable = true;
      app.sessions.testOpener = (_) async => throw (gone ? '找不到原始文件' : 'cannot decode');
      await pumpDapple(tester, app);
      openEditor(tester.element(find.byType(Scaffold).first), app, ph.id);
      await tester.pump(const Duration(milliseconds: 400));
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 1200)));
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const ValueKey('open-failed')), findsOneWidget);
      expect(find.text(gone ? '原文件已不在这台设备上' : '这张照片无法打开'), findsOneWidget);
      expect(find.byKey(const ValueKey('preview-busy')), findsNothing, reason: 'the spinner is gone');
      expect(find.text('从图库移除'), findsOneWidget);
      await tester.tap(find.text('从图库移除'));
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      expect(app.photoById(ph.id), isNull, reason: 'the entry is removed once the editor has left');
      expect(find.byType(EditorPage), findsNothing);
      await tester.pump(const Duration(seconds: 5));
    });
  }

  group('library pruning', () {
    late Directory tmp;
    setUp(() => tmp = Directory.systemTemp.createTempSync('dapple_prune_'));
    tearDown(() {
      try {
        tmp.deleteSync(recursive: true);
      } catch (_) {}
    });

    test('only a certain "no such file" in an existing folder counts', () {
      final here = File(p.join(tmp.path, 'here.raf'))..writeAsBytesSync([1]);
      final dead = p.join(tmp.path, 'dead.raf');
      final inMissingDir = p.join(tmp.path, 'no_such_dir', 'x.raf');
      final r = missingSourceIdsSync([
        (id: 'here', paths: [here.path]),
        (id: 'dead', paths: [dead]),
        (id: 'nodir', paths: [inMissingDir]), // a folder that is not there (card out): cannot tell
        (id: 'moved', paths: [dead, here.path]), // found at the second place
      ]);
      expect(r, {'dead'});
    });

    test('the volume that must be present for a missing file to count', () {
      expect(volumeRootOf('/storage/emulated/0/Pictures/X/a.arw'), '/storage/emulated/0');
      expect(volumeRootOf('/sdcard/DCIM/a.arw'), '/sdcard');
      expect(volumeRootOf('/storage/1A2B-3C4D/DCIM/a.arw'), '/storage/1A2B-3C4D');
      expect(volumeRootOf('/data/user/0/app/files/a.arw'), '/data/user/0/app/files');
    });

    test('a path that exists but cannot be opened as a file is not pruned', () {
      final d = Directory(p.join(tmp.path, 'is_a_dir.raf'))..createSync();
      expect(missingSourceIdsSync([(id: 'd', paths: [d.path])]), isEmpty);
    });

    Future<AppState> app0() async {
      final app = AppState()..useFoldersForTest(tmp);
      Directory(p.join(tmp.path, 'inbox')).createSync();
      return app;
    }

    test('the library drops dead entries and their thumbnails, keeps everything it cannot be sure about', () async {
      final app = await app0();
      final live = File(p.join(tmp.path, 'live.raf'))..writeAsBytesSync([1]);
      final thumb = File(p.join(tmp.path, 'dead_thumb.jpg'))..writeAsBytesSync([1]);
      final dead = p.join(tmp.path, 'dead.raf');
      app.simulatePhotoForTest(_raw('live', path: live.path));
      app.simulatePhotoForTest(_raw('dead', path: dead)..previewPath = thumb.path);
      app.simulatePhotoForTest(_raw('failed', path: dead, status: PhotoStatus.failed));
      app.simulatePhotoForTest(_raw('arriving', path: dead, status: PhotoStatus.arriving)); // import in progress
      app.simulatePhotoForTest(_raw('pending', path: dead, status: PhotoStatus.pending)); // queued for development
      app.simulatePhotoForTest(_raw('uri_only')); // no path to look at
      app.simulatePhotoForTest(_raw('card', path: p.join(tmp.path, 'sd', 'c.raf'))); // its folder is not there
      app.simulatePhotoForTest(_raw('video', path: dead, kind: PhotoKind.video));
      app.simulatePhotoForTest(_raw('ftp:arr:1', path: dead));
      expect(await app.pruneMissingSources(), 2);
      for (final keep in ['live', 'arriving', 'pending', 'uri_only', 'card', 'video', 'ftp:arr:1']) {
        expect(app.photoById(keep), isNotNull, reason: keep);
      }
      expect(app.photoById('dead'), isNull);
      expect(app.photoById('failed'), isNull);
      expect(thumb.existsSync(), isFalse, reason: 'the thumbnail goes with the entry');
      expect(live.existsSync(), isTrue);
      await Future<void>.delayed(const Duration(milliseconds: 700)); // the library file is written
    });

    test('an entry found again in the inbox (the app folder moved) is kept', () async {
      final app = await app0();
      File(p.join(tmp.path, 'inbox', 'moved.raf')).writeAsBytesSync([1]);
      app.simulatePhotoForTest(_raw('moved', path: p.join(tmp.path, 'old_container', 'moved.raf')));
      expect(await app.pruneMissingSources(), 0);
      expect(app.photoById('moved'), isNotNull);
    });

    test('the photo the editor has open is not pruned', () async {
      final app = await app0();
      app.simulatePhotoForTest(_raw('dead', path: p.join(tmp.path, 'dead.raf')));
      app.sessions.testAvailable = true;
      app.sessions.testOpener = (_) async => null;
      await app.sessions.get(app.photoById('dead')!); // the editor claims it
      expect(await app.pruneMissingSources(), 0);
      expect(app.photoById('dead'), isNotNull);
      await app.sessions.closeAll();
      expect(await app.pruneMissingSources(), 1);
      await Future<void>.delayed(const Duration(milliseconds: 700));
    });

    test('nothing is pruned while an import is running', () async {
      final app = await app0();
      app.simulatePhotoForTest(_raw('dead', path: p.join(tmp.path, 'dead.raf')));
      app.importing = true;
      expect(await app.pruneMissingSources(), 0);
      expect(app.photoById('dead'), isNotNull);
      app.importing = false;
      expect(await app.pruneMissingSources(), 1);
      await Future<void>.delayed(const Duration(milliseconds: 700));
    });
  });
}
