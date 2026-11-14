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

/// What the app can do about the photo library right now.
///  [full]/[limited] list photos (limited = the user picked some: iOS "选择照片", Android 14 "选择照片和视频");
///  [addOnly] iOS "仅添加": saving to the album works, listing does not; it is a choice the user can widen in Settings, not a refusal;
///  [askable] the system prompt can still be shown; [blocked] the user refused (or a policy forbids it): only the system Settings can change it.
enum MediaAccess { full, limited, addOnly, askable, blocked }

extension MediaAccessX on MediaAccess {
  bool get listing => this == MediaAccess.full || this == MediaAccess.limited;
}

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
    Future<bool> Function()? openSettings,
  })  : _openSettings = openSettings ?? openAppSettings,
        _sdkInt = sdkInt ?? _platformSdkInt,
        _status = status ?? ((p) => p.status),
        _request = request ?? ((p) => p.request());

  final Future<bool> Function() _openSettings;
  final Future<int> Function() _sdkInt;
  final Future<PermissionStatus> Function(Permission) _status;
  final Future<PermissionStatus> Function(Permission) _request;

  /// Maps a permission_handler status to what the UI needs. Android reports `denied` before the first ask and after one refusal
  /// (still askable) and `permanentlyDenied` after that; iOS `denied` is final (the system never prompts twice), `restricted` is a policy.
  static MediaAccess accessOfStatus(PermissionStatus st) {
    if (st.isGranted) return MediaAccess.full;
    if (st.isLimited) return MediaAccess.limited;
    if (st.isPermanentlyDenied || st.isRestricted) return MediaAccess.blocked;
    return MediaAccess.askable;
  }

  static MediaAccess accessOfIos(String? s) => switch (s) {
        'authorized' => MediaAccess.full,
        'limited' => MediaAccess.limited,
        'addOnly' => MediaAccess.addOnly,
        'denied' || 'restricted' => MediaAccess.blocked,
        _ => MediaAccess.askable,
      };

  Future<MediaAccess> level() async {
    if (Platform.isIOS) return accessOfIos(await _photos.invokeMethod<String>('authorizationStatus'));
    return accessOfStatus(await _status(mediaPermissionForSdk(await _sdkInt())));
  }

  /// Shows the system prompt (when it can be shown) and returns the access afterwards.
  Future<MediaAccess> requestLevel() async {
    if (Platform.isIOS) return accessOfIos(await _photos.invokeMethod<String>('requestAuthorization'));
    return accessOfStatus(await _request(mediaPermissionForSdk(await _sdkInt())));
  }

  /// "Choose more photos" while the access is limited: the iOS picker of the selection; on Android 14 the same permission request shows the picker again.
  Future<void> chooseMore() async {
    if (Platform.isIOS) return presentLimitedPicker();
    await _request(mediaPermissionForSdk(await _sdkInt()));
  }

  /// The app's page in the system Settings.
  Future<bool> openSettings() => _openSettings();

  Future<bool> isGranted() async {
    if (Platform.isIOS) return _iosGranted(await _photos.invokeMethod<String>('authorizationStatus'));
    return mediaStatusGrants(await _status(mediaPermissionForSdk(await _sdkInt())));
  }

  Future<bool> request() async {
    if (Platform.isIOS) return _iosGranted(await _photos.invokeMethod<String>('requestAuthorization'));
    return mediaStatusGrants(await _request(mediaPermissionForSdk(await _sdkInt())));
  }

  /// iOS (PhotoKit): full and limited access both let the app list photos; "limited" can be widened with [presentLimitedPicker].
  static bool _iosGranted(String? s) => s == 'authorized' || s == 'limited';
  static const MethodChannel _photos = MethodChannel('dapple/photos');
  static Future<String> iosStatus() async => await _photos.invokeMethod<String>('authorizationStatus') ?? 'notDetermined';
  static Future<void> presentLimitedPicker() async => _photos.invokeMethod<void>('presentLimitedLibraryPicker');
}
