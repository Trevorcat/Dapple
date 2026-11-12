// The app ships copies of the film pack and its notices (assets/film/); they must stay identical to
// core/data/film (regenerate with: cp core/data/film/{dapple.dfp,NOTICE.md,SPEKTRAFILM_LICENSE.txt} app/assets/film/).
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  for (final f in ['dapple.dfp', 'NOTICE.md', 'SPEKTRAFILM_LICENSE.txt']) {
    test('assets/film/$f equals core/data/film/$f', () {
      final a = File('assets/film/$f'), b = File('../core/data/film/$f');
      expect(a.existsSync(), isTrue);
      expect(b.existsSync(), isTrue);
      expect(a.readAsBytesSync(), b.readAsBytesSync());
    });
  }

  test('the pack starts with the DFP magic and is declared in pubspec.yaml', () {
    final head = File('assets/film/dapple.dfp').openSync().readSync(4);
    expect(String.fromCharCodes(head), 'DFP2');
    final y = File('pubspec.yaml').readAsStringSync();
    expect(y, contains('assets/film/dapple.dfp'));
  });
}
