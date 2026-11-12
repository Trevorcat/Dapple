import 'dart:io';

import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';

/// Camera permission as the capture flow needs to tell it: [blocked] = only the system settings can grant it now.
enum CapturePerm { granted, denied, blocked }

/// Files of one shutter press: JPEG only, RAW + JPEG, or a DNG alone (RAW-only capture; the library develops the DNG itself).
class CaptureResult {
  const CaptureResult({this.jpegPath, this.dngPath, this.source = 'camerax'})
      : assert(jpegPath != null || dngPath != null, 'a capture has at least one file');
  final String? jpegPath;
  final String? dngPath;

  /// File name stem shared by the files of this shot.
  String get stem => _stemOf((jpegPath ?? dngPath)!);

  static String _stemOf(String path) {
    final name = path.split(RegExp(r'[\\/]')).last;
    final dot = name.lastIndexOf('.');
    return dot > 0 ? name.substring(0, dot) : name;
  }

  /// `camerax` (in-app capture) or `system` (ACTION_IMAGE_CAPTURE fallback).
  final String source;
}

/// Phone-camera capture (testing convenience, requirements 4.2.6). Tests substitute a fake.
abstract class CaptureBackend {
  bool get available;
  Future<CapturePerm> permission({bool request = false});
  Future<bool> openSettings();

  /// Opens the capture screen; null when the user closed it without a shot. Throws when the camera cannot be used.
  Future<CaptureResult?> capture();

  /// Copies the file into the phone gallery (Pictures/Dapple).
  Future<void> saveToGallery(String path, String name, String mime);
}

const MethodChannel _channel = MethodChannel('dapple/capture');

class ChannelCaptureBackend implements CaptureBackend {
  @override
  bool get available => Platform.isAndroid;

  @override
  Future<CapturePerm> permission({bool request = false}) async {
    var st = await Permission.camera.status;
    if (!st.isGranted && request) st = await Permission.camera.request();
    if (st.isGranted || st.isLimited) return CapturePerm.granted;
    if (st.isPermanentlyDenied || st.isRestricted) return CapturePerm.blocked;
    return CapturePerm.denied;
  }

  @override
  Future<bool> openSettings() => openAppSettings();

  @override
  Future<CaptureResult?> capture() async {
    final r = await _channel.invokeMapMethod<String, dynamic>('capture');
    final jpg = r?['jpg'] as String?;
    final dng = r?['dng'] as String?;
    if (r == null || (jpg == null && dng == null)) return null;
    return CaptureResult(jpegPath: jpg, dngPath: dng, source: (r['source'] as String?) ?? 'camerax');
  }

  @override
  Future<void> saveToGallery(String path, String name, String mime) async {
    await _channel.invokeMethod<String>('saveToGallery', {'path': path, 'name': name, 'mime': mime});
  }
}
