import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';

import '../platform/usb_camera.dart';
import 'ingest.dart';

/// Where the wired import stands (what the home card shows).
enum UsbPhase { none, permission, opening, ready, unplugged, error }

/// A camera on the USB cable, mirroring [FtpIngestSource]'s role: the native side lists and copies, this class decides what to
/// import (only shots taken after connecting, plus what the user picks) and turns finished copies into [IncomingFile]s.
///
/// Identity: a card file is `usb:<lower-case name>|<size>|<capture second>` (see CamIdentity in UsbCamModel.kt). A file counts as
/// already known when the library holds that id, or any photo with the same name and size (the same shot received over FTP).
class UsbIngest extends ChangeNotifier {
  UsbIngest(this.backend, {DateTime Function()? clock, this.unpluggedFor = const Duration(seconds: 45)}) : _clock = clock ?? DateTime.now;
  final UsbCameraBackend backend;
  final DateTime Function() _clock;

  /// How long the "unplugged" card stays before the card goes back to normal.
  final Duration unpluggedFor;

  // wired by AppState
  bool Function(UsbFile f) isKnown = (_) => false;
  void Function(String name)? onArrivalStart;
  void Function(String name, double fraction)? onArrivalProgress;
  void Function(String name, bool ok)? onArrivalEnd;
  void Function(List<IncomingFile> files)? onFiles;

  UsbPhase phase = UsbPhase.none;
  String? reason;
  String model = '';
  String serial = '';
  int cardTotal = 0;
  int received = 0; // photos received since this camera connected
  int failed = 0;
  String? lastName;
  String? notice; // a one-off line such as "空间不足"
  double? progress; // current file, 0..1
  bool get importing => _queuedPhotos > _donePhotos;
  int get queuedPhotos => _queuedPhotos;
  int get donePhotos => _donePhotos;
  int _queuedPhotos = 0;
  int _donePhotos = 0;
  final Map<String, String> _groupOfKey = {}; // file key -> gid, for the queue
  final Map<String, int> _remaining = {}; // gid -> files not finished
  final Set<String> _requested = {}; // keys queued or imported in this session
  final Map<String, UsbFile> _fileOfKey = {};
  StreamSubscription<Map>? _sub;
  Timer? _clearTimer;
  bool _started = false;

  bool get available => backend.available;

  /// True while the home card should show the wired states instead of the usual ones.
  bool get cardActive => backend.available && phase != UsbPhase.none;

  Future<void> start(String inboxPath) async {
    if (_started || !backend.available) return;
    _started = true;
    _sub = backend.events.listen(_onEvent, onError: (_) {});
    await backend.start(inbox: inboxPath);
  }

  Future<void> stop() async {
    _started = false;
    await _sub?.cancel();
    _sub = null;
    _clearTimer?.cancel();
    await backend.stop();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _clearTimer?.cancel();
    super.dispose();
  }

  // ------------------------------------------------------------------ events

  void _onEvent(Map e) {
    switch (e['type']) {
      case 'state':
        _onState(e);
        break;
      case 'newShots':
        _onNewShots(e);
        break;
      case 'importQueue':
        break;
      case 'importStart':
        final name = e['name'] as String;
        progress = 0;
        onArrivalStart?.call(name);
        notifyListeners();
        break;
      case 'importProgress':
        final size = (e['size'] as num?)?.toInt() ?? 0;
        final f = size > 0 ? ((e['bytes'] as num?)?.toInt() ?? 0) / size : 0.0;
        progress = f.clamp(0.0, 1.0).toDouble();
        onArrivalProgress?.call(e['name'] as String, progress!);
        notifyListeners();
        break;
      case 'importDone':
        _onImportDone(e);
        break;
      case 'importFailed':
        final name = e['name'] as String;
        final key = e['key'] as String?;
        onArrivalEnd?.call(name, false);
        progress = null;
        final why = e['reason'];
        if (why == 'space') notice = 'space';
        if (why != 'gone') failed++;
        if (key != null) _requested.remove(key); // a later pick may try it again
        _fileFinished(key);
        notifyListeners();
        break;
      case 'importQueueDone':
        progress = null;
        if (_queuedPhotos == _donePhotos || e['aborted'] != null || e['cancelled'] == true) {
          _queuedPhotos = 0;
          _donePhotos = 0;
          _groupOfKey.clear();
          _remaining.clear();
        }
        notifyListeners();
        break;
    }
  }

  void _onState(Map e) {
    final st = e['state'] as String? ?? 'none';
    reason = e['reason'] as String?;
    _clearTimer?.cancel();
    final dev = e['device'] as Map?;
    if (dev != null) {
      model = _clean(dev['manufacturer'] as String?, dev['model'] as String?);
      serial = dev['serial'] as String? ?? '';
    }
    final card = e['card'] as Map?;
    if (card != null) cardTotal = (card['total'] as num?)?.toInt() ?? 0;
    switch (st) {
      case 'permission':
        phase = UsbPhase.permission;
        break;
      case 'opening':
        phase = UsbPhase.opening;
        break;
      case 'ready':
        phase = UsbPhase.ready;
        received = 0;
        failed = 0;
        notice = null;
        lastName = null;
        _requested.clear();
        _fileOfKey.clear();
        _queuedPhotos = 0;
        _donePhotos = 0;
        break;
      case 'unplugged':
        phase = UsbPhase.unplugged;
        progress = null;
        _queuedPhotos = 0;
        _donePhotos = 0;
        _clearTimer = Timer(unpluggedFor, () {
          if (phase == UsbPhase.unplugged) {
            phase = UsbPhase.none;
            notifyListeners();
          }
        });
        break;
      case 'error':
        phase = UsbPhase.error;
        break;
      default:
        phase = UsbPhase.none;
    }
    notifyListeners();
  }

  /// "Sony" + "ILCE-7RM5" -> "Sony ILCE-7RM5"; the model alone when it already starts with the maker.
  static String _clean(String? maker, String? model) {
    final m = (model ?? '').trim();
    final mk = (maker ?? '').trim();
    if (m.isEmpty) return mk;
    if (mk.isEmpty || m.toLowerCase().startsWith(mk.toLowerCase())) return m;
    return '$mk $m';
  }

  void _onNewShots(Map e) {
    for (final g in (e['groups'] as List? ?? const [])) {
      final group = UsbGroup.fromMap(g as Map);
      importGroups([group]);
    }
    final total = (e['total'] as num?)?.toInt();
    if (total != null) {
      cardTotal = total;
      notifyListeners();
    }
  }

  void _onImportDone(Map e) {
    final name = e['name'] as String;
    final key = e['key'] as String;
    final out = (e['outName'] as String?) ?? name;
    final path = e['path'] as String;
    onArrivalEnd?.call(name, true);
    progress = null;
    final dateMs = (e['dateMs'] as num?)?.toInt() ?? 0;
    final now = _clock().millisecondsSinceEpoch;
    final added = dateMs > 0 && dateMs <= now + 24 * 3600 * 1000 ? dateMs : now;
    int size = (e['size'] as num?)?.toInt() ?? 0;
    try {
      size = File(path).lengthSync();
    } catch (_) {}
    onFiles?.call([IncomingFile(id: key, name: out, addedMs: added, path: path, size: size)]);
    lastName = out;
    _fileFinished(key, ok: true);
    notifyListeners();
  }

  void _fileFinished(String? key, {bool ok = false}) {
    final gid = key == null ? null : _groupOfKey[key];
    if (gid == null) return;
    final left = (_remaining[gid] ?? 1) - 1;
    _remaining[gid] = left;
    if (left <= 0) {
      _donePhotos++;
      if (ok) received++;
    }
  }

  // ------------------------------------------------------------------ importing

  /// Queues the files of [groups] that the library does not know. Returns how many photos were queued.
  int importGroups(List<UsbGroup> groups) {
    final handles = <int>[];
    var photos = 0;
    for (final g in groups) {
      final fresh = g.files.where((f) => !_requested.contains(f.key) && !isKnown(f)).toList();
      if (fresh.isEmpty) continue;
      photos++;
      for (final f in fresh) {
        _requested.add(f.key);
        _fileOfKey[f.key] = f;
        _groupOfKey[f.key] = g.gid;
        handles.add(f.handle);
      }
      _remaining[g.gid] = (_remaining[g.gid] ?? 0) + fresh.length;
    }
    if (handles.isEmpty) return 0;
    _queuedPhotos += photos;
    notifyListeners();
    unawaited(backend.importHandles(handles).catchError((Object _) {}));
    return photos;
  }

  Future<void> cancelImport() => backend.cancelImport();

  Future<void> retry() async {
    notice = null;
    await backend.requestPermission();
  }

  Future<String> diagnostics() => backend.diagnostics();

  // ------------------------------------------------------------------ the card (wording: docs/design/guidelines.md 13)

  String get _name => model.isEmpty ? '相机' : model;

  String get cardTitle {
    switch (phase) {
      case UsbPhase.permission:
        return '相机已插入';
      case UsbPhase.opening:
        return '$_name · 正在连接';
      case UsbPhase.ready:
        return '$_name · 已连接';
      case UsbPhase.unplugged:
        return '$_name · 已拔出';
      case UsbPhase.error:
        return '$_name · 连接失败';
      case UsbPhase.none:
        return '';
    }
  }

  String get cardLine {
    switch (phase) {
      case UsbPhase.permission:
        return '等待允许访问';
      case UsbPhase.opening:
        return '读取相机';
      case UsbPhase.ready:
        if (notice == 'space') return '手机空间不足 · 已停止';
        if (importing) return '正在接收第 ${(_donePhotos + 1).clamp(1, _queuedPhotos)} 张 · 共 $_queuedPhotos 张';
        if (failed > 0) return '$failed 个文件没收到 · 共收到 $received 张';
        if (lastName != null) return '收到 ${_stem(lastName!)} · 共 $received 张';
        return cardTotal > 0 ? '约 $cardTotal 个文件 · 自动接收' : '自动接收新照片';
      case UsbPhase.unplugged:
        return received > 0 ? '共收到 $received 张' : '没有新照片';
      case UsbPhase.error:
        return switch (reason) {
          'permission_denied' => '未允许访问相机',
          'busy' => '相机被占用',
          'no_storage' => '读不到存储卡',
          'card_unreadable' => '读不到卡里的照片',
          _ => '相机没有响应',
        };
      case UsbPhase.none:
        return '';
    }
  }

  String? get cardAction {
    switch (phase) {
      case UsbPhase.permission:
        return '允许';
      case UsbPhase.ready:
        return '导入照片…';
      case UsbPhase.error:
        return reason == 'permission_denied' ? '允许' : '重试';
      default:
        return null;
    }
  }

  /// 0..1 of the whole queue (finished photos plus the file in flight), null when nothing is arriving.
  double? get cardProgress {
    if (phase != UsbPhase.ready || !importing) return null;
    final cur = progress ?? 0;
    return ((_donePhotos + cur) / _queuedPhotos).clamp(0.0, 1.0).toDouble();
  }

  static String _stem(String n) => n.contains('.') ? n.substring(0, n.lastIndexOf('.')) : n;
}
