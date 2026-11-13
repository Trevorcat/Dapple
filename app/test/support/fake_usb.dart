import 'dart:async';
import 'dart:typed_data';

import 'package:dapple/platform/usb_camera.dart';

/// A scripted camera for the Dart tests: the test pushes native events into [ctl] and reads what the app asked for.
class FakeUsbBackend implements UsbCameraBackend {
  final StreamController<Map> ctl = StreamController<Map>.broadcast(sync: true);
  final List<List<int>> imports = [];
  final List<int> thumbRequests = [];
  List<UsbGroup> card = []; // newest first
  bool started = false;
  int permissionAsks = 0;
  int cancels = 0;

  @override
  bool get available => true;
  @override
  Stream<Map> get events => ctl.stream;
  @override
  Future<void> start({required String inbox}) async => started = true;
  @override
  Future<void> stop() async => started = false;
  @override
  Future<void> requestPermission() async => permissionAsks++;
  @override
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false}) async {
    final end = (cursor + limit).clamp(0, card.length);
    return UsbPage(groups: card.sublist(cursor, end), next: end, done: end >= card.length, total: card.length, skippedVideos: 2);
  }

  @override
  Future<Uint8List?> thumbnail(int handle) async {
    thumbRequests.add(handle);
    return null;
  }

  @override
  Future<void> importHandles(List<int> handles) async => imports.add(handles);
  @override
  Future<void> cancelImport() async => cancels++;
  @override
  Future<String> diagnostics() async => 'DIAG TEXT';

  void emit(Map m) => ctl.add(m);

  void ready({String model = 'ILCE-7RM5', String maker = 'Sony', int total = 1234}) => emit({
        'type': 'state',
        'state': 'ready',
        'device': {'manufacturer': maker, 'model': model, 'serial': 'S1'},
        'card': {'total': total},
      });
}

Map camFile(int h, String name, {int size = 100, int sec = 1700000000, String? kind}) => {
      'handle': h,
      'name': name,
      'size': size,
      'dateMs': sec * 1000,
      'key': 'usb:${name.toLowerCase()}|$size|$sec',
      'kind': kind ?? (name.toLowerCase().endsWith('.jpg') ? 'jpeg' : 'raw'),
    };

Map camGroup(String stem, List<Map> files) =>
    {'gid': '100msdcf/${stem.toLowerCase()}', 'stem': stem.toLowerCase(), 'dir': '100MSDCF', 'dateMs': 1700000000000, 'files': files};
