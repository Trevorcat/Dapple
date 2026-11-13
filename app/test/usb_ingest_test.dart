import 'dart:io';

import 'package:dapple/platform/usb_camera.dart';
import 'package:dapple/services/ingest.dart';
import 'package:dapple/services/usb_ingest.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/fake_usb.dart';

void main() {
  late FakeUsbBackend cam;
  late UsbIngest usb;
  final started = <String>[];
  final ended = <String>[];
  final got = <IncomingFile>[];

  setUp(() async {
    cam = FakeUsbBackend();
    usb = UsbIngest(cam, unpluggedFor: const Duration(milliseconds: 30));
    started.clear();
    ended.clear();
    got.clear();
    usb.onArrivalStart = started.add;
    usb.onArrivalEnd = (n, ok) => ended.add('$n:$ok');
    usb.onFiles = got.addAll;
    await usb.start('/inbox');
  });
  tearDown(() => usb.dispose());

  test('no camera: the card stays out of the way', () {
    expect(usb.phase, UsbPhase.none);
    expect(usb.cardActive, isFalse);
    expect(cam.started, isTrue);
  });

  test('card states in plain Chinese, no protocol words', () {
    cam.emit({'type': 'state', 'state': 'permission'});
    expect((usb.cardTitle, usb.cardLine, usb.cardAction), ('相机已插入', '等待允许访问', '允许'));
    cam.emit({'type': 'state', 'state': 'opening'});
    expect(usb.cardTitle, '相机 · 正在连接');
    cam.ready();
    expect(usb.cardTitle, 'Sony ILCE-7RM5 · 已连接');
    expect(usb.cardLine, '约 1234 个文件 · 自动接收');
    expect(usb.cardAction, '导入照片…');
    final all = '${usb.cardTitle}${usb.cardLine}${usb.cardAction}';
    expect(all, isNot(contains('MTP')));
    expect(all, isNot(contains('PTP')));
  });

  test('error reasons, each with a next step', () {
    String line(String r) {
      cam.emit({'type': 'state', 'state': 'error', 'reason': r});
      return '${usb.cardLine}|${usb.cardAction}';
    }

    expect(line('permission_denied'), '未允许访问相机|允许');
    expect(line('busy'), '相机被占用|重试');
    expect(line('open_failed'), '相机没有响应|重试');
    expect(line('no_storage'), '读不到存储卡|重试');
    expect(line('card_unreadable'), '读不到卡里的照片|重试');
    expect(usb.cardTitle, '相机 · 连接失败');
  });

  test('a new shot is imported on its own, JPEG first, and shows as received', () {
    cam.ready();
    cam.emit({
      'type': 'newShots',
      'total': 1236,
      'groups': [
        camGroup('DSC00002', [camFile(7, 'DSC00002.JPG'), camFile(6, 'DSC00002.ARW', size: 500)])
      ],
    });
    expect(cam.imports, [
      [7, 6]
    ]);
    expect(usb.importing, isTrue);
    expect(usb.cardLine, '正在接收第 1 张 · 共 1 张');
    cam.emit({'type': 'importStart', 'name': 'DSC00002.JPG', 'key': 'usb:dsc00002.jpg|100|1700000000', 'size': 100, 'index': 1, 'total': 1});
    expect(started, ['DSC00002.JPG']);
    cam.emit({'type': 'importProgress', 'name': 'DSC00002.JPG', 'bytes': 50, 'size': 100});
    expect(usb.cardProgress, closeTo(0.5, 1e-9)); // half of the one queued photo
    cam.emit({
      'type': 'importDone',
      'key': 'usb:dsc00002.jpg|100|1700000000',
      'name': 'DSC00002.JPG',
      'outName': 'DSC00002.JPG',
      'path': '/nonexistent/DSC00002.JPG',
      'dateMs': 1700000000000,
      'size': 100
    });
    expect(got.single.id, 'usb:dsc00002.jpg|100|1700000000');
    expect(got.single.path, '/nonexistent/DSC00002.JPG');
    expect(usb.importing, isTrue, reason: 'the RAW of the same shot is still coming');
    cam.emit({
      'type': 'importDone',
      'key': 'usb:dsc00002.arw|500|1700000000',
      'name': 'DSC00002.ARW',
      'outName': 'DSC00002.ARW',
      'path': '/x/DSC00002.ARW',
      'dateMs': 1700000000000
    });
    expect(usb.importing, isFalse);
    expect(usb.received, 1);
    cam.emit({'type': 'importQueueDone', 'imported': 2, 'failed': 0, 'cancelled': false});
    expect(usb.cardLine, '收到 DSC00002 · 共 1 张');
    expect(ended, ['DSC00002.JPG:true', 'DSC00002.ARW:true']);
  });

  test('what the library already has is never queued again', () {
    cam.ready();
    usb.isKnown = (f) => f.name == 'DSC00001.JPG';
    cam.emit({
      'type': 'newShots',
      'groups': [
        camGroup('DSC00001', [camFile(1, 'DSC00001.JPG')])
      ]
    });
    expect(cam.imports, isEmpty);
    expect(usb.importing, isFalse);
    // the same file announced twice in one session (event and poll) is queued once
    for (var i = 0; i < 2; i++) {
      cam.emit({
        'type': 'newShots',
        'groups': [
          camGroup('DSC00003', [camFile(3, 'DSC00003.JPG')])
        ]
      });
    }
    expect(cam.imports.length, 1);
  });

  test('the picker imports only what is picked and counts photos, not files', () {
    cam.ready();
    final g = [
      UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.ARW'), camFile(2, 'DSC00001.JPG')])),
      UsbGroup.fromMap(camGroup('DSC00002', [camFile(3, 'DSC00002.JPG')])),
    ];
    expect(usb.importGroups(g), 2);
    expect(usb.queuedPhotos, 2);
    expect(cam.imports.single, [1, 2, 3]);
    usb.cancelImport();
    expect(cam.cancels, 1);
  });

  test('a failed file is counted and may be tried again by a later pick', () {
    cam.ready();
    final g = UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.JPG')]));
    usb.importGroups([g]);
    cam.emit({'type': 'importFailed', 'key': 'usb:dsc00001.jpg|100|1700000000', 'name': 'DSC00001.JPG', 'reason': 'failed'});
    expect(usb.failed, 1);
    cam.emit({'type': 'importQueueDone', 'imported': 0, 'failed': 1});
    expect(usb.cardLine, '1 个文件没收到 · 共收到 0 张');
    expect(usb.importGroups([g]), 1);
  });

  test('not enough space on the phone stops with a plain line', () {
    cam.ready();
    usb.importGroups([UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.ARW')]))]);
    cam.emit({'type': 'importFailed', 'key': 'usb:dsc00001.arw|100|1700000000', 'name': 'DSC00001.ARW', 'reason': 'space'});
    cam.emit({'type': 'importQueueDone', 'imported': 0, 'failed': 1, 'aborted': 'space'});
    expect(usb.cardLine, '手机空间不足 · 已停止');
    expect(usb.importing, isFalse);
  });

  test('unplugged mid-import: the card says so, the queue is dropped, the card clears itself, re-plug starts clean', () async {
    cam.ready();
    usb.importGroups([UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.ARW')]))]);
    cam.emit({'type': 'importStart', 'name': 'DSC00001.ARW', 'key': 'usb:dsc00001.arw|100|1700000000', 'size': 100});
    cam.emit({'type': 'importFailed', 'key': 'usb:dsc00001.arw|100|1700000000', 'name': 'DSC00001.ARW', 'reason': 'gone'});
    cam.emit({'type': 'state', 'state': 'unplugged'});
    expect(usb.cardTitle, 'Sony ILCE-7RM5 · 已拔出');
    expect(usb.importing, isFalse);
    expect(usb.failed, 0, reason: 'a pulled cable is not a failed file');
    expect(ended, ['DSC00001.ARW:false']);
    await Future<void>.delayed(const Duration(milliseconds: 80));
    expect(usb.phase, UsbPhase.none);
    cam.ready(total: 99);
    expect(usb.received, 0);
    expect(usb.cardLine, '约 99 个文件 · 自动接收');
    // the same file is offered again after re-plugging and is queued again
    expect(usb.importGroups([UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.ARW')]))]), 1);
  });

  test('capture time becomes the added time unless the camera clock is far in the future', () {
    cam.ready();
    usb.importGroups([UsbGroup.fromMap(camGroup('A', [camFile(1, 'A.JPG')])), UsbGroup.fromMap(camGroup('B', [camFile(2, 'B.JPG', sec: 4102444800)]))]);
    for (final n in ['A.JPG', 'B.JPG']) {
      final sec = n == 'A.JPG' ? 1700000000 : 4102444800;
      cam.emit({'type': 'importDone', 'key': 'usb:${n.toLowerCase()}|100|$sec', 'name': n, 'path': '/x/$n', 'dateMs': sec * 1000});
    }
    expect(got[0].addedMs, 1700000000000);
    expect(got[1].addedMs, closeTo(DateTime.now().millisecondsSinceEpoch, 5000));
  });

  group('library identity', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('usbk'));
    tearDown(() => dir.deleteSync(recursive: true));

    test('same key, or the same name and size as a photo that came over FTP, is the same photo', () {
      final app = AppState.demo()..nativeOk = false;
      final ftp = File('${dir.path}/DSC00012.ARW')..writeAsBytesSync(List.filled(40, 1));
      final ftpJpg = File('${dir.path}/DSC00012.JPG')..writeAsBytesSync(List.filled(10, 2));
      app.ingestFtpFiles([
        IncomingFile(id: 'ftp:DSC00012.ARW', name: 'DSC00012.ARW', addedMs: 1, path: ftp.path, size: 40),
        IncomingFile(id: 'ftp:DSC00012.JPG', name: 'DSC00012.JPG', addedMs: 2, path: ftpJpg.path, size: 10),
      ]);
      UsbFile u(String n, int size, {String? key}) =>
          UsbFile(handle: 1, name: n, size: size, dateMs: 5, key: key ?? 'usb:${n.toLowerCase()}|$size|0', kind: 'raw');
      expect(app.usbKnown(u('DSC00012.ARW', 40)), isTrue);
      expect(app.usbKnown(u('dsc00012.arw', 40)), isTrue, reason: 'case does not matter');
      expect(app.usbKnown(u('DSC00012.JPG', 10)), isTrue, reason: 'the JPEG was paired into the RAW photo');
      expect(app.usbKnown(u('DSC00012.ARW', 41)), isFalse, reason: 'another size is another file');
      expect(app.usbKnown(u('DSC00013.ARW', 40)), isFalse);
      final usbRaw = File('${dir.path}/DSC00020.ARW')..writeAsBytesSync(List.filled(7, 3));
      app.ingestFtpFiles([IncomingFile(id: 'usb:dsc00020.arw|7|9', name: 'DSC00020.ARW', addedMs: 3, path: usbRaw.path, size: 7)]);
      expect(app.usbKnown(u('DSC00020.ARW', 7, key: 'usb:dsc00020.arw|7|9')), isTrue);
    });

    test('RAW and JPEG that came over the cable become one photo, whichever arrives first', () {
      final app = AppState.demo()..nativeOk = false;
      final before = app.photos.length;
      app.ingestFtpFiles([IncomingFile(id: 'usb:p.jpg|1|1', name: 'P.JPG', addedMs: 1, path: '${dir.path}/P.JPG', size: 1)]);
      app.ingestFtpFiles([IncomingFile(id: 'usb:p.arw|2|1', name: 'P.ARW', addedMs: 2, path: '${dir.path}/P.ARW', size: 2)]);
      expect(app.photos.length, before + 1);
      expect(app.photos.where((p) => p.name == 'P.ARW').single.pairedJpeg, '${dir.path}/P.JPG');
    });
  });
}
