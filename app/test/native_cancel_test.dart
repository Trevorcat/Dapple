import 'dart:io';

import 'package:dapple/native/dapple_bindings.dart';
import 'package:dapple/native/dapple_session.dart';
import 'package:flutter_test/flutter_test.dart';

/// The Dart side of `dapple_session_cancel` (latest-wins cancellation, pipeline-architecture.md 5.8).
///
/// The binding test needs a host build of the engine: set DAPPLE_LIB to the host build of libdapple (build/NAME/bin/libdapple.dll, libdapple.so). Without
/// it the symbol tests are skipped; the guard tests (closed session, no library) always run.
void main() {
  final lib = Platform.environment['DAPPLE_LIB'];
  final haveLib = lib != null && lib.isNotEmpty && File(lib).existsSync();

  test('cancel() on a closed session does nothing and reports false', () {
    final s = DappleSession.forTest(0x1234, closed: true);
    expect(s.cancel(), isFalse);
  });

  test('cancel() never throws when the library is missing', () {
    if (haveLib) return; // covered by the bound tests below
    expect(DappleSession.forTest(0).cancel(), isFalse);
  });

  group('bound to the host engine', () {
    test('dapple_session_cancel is bound and rejects a null session', () {
      final b = DappleBindings.instanceOrNull;
      expect(b, isNotNull, reason: DappleBindings.loadError?.toString());
      expect(b!.sessionCancel, isNotNull, reason: 'the symbol must be exported by the library');
      // an open session cannot be built without a photo here: the C function reports an error for a null session (not a crash)
      expect(DappleSession.forTest(0).cancel(), isFalse);
    });
  }, skip: haveLib ? false : 'set DAPPLE_LIB to a host libdapple build');
}
