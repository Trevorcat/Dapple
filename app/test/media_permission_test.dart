import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';

import 'package:dapple/services/media_permission.dart';

void main() {
  test('permission by SDK level', () {
    for (final s in [24, 28, 29, 30, 31, 32]) {
      expect(mediaPermissionForSdk(s), Permission.storage, reason: 'API $s');
    }
    for (final s in [33, 34, 35]) {
      expect(mediaPermissionForSdk(s), Permission.photos, reason: 'API $s');
    }
  });

  test('granted and limited both count', () {
    expect(mediaStatusGrants(PermissionStatus.granted), isTrue);
    expect(mediaStatusGrants(PermissionStatus.limited), isTrue);
    expect(mediaStatusGrants(PermissionStatus.denied), isFalse);
    expect(mediaStatusGrants(PermissionStatus.permanentlyDenied), isFalse);
  });

  for (final sdk in [24, 29, 31, 32, 33, 34, 35]) {
    test('API $sdk asks the right permission for status and request', () async {
      final asked = <Permission>[];
      final mp = MediaPermission(
        sdkInt: () async => sdk,
        status: (p) async { asked.add(p); return PermissionStatus.denied; },
        request: (p) async { asked.add(p); return sdk >= 34 ? PermissionStatus.limited : PermissionStatus.granted; },
      );
      expect(await mp.isGranted(), isFalse);
      expect(await mp.request(), isTrue);
      final want = sdk >= 33 ? Permission.photos : Permission.storage;
      expect(asked, [want, want]);
    });
  }
}
