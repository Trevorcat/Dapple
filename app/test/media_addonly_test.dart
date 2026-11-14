import 'package:dapple/services/media_permission.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('add-only: the card does not say refused for good, the action is 打开设置', () async {
    var opened = 0;
    final app = AppState()
      ..ready = true
      ..permissionPlatform = true
      ..mediaPerm = MediaPermission(openSettings: () async { opened++; return true; });
    app.mediaAccess = MediaAccess.addOnly;
    app.mediaPermitted = false;
    expect(app.mediaAddOnly, isTrue);
    expect(app.mediaBlocked, isFalse);
    if (app.needsMediaPermission) {
      expect(app.connLine, isNot('已被系统拒绝'));
      expect(app.connLine, contains('存入'));
      expect(app.connAction, '打开设置');
    }
    await app.requestPermission();
    expect(opened, 1);
  });
}
