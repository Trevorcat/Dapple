import 'package:dapple/native/dapple_session.dart';
import 'package:dapple/services/editor_engine.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('the touch hint is a harmless no-op without a session or without the native symbol', () {
    // address 0 = no session; and the test host has no native library, so the lookup yields null
    expect(DappleSession.touchHint(0, 80), isFalse);
    expect(DappleSession.touchHint(0, -1), isFalse);
    expect(DappleSession.touchHint(12345, 80), isFalse);
  });

  test('the burst is shorter than a human touch-down to first movement', () {
    expect(EditorEngine.touchHintBurstMs, inInclusiveRange(40, 120));
  });
}
