import 'package:flutter_test/flutter_test.dart';
import 'package:dapple/services/blank_check.dart';

void main() {
  test('all-black RGBA is blank', () {
    expect(isBlankRgba(List.filled(16 * 4, 0)), isTrue);
  });
  test('one lit pixel is not blank', () {
    final b = List.filled(16 * 4, 0);
    b[20] = 120;
    expect(isBlankRgba(b), isFalse);
  });
}
