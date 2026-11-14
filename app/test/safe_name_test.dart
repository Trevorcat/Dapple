import 'dart:io';

import 'package:dapple/models.dart';
import 'package:dapple/services/safe_name.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('old Android names are unchanged', () {
    for (final id in ['ms:123', 'ftp:DSC00123.ARW', 'cap:20240101_a-b.c', 'doc:sample.jpg', 'ftp:arr:X_1']) {
      expect(safeFileStem(id), id.replaceAll(':', '_'));
    }
  });
  test('iOS identifiers lose their slashes', () {
    final s = safeFileStem('ph:9F983DBA-EC35-42B8-8773-B597CF782EDD/L0/001:0');
    expect(s, isNot(contains('/')));
    expect(s, matches(RegExp(r'^[A-Za-z0-9._-]+$')));
  });
  test('different ids never share a name', () {
    expect(safeFileStem('a/b'), isNot(safeFileStem('a_b')));
    expect(safeFileStem('a/b'), isNot(safeFileStem('a b')));
    expect(safeFileStem('照片1'), isNot(safeFileStem('照片2')));
    expect(safeFileStem('x' * 300), isNot(safeFileStem('${'x' * 299}y')));
  });
  test('unicode, long, empty, dots', () {
    expect(safeFileStem('照片.jpg'), matches(RegExp(r'^[A-Za-z0-9._-]+$')));
    expect(safeFileStem('a' * 500).length, lessThanOrEqualTo(140));
    expect(safeFileStem(''), isNotEmpty);
    expect(safeFileStem('..'), isNot('..'));
  });
  test('hash is stable across runs', () {
    expect(fnv1aHex('a'), 'e40c292c');
    expect(safeFileStem('a/b'), safeFileStem('a/b'));
  });
  test('safeFileName keeps the extension', () {
    expect(safeFileName('IMG 1.HEIC'), endsWith('.HEIC'));
    expect(safeFileName('a/b/IMG_1.dng'), 'IMG_1.dng');
  });
  test('previewPathFor stays directly inside previews', () {
    final app = AppState();
    app.previews = Directory.systemTemp.createTempSync('prev_');
    final ph = Photo(id: 'ph:9F983DBA-EC35-42B8-8773-B597CF782EDD/L0/001:0', name: 'a.heic', addedMs: 1);
    final path = app.previewPathFor(ph);
    expect(File(path).parent.path, app.previews.path);
  });
  test('engine write failure reads as plain failure', () {
    final ph = Photo(id: 'x', name: 'a', addedMs: 1)..error = 'DappleException: Cannot write 001_0.jpg';
    expect(ph.failReason, '没处理成功');
  });
}
