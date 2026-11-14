// Chinese typesetting lint (guidelines 3.2): no weight above 600 anywhere in lib; 11 px only for digits / Latin.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// `path:line` -> why a weight above 600 is justified. Empty: there is no such case.
const _heavyAllow = <String, String>{};

void main() {
  final files = Directory('lib').listSync(recursive: true).whereType<File>().where((f) => f.path.endsWith('.dart')).toList();

  test('no FontWeight above 600 in lib (3.2 rule 1)', () {
    final heavy = RegExp(r'FontWeight\.(w700|w800|w900|bold)|FontWeight\.values\[[6-8]\]');
    final bad = <String>[];
    for (final f in files) {
      final lines = f.readAsLinesSync();
      for (var i = 0; i < lines.length; i++) {
        final l = lines[i];
        if (l.trimLeft().startsWith('//')) continue;
        final path = f.path.replaceAll('\\', '/');
        final key = '$path:${i + 1}';
        if (heavy.hasMatch(l) && !_heavyAllow.containsKey(key)) bad.add('$key: ${l.trim()}');
      }
    }
    expect(bad, isEmpty, reason: bad.join('\n'));
  });

  test('11 px text carries no Chinese (3.2 rule 3)', () {
    final han = RegExp(r'[一-鿿]');
    final small = RegExp(r'T\.mini\(|fontSize:\s*(11|10|9)(\.0)?\b');
    final bad = <String>[];
    for (final f in files) {
      final lines = f.readAsLinesSync();
      for (var i = 0; i < lines.length; i++) {
        final l = lines[i];
        if (l.trimLeft().startsWith('//')) continue;
        final code = l.split('//').first;
        if (small.hasMatch(code) && han.hasMatch(code)) bad.add('${f.path}:${i + 1}: ${l.trim()}');
      }
    }
    expect(bad, isEmpty, reason: bad.join('\n'));
  });
}
