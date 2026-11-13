// A camera on the phone's USB cable: the platform seam of the wired import (docs/research/usb-tethering.md step 1).
//
// Android: Kotlin UsbCameraPlugin (android.mtp.MtpDevice) behind dapple/usbcam + dapple/usbcam_events.
// iOS (not built yet) must provide the same behaviour behind this interface, see [UsbCameraBackend] and
// docs/research/usb-tethering-test-plan.md "iOS later". Host / tests / platforms without it: [NoUsbCamera].
import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';

const MethodChannel _ch = MethodChannel('dapple/usbcam');
const EventChannel _chEvents = EventChannel('dapple/usbcam_events');

/// One file on the card.
class UsbFile {
  UsbFile({required this.handle, required this.name, required this.size, required this.dateMs, required this.key, required this.kind});

  /// Session-local object handle (only valid while this camera session lives); [key] is the stable identity.
  final int handle;
  final String name;
  final int size;
  final int dateMs;

  /// `usb:<lower-case name>|<size>|<capture second>`
  final String key;

  /// raw | jpeg | heif
  final String kind;

  factory UsbFile.fromMap(Map m) => UsbFile(
        handle: (m['handle'] as num).toInt(),
        name: m['name'] as String,
        size: (m['size'] as num).toInt(),
        dateMs: (m['dateMs'] as num).toInt(),
        key: m['key'] as String,
        kind: m['kind'] as String? ?? 'jpeg',
      );
}

/// One exposure: the RAW and the JPEG / HEIF with the same name in the same folder.
class UsbGroup {
  UsbGroup({required this.gid, required this.stem, required this.dateMs, required this.files});
  final String gid;
  final String stem;
  int dateMs;
  final List<UsbFile> files;

  String get label => files.first.name.contains('.') ? files.first.name.substring(0, files.first.name.lastIndexOf('.')) : files.first.name;
  bool get hasRaw => files.any((f) => f.kind == 'raw');
  int get size => files.fold(0, (a, f) => a + f.size);

  /// The file whose embedded thumbnail the camera serves best: the JPEG when there is one.
  UsbFile get thumbSource => files.firstWhere((f) => f.kind != 'raw', orElse: () => files.first);

  factory UsbGroup.fromMap(Map m) => UsbGroup(
        gid: m['gid'] as String,
        stem: m['stem'] as String,
        dateMs: (m['dateMs'] as num).toInt(),
        files: [for (final f in (m['files'] as List)) UsbFile.fromMap(f as Map)],
      );

  /// Files of [more] with the same gid (the other half of a pair that landed on another page).
  void merge(UsbGroup more) {
    for (final f in more.files) {
      if (!files.any((x) => x.handle == f.handle)) files.add(f);
    }
    files.sort((a, b) => (a.kind == 'raw' ? 1 : 0) - (b.kind == 'raw' ? 1 : 0));
    if (more.dateMs > dateMs) dateMs = more.dateMs;
  }
}

class UsbPage {
  UsbPage({required this.groups, required this.next, required this.done, required this.total, required this.skippedVideos});
  final List<UsbGroup> groups;
  final int next;
  final bool done;
  final int total;
  final int skippedVideos;
}

abstract class UsbCameraBackend {
  /// False where the platform cannot talk to a USB camera (nothing is shown then).
  bool get available;

  /// Raw event maps ("type": state | newShots | importQueue | importStart | importProgress | importDone | importFailed | importQueueDone).
  Stream<Map> get events;

  /// Starts watching for a camera; finished files are written into [inbox].
  Future<void> start({required String inbox});
  Future<void> stop();
  Future<void> requestPermission();
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false});
  Future<Uint8List?> thumbnail(int handle);

  /// Copies these object handles to the inbox, in this order.
  Future<void> importHandles(List<int> handles);
  Future<void> cancelImport();

  /// The text to copy and send after trying a camera.
  Future<String> diagnostics();

  static UsbCameraBackend forPlatform() => Platform.isAndroid ? ChannelUsbCamera() : const NoUsbCamera();
}

class ChannelUsbCamera implements UsbCameraBackend {
  Stream<Map>? _events;

  @override
  bool get available => true;

  @override
  Stream<Map> get events => _events ??= _chEvents.receiveBroadcastStream().map((e) => e as Map).asBroadcastStream();

  @override
  Future<void> start({required String inbox}) async => _ch.invokeMethod<bool>('start', {'inbox': inbox});

  @override
  Future<void> stop() async => _ch.invokeMethod<bool>('stop');

  @override
  Future<void> requestPermission() async => _ch.invokeMethod<bool>('requestPermission');

  @override
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false}) async {
    final m = await _ch.invokeMapMethod<String, dynamic>('list', {'cursor': cursor, 'limit': limit, 'refresh': refresh});
    return UsbPage(
      groups: [for (final g in (m?['groups'] as List? ?? const [])) UsbGroup.fromMap(g as Map)],
      next: (m?['next'] as num?)?.toInt() ?? 0,
      done: m?['done'] as bool? ?? true,
      total: (m?['total'] as num?)?.toInt() ?? 0,
      skippedVideos: (m?['skippedVideos'] as num?)?.toInt() ?? 0,
    );
  }

  @override
  Future<Uint8List?> thumbnail(int handle) async {
    try {
      return await _ch.invokeMethod<Uint8List>('thumbnail', {'handle': handle});
    } on PlatformException {
      return null;
    }
  }

  @override
  Future<void> importHandles(List<int> handles) async => _ch.invokeMethod<bool>('import', {'handles': handles});

  @override
  Future<void> cancelImport() async => _ch.invokeMethod<bool>('cancelImport');

  @override
  Future<String> diagnostics() async => await _ch.invokeMethod<String>('diagnostics') ?? '';
}

/// iOS today, desktop, tests that need no camera: no camera, no events, never throws.
class NoUsbCamera implements UsbCameraBackend {
  const NoUsbCamera();
  @override
  bool get available => false;
  @override
  Stream<Map> get events => const Stream.empty();
  @override
  Future<void> start({required String inbox}) async {}
  @override
  Future<void> stop() async {}
  @override
  Future<void> requestPermission() async {}
  @override
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false}) async =>
      UsbPage(groups: [], next: 0, done: true, total: 0, skippedVideos: 0);
  @override
  Future<Uint8List?> thumbnail(int handle) async => null;
  @override
  Future<void> importHandles(List<int> handles) async {}
  @override
  Future<void> cancelImport() async {}
  @override
  Future<String> diagnostics() async => 'USB camera import is not available on this platform';
}
