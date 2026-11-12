import 'dart:io';

import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';

/// Which runtime permission gives read access to the photo library on an Android SDK level.
///
/// API 33+ has READ_MEDIA_IMAGES (permission_handler's `photos`; on 34+ it also reports partial access as limited);
/// below that the only permission is READ_EXTERNAL_STORAGE (`storage`). permission_handler resolves `photos` to an
/// empty list below 33 ("No permissions found in manifest for: []"), so it must not be asked there.
Permission mediaPermissionForSdk(int sdkInt) => sdkInt >= 33 ? Permission.photos : Permission.storage;

/// Whether a permission status lets the app list photos (full or the Android 14 partial selection).
bool mediaStatusGrants(PermissionStatus st) => st.isGranted || st.isLimited;

const MethodChannel _sdkChannel = MethodChannel('dapple/capture');

Future<int> _platformSdkInt() async {
  if (!Platform.isAndroid) return 0;
  try {
    return await _sdkChannel.invokeMethod<int>('sdkInt') ?? 33;
  } catch (_) {
    return 33;
  }
}

/// Photo-library permission across API 24-35; every platform call is injectable for tests.
class MediaPermission {
  MediaPermission({
    Future<int> Function()? sdkInt,
    Future<PermissionStatus> Function(Permission)? status,
    Future<PermissionStatus> Function(Permission)? request,
  })  : _sdkInt = sdkInt ?? _platformSdkInt,
        _status = status ?? ((p) => p.status),
        _request = request ?? ((p) => p.request());

  final Future<int> Function() _sdkInt;
  final Future<PermissionStatus> Function(Permission) _status;
  final Future<PermissionStatus> Function(Permission) _request;

  Future<bool> isGranted() async => mediaStatusGrants(await _status(mediaPermissionForSdk(await _sdkInt())));

  Future<bool> request() async => mediaStatusGrants(await _request(mediaPermissionForSdk(await _sdkInt())));
}
