import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter/widgets.dart' show GlobalKey, NavigatorState;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../model/catalog.dart';
import '../model/film_catalog.dart';
import '../model/film_names.dart' show parseFilmSwatches;
import '../model/params.dart';
import '../models.dart';
import '../native/dapple_session.dart';
import '../services/blank_check.dart';
import '../services/detection.dart';
import '../services/ftp_ingest.dart';
import '../services/ingest.dart';
import '../services/live_bridge.dart';
import '../services/merge_engine.dart';
import '../services/session_cache.dart';
import '../services/tool_models.dart';

/// A transient message (Snackbar on Android). Single instance, newest replaces older
/// (requirements 3.3). [actionLabel] toasts stay 8 s, plain ones 2.4 s.
class Toast {
  Toast(this.message, {this.actionLabel, this.onAction}) : id = _n++;
  static int _n = 0;
  final int id;
  final String message;
  final String? actionLabel;
  final VoidCallback? onAction;
  Duration get duration => Duration(milliseconds: actionLabel != null ? 8000 : 2400);
}

/// A day section of the home grid.
class PhotoGroup {
  PhotoGroup(this.title, this.countText, this.photos, {this.isEarlier = false});
  final String title;
  final String countText;
  final List<Photo> photos;
  final bool isEarlier;
}

enum DemoScenario { normal, disconnected, transferFailed, empty }

/// Central app state: photo library, per-photo edit state with undo, selection,
/// settings, develop queue, toasts. `AppState.demo()` runs entirely in memory with the
/// prototype's sample photos (widget tests / screenshots; no platform channels).
class AppState extends ChangeNotifier {
  AppState() : isDemo = false;

  /// In-memory demo data. [now] pins "today" so tests are deterministic.
  AppState.demo({DemoScenario scenario = DemoScenario.normal, DateTime? now})
      : isDemo = true,
        _demoNow = now ?? DateTime(2026, 11, 11, 15, 30) {
    ready = true;
    mediaPermitted = true;
    settings.onboarded = true;
    _loadDemo(scenario);
  }

  final bool isDemo;
  DateTime? _demoNow;
  DateTime get now => _demoNow ?? DateTime.now();

  late final Directory _docs;
  late final Directory _cache;
  late final Directory inbox;
  late final Directory previews;
  late final Directory exports;
  late final Directory merged;
  late final Directory liveDir;
  late final SharedPreferences _prefs;

  final Map<String, Photo> _photos = {};
  AppSettings settings = AppSettings();

  final MediaStoreIngestSource mediaSource = MediaStoreIngestSource();
  late final PickerIngestSource pickerSource;

  /// Engine entry points of the HDR / panorama / Motion Photo tools (tests substitute a fake).
  MergeEngine engine = const NativeMergeEngine();
  FtpBackend ftpBackend = ChannelFtpBackend();
  late final FtpIngestSource ftpSource = FtpIngestSource(ftpBackend);
  StreamSubscription<FtpEvent>? _ftpSub;
  final GlobalKey<NavigatorState> navigatorKey = GlobalKey<NavigatorState>();

  /// Set by the app shell: reopens the tool page of a (failed) background job.
  void Function(ToolJob job)? reopenTool;
  StreamSubscription<void>? _watchSub;
  Timer? _scanDebounce;

  late final SessionCache sessions = SessionCache(this);

  /// Film stocks of the engine's pack (empty when the pack is missing or the library has no film support).
  FilmCatalog film = FilmCatalog.empty;

  /// Asset path of the film pack (a copy of core/data/film/dapple.dfp, see docs/film/README.md).
  static const filmPackAsset = 'assets/film/dapple.dfp';

  /// Reads the film pack asset into the engine (2 ms for the 1.2 MB pack) and lists its stocks.
  Future<void> _loadFilm() async {
    try {
      final data = await rootBundle.load(filmPackAsset);
      final bytes = data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
      if (!DappleSession.loadFilmPack(bytes)) {
        debugPrint('film pack not loaded');
        return;
      }
      film = FilmCatalog.fromJson(DappleSession.filmListJson(), attribution: DappleSession.filmAttribution());
      if (!film.isEmpty) unawaited(_loadFilmSwatches(bytes.length));
    } catch (e) {
      debugPrint('film pack failed: $e');
    }
  }

  /// Bump when the engine's swatch patches change (invalidates the cached swatches).
  static const _swatchVersion = 1;

  /// Swatches of the stocks, rendered by the engine from fixed test patches (a few seconds, once per pack): read from
  /// the cache file when its key (version, pack size, stock ids) matches, else computed in a background isolate and cached.
  Future<void> _loadFilmSwatches(int packBytes) async {
    try {
      final key = '$_swatchVersion|$packBytes|${film.stocks.map((s) => s.id).join(',')}';
      final file = File(p.join(_docs.path, 'film_swatches.json'));
      String? json;
      try {
        if (file.existsSync()) {
          final j = jsonDecode(file.readAsStringSync());
          if (j is Map && j['key'] == key) json = jsonEncode(j['swatches']);
        }
      } catch (_) {}
      var fromCache = json != null;
      json ??= await DappleSession.filmSwatchesJsonIsolate();
      final sw = parseFilmSwatches(json);
      if (sw.isEmpty) return;
      if (!fromCache) {
        try {
          file.writeAsStringSync(jsonEncode({'key': key, 'swatches': jsonDecode(json!)}));
        } catch (_) {}
      }
      film = film.withSwatches(sw);
      notifyListeners();
    } catch (e) {
      debugPrint('film swatches failed: $e');
    }
  }

  bool ready = false;
  bool mediaPermitted = true;
  bool nativeOk = true;
  String? nativeError;

  // ---------------------------------------------------------------- navigation
  /// 0 照片, 1 风格, 2 设置
  int tab = 0;
  void setTab(int i) {
    if (i == tab) return;
    tab = i;
    // switching tab clears the selection and unfreezes the grid (requirements 2.4)
    selectMode = false;
    _selected.clear();
    setFrozen(false);
    notifyListeners();
  }

  // ---------------------------------------------------------------- toasts
  Toast? toast;
  void showToast(String message, {String? actionLabel, VoidCallback? onAction}) {
    toast = Toast(message, actionLabel: actionLabel, onAction: onAction);
    notifyListeners();
  }

  void clearToast(int id) {
    if (toast?.id == id) {
      toast = null;
      notifyListeners();
    }
  }

  // ---------------------------------------------------------------- photos
  final List<String> _held = []; // arrived while the grid is frozen
  List<Photo> get photos {
    final l = _photos.values.where((e) => !_held.contains(e.id)).toList()..sort((a, b) => b.addedMs.compareTo(a.addedMs));
    return l;
  }

  Photo? photoById(String id) => _photos[id];
  List<Photo> get allPhotos => _photos.values.toList();
  bool get isEmpty => _photos.isEmpty;

  /// Photos reachable by prev / next in the editor: processed + processing (not arriving / interrupted).
  List<Photo> get editable => photos.where((e) => e.openable).toList();

  bool _sameDay(DateTime a, DateTime b) => a.year == b.year && a.month == b.month && a.day == b.day;
  String _hhmm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  List<PhotoGroup> get groups {
    final n = now;
    final yest = n.subtract(const Duration(days: 1));
    final by = <String, List<Photo>>{};
    final order = <String>[];
    for (final ph in photos) {
      final t = DateTime.fromMillisecondsSinceEpoch(ph.addedMs);
      final k = _sameDay(t, n) ? 't' : _sameDay(t, yest) ? 'y' : '${t.year}-${t.month}-${t.day}';
      (by[k] ??= []).add(ph);
      if (!order.contains(k)) order.add(k);
    }
    final out = <PhotoGroup>[];
    for (final k in order) {
      final l = by[k]!;
      final first = DateTime.fromMillisecondsSinceEpoch(l.map((e) => e.addedMs).reduce((a, b) => a < b ? a : b));
      final place = l.map((e) => e.place).whereType<String>().cast<String?>().firstWhere((_) => true, orElse: () => null);
      final day = k == 't' ? '今天' : k == 'y' ? '昨天' : '${first.month} 月 ${first.day} 日';
      final where = place ?? (!isDemo && !cameraSource ? '手机相册' : null);
      final title = where == null ? day : '$day · $where';
      final unit = k == 'y' ? '项' : '张';
      out.add(PhotoGroup(title, '${l.length} $unit · ${_hhmm(first)}${k == 'y' ? '' : ' 起'}', l,
          isEarlier: k == 'y'));
    }
    return out;
  }

  // ---------------------------------------------------------------- connection card
  /// True when the transport is gone (demo scenario / missing permission).
  bool linkDown = false;

  /// True only while a real camera ingest source (FTP / Creators' App link) is connected. The
  /// production app has none yet (photos come from the phone gallery), so this stays false there;
  /// the demo data set pretends a camera is paired. Never show a camera name without it.
  late bool cameraSource = isDemo;
  late String cameraName = isDemo ? 'Sony α7R V' : '';

  /// What the status UI calls the origin of the photos.
  String get sourceLabel => cameraSource && cameraName.isNotEmpty ? cameraName : '手机相册';

  ConnState get connState {
    if (linkDown) return ConnState.disconnected;
    if (_photos.isEmpty) return ConnState.idle;
    if (_photos.values.any((e) => e.status == PhotoStatus.interrupted || e.status == PhotoStatus.failed)) {
      return ConnState.interrupted;
    }
    if (!cameraSource) return ConnState.idle;
    // FTP receiver on but no camera talking to it yet: waiting, not connected.
    if (!isDemo && ftpRunning && ftpClients == 0 && arrivingCount == 0) return ConnState.idle;
    return ConnState.connected;
  }

  int get interruptedCount =>
      _photos.values.where((e) => e.status == PhotoStatus.interrupted || e.status == PhotoStatus.failed).length;
  int get arrivingCount => _photos.values.where((e) => e.status == PhotoStatus.arriving).length;
  int get processedCount => _photos.values.where((e) => e.status == PhotoStatus.done).length;
  int get processingCount => _photos.values.where((e) => e.processing).length;

  /// Progress of the arriving photo (0..1) or null.
  double? get arrivingProgress {
    final a = _photos.values.where((e) => e.status == PhotoStatus.arriving);
    if (a.isEmpty) return null;
    return a.first.progress;
  }

  String get connTitle {
    switch (connState) {
      case ConnState.disconnected:
        return '${cameraName.isEmpty ? '手机相册' : cameraName} · 已断开';
      case ConnState.idle:
        if (!isDemo && ftpRunning) return '相机无线接收 · 等待相机';
        return cameraSource ? '$cameraName · 未连接' : '未连接相机';
      default:
        return '$cameraName · 已连接';
    }
  }

  String get connLine {
    switch (connState) {
      case ConnState.disconnected:
        return '最后收到第 ${_photos.length} 张';
      case ConnState.idle:
        if (!isDemo && ftpRunning) return ftpAddressText ?? '未连接 Wi-Fi 或热点';
        return cameraSource || _photos.isEmpty ? '没有新照片' : '照片来自手机相册 · ${_photos.length} 张';
      case ConnState.interrupted:
        return '$interruptedCount 张传输中断';
      case ConnState.connected:
        if (arrivingCount > 0) {
          final today = _photos.values.where((e) => _sameDay(DateTime.fromMillisecondsSinceEpoch(e.addedMs), now));
          final n = today.where((e) => e.status != PhotoStatus.arriving).length + 1;
          final done = today.where((e) => e.status == PhotoStatus.done).length;
          return '正在接收第 $n 张 · 已处理 $done 张';
        }
        if (processingCount == 1) return '正在处理最后 1 张';
        if (processingCount > 1) return '正在处理 $processingCount 张 · 已处理 $processedCount 张';
        return '本组 $processedCount 张已处理';
    }
  }

  /// Label of the card's action button or null.
  String? get connAction {
    switch (connState) {
      case ConnState.interrupted:
        return '重试';
      case ConnState.disconnected:
        return '重新连接';
      case ConnState.idle:
        if (!isDemo && ftpRunning) return null;
        return cameraSource ? '连接' : '连接相机';
      case ConnState.connected:
        return null;
    }
  }

  Future<void> connectAction() async {
    switch (connState) {
      case ConnState.interrupted:
        retryAll();
        showToast('已重新传输');
        break;
      case ConnState.disconnected:
      case ConnState.idle:
        if (isDemo) {
          linkDown = false;
          showToast('已连接');
          notifyListeners();
        } else {
          // No camera transport yet: only the gallery permission can be (re)granted here.
          if (!mediaPermitted) await requestPermission();
          showToast(mediaPermitted ? '相机无线接收暂未开放，目前从手机相册导入' : '需要允许访问照片');
        }
        break;
      case ConnState.connected:
        break;
    }
  }

  void retryAll() {
    for (final ph in _photos.values.where((e) => e.status == PhotoStatus.interrupted || e.status == PhotoStatus.failed)) {
      retry(ph, quiet: true);
    }
    notifyListeners();
  }

  // ---------------------------------------------------------------- freeze / "新到 N 张"
  bool frozen = false;
  int get newCount => _held.length;

  void setFrozen(bool v) {
    if (frozen == v) return;
    frozen = v;
    if (!v) _held.clear();
    notifyListeners();
  }

  /// Releases held photos ("新到 N 张" pill tapped).
  void releaseNew() {
    _held.clear();
    frozen = false;
    notifyListeners();
  }

  // ---------------------------------------------------------------- selection
  bool selectMode = false;
  final List<String> _selected = [];
  List<String> get selectedIds => List.unmodifiable(_selected);
  bool isSelected(String id) => _selected.contains(id);
  int selectionNumber(String id) => _selected.indexOf(id) + 1;

  void enterSelect([String? id]) {
    selectMode = true;
    frozen = true;
    final ph = id == null ? null : _photos[id];
    if (ph != null && selectable(ph) && !_selected.contains(id)) _selected.add(id!);
    notifyListeners();
  }

  void exitSelect() {
    selectMode = false;
    _selected.clear();
    setFrozen(false);
  }

  bool selectable(Photo ph) => ph.openable;

  void toggleSelect(String id) {
    final ph = _photos[id];
    if (ph == null || !selectable(ph)) return;
    if (!_selected.remove(id)) _selected.add(id);
    notifyListeners();
  }

  void selectAll(Iterable<Photo> list) {
    for (final ph in list) {
      if (selectable(ph) && !_selected.contains(ph.id)) _selected.add(ph.id);
    }
    notifyListeners();
  }

  void deselectAll(Iterable<Photo> list) {
    final ids = list.map((e) => e.id).toSet();
    _selected.removeWhere(ids.contains);
    notifyListeners();
  }

  List<Photo> get selectedPhotos => [for (final id in _selected) if (_photos[id] != null) _photos[id]!];

  // ---------------------------------------------------------------- edit state, undo, clipboard
  final Map<String, UndoStack> _undo = {};
  UndoStack _stack(String id) => _undo.putIfAbsent(id, UndoStack.new);
  bool canUndo(String id) => _undo[id]?.canUndo ?? false;
  bool canRedo(String id) => _undo[id]?.canRedo ?? false;

  EditState editOf(String id) => _photos[id]?.edit ?? EditState.empty;

  /// Applies [f] to the photo's edit state and records an undo point. Repeated calls with the same
  /// [coalesce] key within 800 ms (a slider drag) form one step.
  ///
  /// [gesture]: [coalesce] identifies one continuous finger gesture, which is a single undo step
  /// however long the finger pauses (requirements 4.15.10).
  void applyEdit(String id, EditState Function(EditState e) f, {String? coalesce, bool gesture = false}) {
    final ph = _photos[id];
    if (ph == null) return;
    final next = f(ph.edit);
    if (next.sameAs(ph.edit)) return;
    _stack(id).record(ph.edit, coalesceKey: coalesce, forceMerge: gesture);
    ph.edit = next;
    final flipped = ph.exported;
    ph.exported = false;
    editRevision++;
    if (gesture && !flipped) {
      // A drag step changes nothing the library, the shell or the top bar show: rebuilding all of them
      // (the whole library grid sits under the editor route) on every pointer move cost 10-25 ms per frame
      // on a low-end phone. The editor listens to [editTick] instead.
      _saveSoon();
      editTick.value++;
      return;
    }
    changed();
    editTick.value++;
  }

  /// Bumped on every edit, including drag steps that do not notify [AppState] listeners.
  final ValueNotifier<int> editTick = ValueNotifier(0);

  /// Bumped on every edit; the editor uses it to re-render.
  int editRevision = 0;

  void undo(String id) {
    final ph = _photos[id];
    final prev = ph == null ? null : _stack(id).undo(ph.edit);
    if (ph == null || prev == null) return;
    ph.edit = prev;
    editRevision++;
    changed();
    editTick.value++;
  }

  void redo(String id) {
    final ph = _photos[id];
    final nxt = ph == null ? null : _stack(id).redo(ph.edit);
    if (ph == null || nxt == null) return;
    ph.edit = nxt;
    editRevision++;
    changed();
    editTick.value++;
  }

  EditState? clipboard;
  bool get hasClipboard => clipboard != null;

  void copySettings(String id) {
    clipboard = editOf(id).withoutCrop();
    showToast('已复制设置');
  }

  void pasteSettings(String id) {
    final clip = clipboard;
    if (clip == null) return;
    final before = editOf(id);
    applyEdit(id, (e) => e.pasted(clip));
    showToast('已粘贴设置', actionLabel: '撤销', onAction: () {
      if (editOf(id).sameAs(before)) return;
      undo(id);
    });
  }

  /// Merge (not overwrite) the source's overrides into [targets] for the chosen categories.
  void syncSettings(String srcId, List<String> targets, Set<SyncCat> cats) {
    final src = editOf(srcId);
    final before = <String, EditState>{};
    for (final t in targets) {
      before[t] = editOf(t);
      applyEdit(t, (e) => e.merged(src, cats));
    }
    showToast('已同步 ${targets.length} 张', actionLabel: '撤销', onAction: () {
      for (final t in targets) {
        undo(t);
      }
    });
  }

  /// Clears every override of the photo (requirements 4.8 "还原为自动结果").
  void restoreAuto(String id) {
    applyEdit(id, (_) => EditState.empty);
    showToast('已还原为自动结果', actionLabel: '撤销', onAction: () => undo(id));
  }

  void setDefaultLook(String lookId) {
    final old = settings.defaultLookId;
    if (old == lookId) return;
    updateSettings((s) => s.defaultLookId = lookId);
    showToast('新照片默认使用「${lookById(lookId).name}」', actionLabel: '撤销', onAction: () {
      updateSettings((s) => s.defaultLookId = old);
    });
  }

  /// Wire JSON for the engine (requirements chapter 6 keys). The engine's own automatic correction
  /// (legacy EditParams keys, per-photo) is the base; user deltas add to shared numeric keys.
  Map<String, dynamic> wireParams(Photo ph, {Map<String, dynamic> override = const {}}) {
    final user = ph.edit.toWire(defaultLook: settings.defaultLookId, defaultToneStrength: settings.toneStrength);
    final out = <String, dynamic>{};
    final corr = ph.correction;
    if (settings.autoProcess && corr != null) {
      corr.forEach((k, v) {
        // `exposure` means EV in the legacy schema but a -100..100 slider in chapter 6: do not mix.
        if (k != 'exposure') out[k] = v;
      });
    }
    user.forEach((k, v) {
      final base = out[k];
      if (v is num && base is num && specByKey.containsKey(k)) {
        out[k] = base + v;
      } else {
        out[k] = v;
      }
    });
    out.addAll(override);
    return out;
  }

  // ---------------------------------------------------------------- init / persistence (real mode)
  Future<void> init() async {
    _docs = await getApplicationDocumentsDirectory();
    _cache = await getTemporaryDirectory();
    inbox = Directory(p.join(_docs.path, 'inbox'));
    previews = Directory(p.join(_docs.path, 'previews'));
    exports = Directory(p.join(_cache.path, 'exports'));
    merged = Directory(p.join(_docs.path, 'merged'));
    liveDir = Directory(p.join(_docs.path, 'live'));
    for (final d in [inbox, previews, exports, merged, liveDir]) {
      await d.create(recursive: true);
    }
    pickerSource = PickerIngestSource(inbox);
    _prefs = await SharedPreferences.getInstance();
    settings = AppSettings(
      defaultLookId: _prefs.getString('defaultLook2') ?? 'std',
      toneStrength: _prefs.getDouble('toneStrength') ?? 60,
      autoProcess: _prefs.getBool('autoProcess') ?? true,
      exportLongEdge: _prefs.getInt('exportLongEdge3') ?? 0,
      stripLocation: _prefs.getBool('stripLocation') ?? true,
      jpegQuality: _prefs.getInt('jpegQuality2') ?? 95,
      watchGallery: _prefs.getBool('watchGallery') ?? true,
      onboarded: _prefs.getBool('onboarded') ?? true,
      ftpEnabled: _prefs.getBool('ftpEnabled') ?? false,
      ftpUser: _prefs.getString('ftpUser') ?? 'dapple',
      ftpPass: _prefs.getString('ftpPass') ?? generateFtpPassword(),
      ftpPort: _prefs.getInt('ftpPort') ?? 2121,
      handedness: _prefs.getInt('handedness') ?? 0,
      haptics: _prefs.getBool('haptics') ?? true,
      handSide: _prefs.getInt('handSide') ?? 1,
      editorLayout: _prefs.getString('editorLayout') ?? '',
    );
    await _prefs.setString('ftpPass', settings.ftpPass);
    nativeOk = DappleSession.isAvailable;
    if (!nativeOk) nativeError = '原生库未加载';
    await _loadFilm();
    await _loadLibrary();
    await _loadDetection();
    ready = true;
    notifyListeners();
    await refreshPermission();
    _applyWatch();
    unawaited(scan());
    for (final ph in _photos.values) {
      if (ph.status == PhotoStatus.developing) ph.status = PhotoStatus.pending;
      if (ph.status == PhotoStatus.pending) _enqueue(ph.id);
    }
    unawaited(_auditMerged());
    unawaited(auditLive());
    unawaited(refreshVideos());
    unawaited(initFtp());
    _scheduleDetect();
  }

  File get _libFile => File(p.join(_docs.path, 'library2.json'));

  Future<void> _loadLibrary() async {
    try {
      if (await _libFile.exists()) {
        for (final j in (jsonDecode(await _libFile.readAsString()) as List)) {
          final ph = Photo.fromJson((j as Map).cast<String, dynamic>());
          _photos[ph.id] = ph;
        }
      }
    } catch (e) {
      debugPrint('load library failed: $e');
    }
  }

  Timer? _saveTimer;
  void _saveSoon() {
    if (isDemo) return;
    _saveTimer?.cancel();
    _saveTimer = Timer(const Duration(milliseconds: 500), () async {
      await _libFile.writeAsString(jsonEncode(_photos.values.where((e) => !e.id.startsWith('ftp:arr:')).map((e) => e.toJson()).toList()));
    });
  }

  void changed({bool save = true}) {
    if (save) _saveSoon();
    notifyListeners();
  }

  // ---------------------------------------------------------------- settings
  Future<void> updateSettings(void Function(AppSettings s) f) async {
    f(settings);
    notifyListeners();
    if (isDemo) return;
    await _prefs.setString('defaultLook2', settings.defaultLookId);
    await _prefs.setDouble('toneStrength', settings.toneStrength);
    await _prefs.setBool('autoProcess', settings.autoProcess);
    await _prefs.setInt('exportLongEdge3', settings.exportLongEdge);
    await _prefs.setBool('stripLocation', settings.stripLocation);
    await _prefs.setInt('jpegQuality2', settings.jpegQuality);
    await _prefs.setBool('watchGallery', settings.watchGallery);
    await _prefs.setBool('onboarded', settings.onboarded);
    await _prefs.setBool('ftpEnabled', settings.ftpEnabled);
    await _prefs.setString('ftpUser', settings.ftpUser);
    await _prefs.setString('ftpPass', settings.ftpPass);
    await _prefs.setInt('ftpPort', settings.ftpPort);
    await _prefs.setInt('handedness', settings.handedness);
    await _prefs.setBool('haptics', settings.haptics);
    await _prefs.setInt('handSide', settings.handSide);
    await _prefs.setString('editorLayout', settings.editorLayout);
    _applyWatch();
  }

  // ---------------------------------------------------------------- permissions
  Future<void> refreshPermission() async {
    if (!Platform.isAndroid || isDemo) return;
    final st = await Permission.photos.status; // READ_MEDIA_IMAGES on 13+, READ_EXTERNAL_STORAGE before
    mediaPermitted = st.isGranted || st.isLimited;
    linkDown = !mediaPermitted;
    notifyListeners();
  }

  Future<void> requestPermission() async {
    if (!Platform.isAndroid || isDemo) return;
    final st = await Permission.photos.request();
    mediaPermitted = st.isGranted || st.isLimited;
    linkDown = !mediaPermitted;
    notifyListeners();
    if (mediaPermitted) unawaited(scan());
  }

  /// Android 11+: needed to see RAW files MediaStore does not classify as images and to read them by path.
  Future<void> requestAllFilesAccess() async {
    if (!Platform.isAndroid || isDemo) return;
    await Permission.manageExternalStorage.request();
    unawaited(scan());
  }

  // ---------------------------------------------------------------- ingest
  void _applyWatch() {
    _watchSub?.cancel();
    _watchSub = null;
    if (settings.watchGallery && !isDemo) {
      _watchSub = mediaSource.changes.listen((_) {
        _scanDebounce?.cancel();
        _scanDebounce = Timer(const Duration(seconds: 2), scan);
      });
    }
  }

  bool _scanning = false;
  bool _rescan = false;

  /// Pages through everything added since the persisted high-water mark (DATE_ADDED seconds).
  Future<void> scan() async {
    if (isDemo || !Platform.isAndroid || !mediaPermitted) return;
    if (_scanning) {
      _rescan = true;
      return;
    }
    _scanning = true;
    try {
      const pageSize = 100;
      var since = _prefs.getInt('ingestSince') ??
          (DateTime.now().millisecondsSinceEpoch ~/ 1000 - 7 * 24 * 3600); // first run: last 7 days only
      for (var page = 0; page < 200; page++) {
        final files = await mediaSource.scan(limit: pageSize, sinceSec: since);
        if (files.isEmpty) break;
        _addIncoming(files);
        final maxSec = files.map((f) => f.addedMs ~/ 1000).reduce((a, b) => a > b ? a : b);
        if (files.length < pageSize) {
          since = maxSec > since ? maxSec : since;
          break;
        }
        if (maxSec <= since) break;
        since = maxSec;
      }
      await _prefs.setInt('ingestSince', since);
    } catch (e) {
      debugPrint('scan failed: $e');
    } finally {
      _scanning = false;
      if (_rescan) {
        _rescan = false;
        unawaited(scan());
      }
    }
  }

  void _addIncoming(List<IncomingFile> files) {
    var added = false;
    for (final f in files) {
      if (_photos.containsKey(f.id)) continue;
      _photos[f.id] = Photo(id: f.id, name: f.name, addedMs: f.addedMs, uri: f.uri, path: f.path);
      if (frozen) _held.add(f.id);
      added = true;
    }
    if (!added) return;
    changed();
    for (final ph in _photos.values) {
      if (ph.status == PhotoStatus.pending) _enqueue(ph.id);
    }
  }

  Future<void> importFromPicker() async {
    if (isDemo) return;
    try {
      final files = await pickerSource.pick();
      _addIncoming(files);
      if (files.isEmpty) showToast('没有导入任何文件');
    } catch (e) {
      showToast('导入失败');
    }
  }

  /// Test / demo helper: a new photo arrives (held when the grid is frozen).
  void simulateArrival(int demoIndex) {
    final id = 'demo_new_${_photos.length}';
    final spec = demoPhotos[demoIndex % demoPhotos.length];
    final ph = Photo(
        id: id,
        name: '${spec.name}.ARW',
        addedMs: now.millisecondsSinceEpoch + _photos.length,
        status: PhotoStatus.done,
        demo: demoIndex % demoPhotos.length,
        ratio: spec.ratio,
        exif: spec.exif,
        place: '外滩');
    _photos[id] = ph;
    if (frozen) _held.add(id);
    notifyListeners();
  }

  // ---------------------------------------------------------------- source path resolution
  /// Returns a readable local path for FFI. Copies content:// into cache when the
  /// MediaStore path cannot be opened directly. [temp] tells the caller to delete it afterwards.
  Future<({String path, bool temp})> resolveSource(Photo ph) async {
    final direct = ph.path;
    if (direct != null) {
      try {
        final f = File(direct);
        final raf = await f.open();
        await raf.close();
        return (path: direct, temp: false);
      } catch (_) {}
    }
    if (ph.uri == null) throw '找不到原始文件';
    final dest = p.join(
        _cache.path, 'src', '${DateTime.now().microsecondsSinceEpoch}_${ph.id.replaceAll(':', '_')}_${ph.name}');
    await MediaBridge.copyToFile(ph.uri!, dest);
    return (path: dest, temp: true);
  }

  // ---------------------------------------------------------------- develop queue
  final List<String> _queue = [];
  bool _working = false;

  void _enqueue(String id) {
    if (_queue.contains(id)) return;
    _queue.add(id);
    _pump();
  }

  void retry(Photo ph, {bool quiet = false}) {
    if (ph.id.startsWith('ftp:arr:')) {
      // An interrupted camera upload can only be resent by the camera.
      _photos.remove(ph.id);
      if (!quiet) showToast('请在相机上重新发送');
      changed();
      return;
    }
    if (isDemo) {
      ph.status = PhotoStatus.done;
      ph.progress = 1;
      if (!quiet) showToast('已重新传输');
      changed();
      return;
    }
    ph.status = PhotoStatus.pending;
    ph.error = null;
    changed();
    _enqueue(ph.id);
  }

  Future<void> _pump() async {
    if (_working || !nativeOk) return;
    _working = true;
    try {
      while (_queue.isNotEmpty) {
        _queue.sort((a, b) => (_photos[b]?.addedMs ?? 0).compareTo(_photos[a]?.addedMs ?? 0));
        final ph = _photos[_queue.removeAt(0)];
        if (ph == null || ph.status == PhotoStatus.done) continue;
        await _develop(ph);
      }
    } finally {
      _working = false;
    }
    _scheduleDetect();
  }

  String previewPathFor(Photo ph) => p.join(previews.path, '${ph.id.replaceAll(':', '_')}.jpg');

  Future<void> _develop(Photo ph) async {
    ph.status = PhotoStatus.developing;
    changed(save: false);
    String? tempPath;
    DappleSession? s;
    try {
      final src = await resolveSource(ph);
      if (src.temp) tempPath = src.path;
      s = await DappleSession.open(src.path, full: false, longEdge: 2048);
      final info = await s.info();
      ph.camera = ((info['camera'] as Map?)?['displayName'] ?? info['model'])?.toString();
      final w = (info['width'] as num?)?.toDouble(), h = (info['height'] as num?)?.toDouble();
      if (w != null && h != null && h > 0) {
        ph.ratio = w / h;
        ph.pxW = w.round();
        ph.pxH = h.round();
      }
      final ts = (info['timestamp'] as num?)?.toInt() ?? 0;
      if (ts > 0 && ph.takenMs == null) ph.takenMs = ts * 1000;
      final auto = await s.auto(settings.toneStrength / 100);
      ph.correction = (auto['correction'] as Map?)?.cast<String, dynamic>();
      final an = await s.analyze();
      if (an != null) ph.nearest = an['nearest'] as String?;
      final out = previewPathFor(ph);
      await s.exportJpeg(wireParams(ph), longEdge: 2048, quality: 88, outPath: out);
      ph.previewPath = out;
      _dropImageCache();
      if ((ph.kind == PhotoKind.hdr || ph.kind == PhotoKind.pano) && await isBlankImageFile(out)) {
        _removeBlankMerge(ph);
        return;
      }
      ph.status = PhotoStatus.done;
      ph.error = null;
      if (ph.camera != null && cameraSource) cameraName = ph.camera!;
    } catch (e) {
      ph.status = PhotoStatus.failed;
      ph.error = '$e';
    } finally {
      await s?.close();
      if (tempPath != null) {
        try {
          await File(tempPath).delete();
        } catch (_) {}
      }
    }
    changed();
  }

  /// A merge result that renders completely black is useless (old builds could save one); drop it instead of showing a black tile.
  void _removeBlankMerge(Photo ph) {
    _photos.remove(ph.id);
    _tryDelete(ph.path);
    _tryDelete(ph.previewPath);
    showToast('已移除一张空白的合成结果，请重新合成');
    changed();
  }

  /// One-off startup audit of already-saved HDR/panorama results.
  Future<void> _auditMerged() async {
    if (isDemo) return;
    for (final ph in _photos.values.where((e) => (e.kind == PhotoKind.hdr || e.kind == PhotoKind.pano) && e.status == PhotoStatus.done && e.previewPath != null).toList()) {
      if (await isBlankImageFile(ph.previewPath!)) _removeBlankMerge(ph);
    }
  }

  /// Preview files are rewritten in place; evict decoded copies.
  void _dropImageCache() {
    final c = PaintingBinding.instance.imageCache;
    c.clear();
    c.clearLiveImages();
  }

  /// Re-writes the grid thumbnail (2048 px JPEG) after the editor closes.
  Future<void> commitEdit(Photo ph) async {
    ph.exported = false;
    changed();
    if (isDemo || !nativeOk || ph.previewPath == null) return;
    try {
      final s = await sessions.get(ph);
      if (s == null) return;
      await s.exportJpeg(wireParams(ph), longEdge: 2048, quality: 88, outPath: previewPathFor(ph));
      _dropImageCache();
      notifyListeners();
    } catch (e) {
      debugPrint('commitEdit: $e');
    }
  }

  // ---------------------------------------------------------------- export (publish / save)
  /// Renders a full-quality JPEG of [ph] into the exports cache. [cropOverride] applies a
  /// destination aspect ratio at export time only (requirements 4.11.2).
  Future<String> exportJpeg(Photo ph, {int? longEdge, String? cropOverride}) async {
    final le = longEdge ?? settings.exportLongEdge; // 0 = original size
    final extra = <String, dynamic>{if (cropOverride != null && cropOverride != 'orig') 'crop': cropOverride};
    final params = wireParams(ph, override: extra);
    final tag = jsonEncode([params, le, settings.jpegQuality]).hashCode.toRadixString(16);
    final out = p.join(exports.path, '${p.basenameWithoutExtension(ph.name)}_dapple_$tag.jpg');
    if (await File(out).exists()) return out;
    final src = await resolveSource(ph);
    try {
      await DappleSession.exportOneShot(src.path, params,
          full: true, decodeLongEdge: le, outLongEdge: le, quality: settings.jpegQuality, outPath: out);
    } finally {
      if (src.temp) {
        try {
          await File(src.path).delete();
        } catch (_) {}
      }
    }
    return out;
  }

  final Set<String> _saving = {};

  /// True while [saveOneToAlbum] is exporting [id].
  bool isSaving(String id) => _saving.contains(id);

  /// One-tap "save to album" at the user's save size (original by default): toast while working, toast
  /// when done, and the photo's `exported` flag drives the check mark.
  Future<bool> saveOneToAlbum(Photo ph) async {
    if (!_saving.add(ph.id)) return false;
    showToast('正在保存…');
    var ok = false;
    try {
      ok = await saveToGallery([ph]) > 0;
    } finally {
      _saving.remove(ph.id);
    }
    if (ok) showToast('已存入相册「Dapple」');
    notifyListeners();
    return ok;
  }

  Future<int> saveToGallery(List<Photo> list, {int? longEdge, String? cropOverride}) async {
    var n = 0;
    for (final ph in list) {
      try {
        if (isDemo) {
          n++;
          continue;
        }
        final f = await exportJpeg(ph, longEdge: longEdge, cropOverride: cropOverride);
        await MediaBridge.saveToGallery(f, '${p.basenameWithoutExtension(ph.name)}_dapple.jpg');
        ph.exported = true;
        n++;
      } catch (e) {
        showToast('${ph.title} 保存失败');
      }
    }
    changed();
    return n;
  }

  // ---------------------------------------------------------------- detection (HDR sets / panorama sequences)
  List<DetectedGroup> detected = [];
  final Set<String> _detectScanned = {};
  final Set<String> _dismissed = {};
  Timer? _detectTimer;
  bool _detecting = false;
  bool get detecting => _detecting;

  File get _detectFile => File(p.join(_docs.path, 'detected1.json'));

  Future<void> _loadDetection() async {
    try {
      if (await _detectFile.exists()) {
        final j = jsonDecode(await _detectFile.readAsString()) as Map;
        _detectScanned.addAll((j['scanned'] as List).cast<String>());
        _dismissed.addAll(((j['dismissed'] as List?) ?? const []).cast<String>());
        detected = [
          for (final g in (j['groups'] as List)) DetectedGroup.fromJson((g as Map).cast<String, dynamic>())
        ].where((g) => g.photoIds.every(_photos.containsKey)).toList();
        applyGroupBadges(detected, (id) => _photos[id]);
      }
    } catch (e) {
      debugPrint('load detection failed: $e');
    }
  }

  Future<void> _saveDetection() async {
    if (isDemo) return;
    try {
      await _detectFile.writeAsString(jsonEncode({
        'scanned': _detectScanned.toList(),
        'dismissed': _dismissed.toList(),
        'groups': detected.map((g) => g.toJson()).toList(),
      }));
    } catch (_) {}
  }

  /// Groups shown as cards on the home screen, newest first.
  List<DetectedGroup> get detectedCards =>
      (detected.where((g) => !_dismissed.contains(g.key)).toList()..sort((a, b) => b.startMs.compareTo(a.startMs)))
          .take(4)
          .toList();

  void dismissDetected(DetectedGroup g) {
    _dismissed.add(g.key);
    _saveDetection();
    notifyListeners();
  }

  void _scheduleDetect() {
    if (isDemo || !ready) return;
    _detectTimer?.cancel();
    _detectTimer = Timer(const Duration(seconds: 3), () => unawaited(runDetection()));
  }

  /// Runs dapple_detect_sequences_json in a background isolate over the photos that were not scanned yet
  /// (plus their time neighbours, so a set that continues an older one is joined).
  Future<void> runDetection() async {
    if (isDemo || _detecting || !engine.available) return;
    _detecting = true;
    final handle = MergeHandle();
    try {
      final cands = _photos.values
          .where((e) =>
              e.kind != PhotoKind.hdr &&
              e.kind != PhotoKind.pano &&
              e.kind != PhotoKind.live &&
              e.status != PhotoStatus.arriving &&
              e.status != PhotoStatus.interrupted &&
              e.path != null &&
              (e.isRaw || isJpegName(e.name)) &&
              fileReadable(e.path!))
          .toList()
        ..sort((a, b) => b.sortMs.compareTo(a.sortMs));
      final recent = cands.take(300).toList();
      final fresh = recent.where((e) => !_detectScanned.contains(e.id)).toList();
      if (fresh.isEmpty) return;
      final sub = <Photo>{...fresh};
      for (final e in recent) {
        if (_detectScanned.contains(e.id) && fresh.any((f) => (f.sortMs - e.sortMs).abs() <= 30000)) sub.add(e);
      }
      final list = sub.toList()..sort((a, b) => a.sortMs.compareTo(b.sortMs));
      final idByPath = {for (final e in list) e.path!: e.id};
      final res = await engine.detect([for (final e in list) e.path!], handle);
      if (res == null) return;
      final timeById = {for (final e in list) e.id: e.sortMs};
      final groups = parseDetection(res, idByPath, timeById);
      final ids = {for (final e in list) e.id};
      for (final g in detected.where((g) => g.photoIds.any(ids.contains))) {
        for (final id in g.photoIds) {
          final ph = _photos[id];
          if (ph != null && (ph.kind == PhotoKind.bracket || ph.kind == PhotoKind.frame)) {
            ph.kind = PhotoKind.photo;
            ph.badge = null;
          }
        }
      }
      detected.removeWhere((g) => g.photoIds.any(ids.contains));
      detected.addAll(groups);
      applyGroupBadges(detected, (id) => _photos[id]);
      _detectScanned.addAll(ids);
      await _saveDetection();
      notifyListeners();
    } catch (e) {
      debugPrint('detect failed: $e');
    } finally {
      handle.dispose();
      _detecting = false;
    }
  }

  /// The tool draft for a detected group (source = auto detected).
  MergeDraft draftForGroup(DetectedGroup g) {
    final kind = g.isBracket ? ToolKind.hdr : ToolKind.pano;
    final srcs = <DraftSource>[];
    for (var i = 0; i < g.photoIds.length; i++) {
      final ph = _photos[g.photoIds[i]];
      if (ph == null) continue;
      final ev = g.evSteps[i];
      srcs.add(DraftSource(
          photoId: ph.id,
          label: g.isBracket ? (ev == null ? 'EV' : '${evNumber(ev)} EV') : '${i + 1}/${g.length}',
          timeMs: ph.sortMs,
          detected: true));
    }
    srcs.sort((a, b) => a.timeMs.compareTo(b.timeMs));
    return MergeDraft(
        kind: kind, sources: srcs, note: g.isBracket ? '已识别 ${srcs.length} 张包围曝光' : '已识别 ${srcs.length} 张连续拍摄');
  }

  /// The tool draft for photos picked in multi-select ("已带入你选的 N 张").
  MergeDraft draftForPhotos(ToolKind kind, List<Photo> list) {
    final srcs = [
      for (final ph in list) DraftSource(photoId: ph.id, label: candidateLabel(ph), timeMs: ph.sortMs, detected: false)
    ]..sort((a, b) => a.timeMs.compareTo(b.timeMs));
    return MergeDraft(kind: kind, sources: srcs, note: '已带入你选的 ${srcs.length} 张');
  }

  /// Bottom label of a candidate: EV / n/N when it is part of a detected set, else the capture time.
  String candidateLabel(Photo ph) => (ph.kind == PhotoKind.bracket || ph.kind == PhotoKind.frame) && ph.badge != null
      ? ph.badge!
      : _hhmm(DateTime.fromMillisecondsSinceEpoch(ph.sortMs));

  /// Photos the "从全部照片中选…" picker offers: processed photos and sequence frames (no videos), oldest first.
  List<Photo> get pickerCandidates =>
      (_photos.values.where((e) => e.status == PhotoStatus.done && e.kind != PhotoKind.video && e.kind != PhotoKind.live).toList()
        ..sort((a, b) => a.sortMs.compareTo(b.sortMs)));

  // ---------------------------------------------------------------- HDR / panorama / Motion Photo jobs
  ToolJob? job;

  /// Longest output edge of results (keeps a 3-frame HDR or a wide panorama within phone memory).
  static const int hdrLongEdge = 5000;
  static const int panoLongEdge = 10000;

  bool get hasBackgroundJob => job != null && job!.background && job!.status == JobStatus.running;

  Future<ToolJob> startMerge(MergeDraft d) async {
    final j = ToolJob(kind: d.kind, draft: d);
    job = j;
    notifyListeners();
    unawaited(_runMerge(j));
    return j;
  }

  void cancelJob(ToolJob j) {
    if (j.status != JobStatus.running) return;
    j.handle?.requestCancel();
    j.update(status: JobStatus.cancelled);
    if (job == j) job = null;
    notifyListeners();
  }

  void sendToBackground(ToolJob j) {
    j.update(background: true);
    notifyListeners();
  }

  /// Foreground jobs that finished are acknowledged by the tool page; background ones announce themselves.
  void _jobFinished(ToolJob j) {
    if (j.status == JobStatus.done) {
      if (j.background) showToast('${j.kind.resultName}已加入「今天」');
    } else if (j.status == JobStatus.failed && j.background) {
      showToast('没能对齐这几张', actionLabel: '查看', onAction: () => reopenTool?.call(j));
    }
    if (j.status != JobStatus.running && j.background && job == j) job = null;
    notifyListeners();
  }

  String _stamp() {
    final t = now;
    String two(int v) => v.toString().padLeft(2, '0');
    return '${t.year}${two(t.month)}${two(t.day)}_${two(t.hour)}${two(t.minute)}${two(t.second)}';
  }

  Future<void> _runMerge(ToolJob j) async {
    final d = j.draft!;
    final h = MergeHandle();
    j.handle = h;
    final temps = <String>[];
    Timer? poll;
    String? out;
    try {
      final srcs = [for (final s in d.checked) if (_photos[s.photoId] != null) _photos[s.photoId]!];
      if (srcs.length < 2) throw '至少选 2 张';
      final paths = <String>[];
      for (final ph in srcs) {
        if (isDemo) {
          paths.add(ph.id);
        } else {
          final r = await resolveSource(ph);
          paths.add(r.path);
          if (r.temp) temps.add(r.path);
        }
      }
      final stamp = _stamp();
      final name = '${d.kind == ToolKind.hdr ? 'HDR' : 'PANO'}_$stamp.dng';
      out = isDemo ? name : p.join(merged.path, name);
      poll = Timer.periodic(const Duration(milliseconds: 120), (_) {
        if (j.status == JobStatus.running && !h.disposed) j.update(fraction: h.fraction * 0.96);
      });
      final r = await engine.merge(MergeRequest(
          kind: d.kind.engineKind,
          paths: paths,
          handle: h,
          options: d.options(longEdge: d.kind == ToolKind.hdr ? hdrLongEdge : panoLongEdge),
          dngOut: out));
      poll.cancel();
      poll = null;
      if (j.status == JobStatus.cancelled) {
        if (!isDemo) _tryDelete(out);
        return;
      }
      if (!r.ok) {
        if (!isDemo) _tryDelete(out);
        j.update(status: r.cancelled ? JobStatus.cancelled : JobStatus.failed, error: r.error ?? '合成失败');
        _jobFinished(j);
        return;
      }
      final first = srcs.first;
      final ph = Photo(
        id: 'mrg:$stamp',
        name: name,
        addedMs: now.millisecondsSinceEpoch,
        path: isDemo ? null : out,
        status: isDemo ? PhotoStatus.done : PhotoStatus.pending,
        camera: first.camera,
        kind: d.kind.photoKind,
        badge: d.kind.resultName,
        takenMs: now.millisecondsSinceEpoch,
        demo: isDemo ? first.demo : null,
        ratio: isDemo ? first.ratio : null,
        exif: isDemo ? first.exif : null,
        place: first.place,
      );
      _photos[ph.id] = ph;
      changed();
      if (!isDemo) _enqueue(ph.id);
      j.result = ph;
      j.update(fraction: 1, status: JobStatus.done);
      _jobFinished(j);
    } catch (e) {
      j.update(status: j.status == JobStatus.cancelled ? JobStatus.cancelled : JobStatus.failed, error: '$e');
      _jobFinished(j);
    } finally {
      poll?.cancel();
      h.dispose();
      j.handle = null;
      for (final t in temps) {
        _tryDelete(t);
      }
    }
  }

  void _tryDelete(String? path) {
    if (path == null) return;
    try {
      final f = File(path);
      if (f.existsSync()) f.deleteSync();
    } catch (_) {}
  }

  // ---- Motion Photo
  /// Writable scratch folder for tool intermediates (filmstrip frames, previews).
  String get scratchPath => isDemo ? Directory.systemTemp.path : exports.path;

  VideoItem? latestVideo;
  List<VideoItem> videos = [];

  Future<void> refreshVideos() async {
    if (isDemo || !Platform.isAndroid) return;
    try {
      videos = await LiveBridge.listVideos(limit: 40);
      latestVideo = videos.isEmpty ? null : videos.first;
      notifyListeners();
    } catch (_) {
      // no permission yet / platform without MediaStore
    }
  }

  Future<ToolJob> startLive(LiveDraft d) async {
    final j = ToolJob(kind: ToolKind.live, live: d);
    job = j;
    notifyListeners();
    unawaited(_runLive(j));
    return j;
  }

  Future<void> _runLive(ToolJob j, {Photo? redo}) async {
    final d = j.live!;
    final stamp = _stamp();
    final tmp = isDemo ? Directory.systemTemp.path : _cache.path;
    final cover = p.join(tmp, 'live_cover_$stamp.jpg');
    final clip = p.join(tmp, 'live_clip_$stamp.mp4');
    final name = '${d.video.stem}_MP.jpg';
    final finalOut = redo?.path ?? p.join(isDemo ? tmp : liveDir.path, '${stamp}_$name');
    final out = redo == null ? finalOut : '$finalOut.new'; // a regeneration replaces the file only when it succeeded
    String? videoTemp;
    try {
      final startMs = (d.startSec * 1000).round();
      final durMs = (d.windowLen * 1000).round();
      final coverMs = (d.coverSec * 1000).round();
      j.update(fraction: 0.05);
      final still = await LiveBridge.frame(d.video.uri, coverMs, cover, quality: 94, source: d.source);
      debugPrint('live still: ${still.kind}');
      if (j.status == JobStatus.cancelled) return;
      j.update(fraction: 0.3);
      var clipStartMs = startMs;
      var engineDone = false;
      String? liveInfo;
      try {
        final t = await LiveBridge.trim(d.video.uri, startMs: startMs, durationMs: durMs, dest: clip, mute: d.mute, source: d.source);
        clipStartMs = t.actualStartMs;
        liveInfo = t.info?.summary;
        debugPrint('live clip: $liveInfo');
      } catch (e) {
        // A video that needed the software decoder was re-encoded there: the stream-copy cutter below would put the
        // undecodable (10-bit / 4:2:2) stream into the file, so a failure is a failure.
        if (await _needsSoftware(d.video.uri)) rethrow;
        // Platform trim failed (a codec / container the muxer cannot copy): use the engine's stream-copy cutter.
        debugPrint('platform trim failed ($e), engine cutter');
        final src = await resolveVideo(d.video);
        videoTemp = src.path;
        await engine.motionFromVideo(
            video: src.path,
            jpeg: cover,
            out: out,
            start: d.startSec,
            duration: d.windowLen,
            cover: d.coverSec,
            keepAudio: !d.mute);
        engineDone = true;
      }
      if (j.status == JobStatus.cancelled) return;
      j.update(fraction: 0.65);
      if (!engineDone) {
        final rel = ((coverMs - clipStartMs) / 1000.0).clamp(0.0, d.windowLen + 1.0).toDouble();
        await engine.writeMotionPhoto(jpeg: cover, mp4: clip, out: out, coverSec: rel);
      }
      j.update(fraction: 0.85);
      if (redo != null) File(out).renameSync(finalOut);
      await MediaBridge.saveToGallery(finalOut, name, replace: redo != null);
      final Photo ph;
      if (redo != null) {
        ph = redo
          ..previewPath = null
          ..status = isDemo ? PhotoStatus.done : PhotoStatus.pending
          ..error = null;
      } else {
        ph = Photo(
          id: 'live:$stamp',
          name: '${stamp}_$name',
          addedMs: now.millisecondsSinceEpoch,
          path: finalOut,
          status: isDemo ? PhotoStatus.done : PhotoStatus.pending,
          kind: PhotoKind.live,
          badge: '动态照片',
          takenMs: now.millisecondsSinceEpoch,
        );
      }
      ph
        ..liveSrc = d.video.uri
        ..liveStartMs = clipStartMs
        ..liveCoverMs = coverMs
        ..liveMute = d.mute
        ..liveSource = d.source.code
        ..liveInfo = liveInfo
        ..liveGen = Photo.liveGenCurrent;
      _photos[ph.id] = ph;
      changed();
      if (!isDemo) _enqueue(ph.id);
      j.result = ph;
      j.update(fraction: 1, status: JobStatus.done);
      _jobFinished(j);
    } catch (e) {
      _tryDelete(out);
      if (j.status != JobStatus.cancelled) {
        j.update(status: JobStatus.failed, error: e is UnsupportedVideoException ? e.userMessage : '$e');
      }
      _jobFinished(j);
    } finally {
      _tryDelete(cover);
      _tryDelete(clip);
      _tryDelete(videoTemp);
      if (isDemo && j.status != JobStatus.done) _tryDelete(out);
    }
  }

  Future<bool> _needsSoftware(String uri) async {
    try {
      return (await LiveBridge.probe(uri))['software'] == true;
    } catch (_) {
      return false;
    }
  }

  /// 兼容版 of a Motion Photo for sharing targets that cannot play a 10-bit HEVC clip or an Ultra HDR still:
  /// plain SDR JPEG + 8-bit H.264 clip (same window and cover, HDR / S-Log3 sources converted to SDR), written to the
  /// cache as `compat_<name>`; the saved master stays 10-bit. Returns null for items that were not made from a video.
  /// Plug-in point for the publish flow: call this instead of the master path for a target that needs it (the publish
  /// page does not offer Motion Photos yet).
  Future<String?> exportLiveCompat(Photo ph) async {
    final src = ph.liveSrc, startMs = ph.liveStartMs, coverMs = ph.liveCoverMs;
    if (src == null || startMs == null || coverMs == null) return null;
    final stamp = _stamp();
    final tmp = isDemo ? Directory.systemTemp.path : _cache.path;
    final cover = p.join(tmp, 'compat_cover_$stamp.jpg');
    final clip = p.join(tmp, 'compat_clip_$stamp.mp4');
    final out = p.join(tmp, 'compat_${ph.name}');
    try {
      final source = LiveSource.fromCode(ph.liveSource);
      await LiveBridge.frame(src, coverMs, cover, quality: 94, source: source, compat: true);
      final t = await LiveBridge.trim(src,
          startMs: startMs, durationMs: (LiveDraft.windowSec * 1000).round(), dest: clip, mute: ph.liveMute, source: source, compat: true);
      final rel = ((coverMs - t.actualStartMs) / 1000.0).clamp(0.0, LiveDraft.windowSec + 1.0).toDouble();
      await engine.writeMotionPhoto(jpeg: cover, mp4: clip, out: out, coverSec: rel);
      return out;
    } finally {
      _tryDelete(cover);
      _tryDelete(clip);
    }
  }

  static final RegExp _liveNameRe = RegExp(r'^\d{8}_\d{6}_(.+)_MP\.jpg$');

  /// Startup audit of Motion Photos written before the software decoder existed ([Photo.liveGen] < current).
  /// For each, the clip inside the file is probed: a stream no platform decoder can play (10-bit / 4:2:2, stream-copied
  /// by the old code, with a false-colour or unreadable still) means the item is bad. Bad items are rebuilt from their
  /// source video (found by file name in MediaStore) with the default centred window; when the source is gone the item is
  /// removed with a message. Items whose clip is fine are only stamped with the current revision.
  Future<void> auditLive() async {
    if (isDemo || !Platform.isAndroid) return;
    final stale = [for (final e in _photos.values) if (e.kind == PhotoKind.live && e.liveGen < Photo.liveGenCurrent) e];
    var rebuilt = 0;
    final removed = <String>[];
    for (final ph in stale) {
      try {
        final path = ph.path;
        if (path == null || !File(path).existsSync()) continue;
        final clip = await LiveBridge.probeEmbedded(path);
        if (clip['software'] != true) {
          ph.liveGen = Photo.liveGenCurrent;
          changed();
          continue;
        }
        final stem = _liveNameRe.firstMatch(ph.name)?.group(1);
        final v = stem == null ? null : await LiveBridge.findVideo(stem);
        if (v == null) {
          _photos.remove(ph.id);
          _tryDelete(path);
          _tryDelete(ph.previewPath);
          removed.add(stem ?? ph.name);
          changed();
          continue;
        }
        final d = LiveDraft(video: v);
        d.setStart(((v.seconds - d.windowLen) / 2).clamp(0.0, d.maxStart));
        d.coverSec = d.startSec + d.windowLen / 2;
        _tryDelete(ph.previewPath);
        final j = ToolJob(kind: ToolKind.live, live: d);
        await _runLive(j, redo: ph);
        if (j.status == JobStatus.done) {
          rebuilt++;
        } else {
          debugPrint('live regenerate failed for ${ph.name}: ${j.error}');
        }
      } catch (e) {
        debugPrint('live audit failed for ${ph.name}: $e');
      }
    }
    if (removed.isNotEmpty) {
      showToast('已移除 ${removed.length} 张动态照片（${removed.join('、')}）：原视频已不在，无法按新方式重新生成');
    } else if (rebuilt > 0) {
      showToast('已用新的解码方式重新生成 $rebuilt 张动态照片');
    }
  }

  /// Local copy of a video (the engine's cutter needs a file path).
  Future<({String path, bool temp})> resolveVideo(VideoItem v) async {
    final dest = p.join(_cache.path, 'src', '${DateTime.now().microsecondsSinceEpoch}_${v.name}');
    await MediaBridge.copyToFile(v.uri, dest);
    return (path: dest, temp: true);
  }

  // ---------------------------------------------------------------- camera FTP receiver (相机无线接收)
  bool ftpRunning = false;
  int ftpClients = 0;
  String? ftpError;
  List<FtpAddress> ftpAddresses = [];
  int ftpReceived = 0;
  final Map<String, String> _ftpArriving = {}; // file name -> placeholder photo id

  String? get ftpAddressText {
    final a = preferredAddress(ftpAddresses);
    return a == null ? null : '${a.ip}:${settings.ftpPort}';
  }

  Future<void> initFtp() async {
    if (isDemo || !Platform.isAndroid) return;
    _ftpSub ??= ftpBackend.events.listen(_onFtpEvent, onError: (_) {});
    await refreshFtpStatus();
    if (settings.ftpEnabled && !ftpRunning) await setFtpEnabled(true, persist: false);
    await scanFtpInbox();
  }

  Future<void> refreshFtpStatus() async {
    try {
      final st = await ftpBackend.status();
      ftpRunning = st.running;
      ftpClients = st.clients;
      ftpError = st.error;
      ftpAddresses = st.addresses;
      _syncCameraSource();
      notifyListeners();
    } catch (_) {}
  }

  Future<void> setFtpEnabled(bool on, {bool persist = true}) async {
    if (persist) await updateSettings((s) => s.ftpEnabled = on);
    if (isDemo) {
      ftpRunning = on;
      _syncCameraSource();
      notifyListeners();
      return;
    }
    try {
      if (on) {
        // Android 13+: the foreground notification needs the notification permission.
        if (Platform.isAndroid) await Permission.notification.request();
        await ftpBackend.start(port: settings.ftpPort, user: settings.ftpUser, pass: settings.ftpPass);
        await Future<void>.delayed(const Duration(milliseconds: 400));
      } else {
        await ftpBackend.stop();
        await Future<void>.delayed(const Duration(milliseconds: 200));
      }
    } catch (e) {
      ftpError = '$e';
      showToast('无法启动相机无线接收');
    }
    await refreshFtpStatus();
  }

  /// New random password (the running server is restarted with it).
  Future<void> regenerateFtpPassword() async {
    await updateSettings((s) => s.ftpPass = generateFtpPassword());
    if (ftpRunning && !isDemo) await setFtpEnabled(true, persist: false);
  }

  void _syncCameraSource() {
    cameraSource = isDemo || ftpRunning;
    if (cameraSource && cameraName.isEmpty) cameraName = '相机';
  }

  void _onFtpEvent(FtpEvent e) {
    switch (e.type) {
      case 'state':
        ftpRunning = e.running ?? ftpRunning;
        ftpError = e.error;
        _syncCameraSource();
        unawaited(refreshFtpStatus());
        break;
      case 'clients':
        ftpClients = e.count ?? 0;
        notifyListeners();
        break;
      case 'start':
        final name = e.name!;
        final id = 'ftp:arr:$name';
        _ftpArriving[name] = id;
        _photos[id] = Photo(
            id: id,
            name: name,
            addedMs: DateTime.now().millisecondsSinceEpoch,
            status: PhotoStatus.arriving,
            camera: cameraName.isEmpty ? null : cameraName);
        notifyListeners();
        break;
      case 'progress':
        final ph = _photos[_ftpArriving[e.name] ?? ''];
        if (ph != null) {
          final expect = isJpegName(ph.name) ? 12e6 : 40e6;
          ph.progress = ((e.bytes ?? 0) / expect).clamp(0.02, 0.95).toDouble();
          notifyListeners();
        }
        break;
      case 'failed':
        final ph = _photos[_ftpArriving.remove(e.name) ?? ''];
        if (ph != null) {
          ph.status = PhotoStatus.interrupted;
          changed();
        }
        break;
      case 'done':
        final id = _ftpArriving.remove(e.name);
        if (id != null) _photos.remove(id);
        ftpReceived++;
        unawaited(scanFtpInbox());
        break;
    }
  }

  /// Adds everything the service finished receiving that the library does not know yet.
  Future<void> scanFtpInbox() async {
    if (isDemo || !Platform.isAndroid) return;
    try {
      ingestFtpFiles(await ftpSource.scan());
    } catch (e) {
      debugPrint('ftp scan failed: $e');
    }
  }

  /// RAW+JPEG pairing: a RAW and a JPEG with the same name are one photo (the RAW; the JPEG is kept as
  /// its in-camera look and instant thumbnail). Works whichever of the two arrives first.
  void ingestFtpFiles(List<IncomingFile> files) {
    var added = false;
    for (final f in files) {
      if (_photos.values.any((e) => e.id == f.id || e.pairedJpeg == f.path)) continue;
      final stem = pairStem(f.name);
      if (isJpegName(f.name)) {
        final raw = _photos.values.where((e) => e.id.startsWith('ftp:') && isRawName(e.name) && pairStem(e.name) == stem);
        if (raw.isNotEmpty) {
          final r = raw.first;
          r.pairedJpeg = f.path;
          if (r.status != PhotoStatus.done) r.previewPath = f.path;
          added = true;
          continue;
        }
      }
      String? pair;
      if (isRawName(f.name)) {
        final jpg =
            _photos.values.where((e) => e.id.startsWith('ftp:') && isJpegName(e.name) && pairStem(e.name) == stem).toList();
        for (final j in jpg) {
          pair = j.path;
          _photos.remove(j.id);
        }
      }
      final ph = Photo(id: f.id, name: f.name, addedMs: f.addedMs, path: f.path, pairedJpeg: pair, previewPath: pair);
      if (frozen) _held.add(f.id);
      _photos[f.id] = ph;
      added = true;
    }
    if (!added) return;
    changed();
    for (final ph in _photos.values) {
      if (ph.status == PhotoStatus.pending) _enqueue(ph.id);
    }
    _scheduleDetect();
  }

  // ---------------------------------------------------------------- demo data
  void _loadDemo(DemoScenario scenario) {
    if (scenario == DemoScenario.empty) {
      linkDown = false;
      return;
    }
    final n = now;
    final today = DateTime(n.year, n.month, n.day, 14, 20);
    final yest = DateTime(n.year, n.month, n.day - 1, 19, 5);
    // today, newest first on screen: arriving, 花市(预览), then processed
    final todayOrder = [(-1, 'arriving'), (9, 'preview'), (8, ''), (7, ''), (6, 'flag'), (5, ''), (4, ''), (3, ''), (2, ''), (1, 'adjusted'), (0, '')];
    var i = 0;
    for (final o in todayOrder.reversed) {
      final idx = o.$1 < 0 ? 1 : o.$1;
      final spec = demoPhotos[idx];
      final status = o.$2 == 'arriving'
          ? PhotoStatus.arriving
          : o.$2 == 'preview'
              ? PhotoStatus.developing
              : PhotoStatus.done;
      final id = 'd${_photos.length}';
      final ph = Photo(
        id: id,
        name: o.$1 < 0 ? 'DSC0${8100 + i}.ARW' : '${spec.name}.ARW',
        addedMs: today.add(Duration(minutes: i * 3)).millisecondsSinceEpoch,
        status: status,
        demo: idx,
        ratio: spec.ratio,
        exif: spec.exif,
        place: '外滩',
        progress: 0.5,
        flag: o.$2 == 'flag',
        camera: 'Sony α7R V',
        edit: o.$2 == 'adjusted' ? const EditState(values: {'exposure': 12, 'contrast': 8}) : EditState.empty,
      );
      _photos[id] = ph;
      i++;
    }
    // yesterday: 3 bracket frames, 4 pano frames, 1 video
    final yItems = <(int, PhotoKind, String)>[
      (1, PhotoKind.bracket, '−2 EV'),
      (1, PhotoKind.bracket, '0 EV'),
      (1, PhotoKind.bracket, '+2 EV'),
      (5, PhotoKind.frame, '1/4'),
      (5, PhotoKind.frame, '2/4'),
      (5, PhotoKind.frame, '3/4'),
      (5, PhotoKind.frame, '4/4'),
      (7, PhotoKind.video, '0:14'),
    ];
    for (var k = 0; k < yItems.length; k++) {
      final spec = demoPhotos[yItems[k].$1];
      final id = 'y$k';
      _photos[id] = Photo(
        id: id,
        name: '${spec.name}${k + 1}.ARW',
        addedMs: yest.add(Duration(minutes: k)).millisecondsSinceEpoch,
        status: PhotoStatus.done,
        demo: yItems[k].$1,
        ratio: spec.ratio,
        exif: spec.exif,
        place: '陆家嘴',
        kind: yItems[k].$2,
        badge: yItems[k].$3,
        camera: 'Sony α7R V',
      );
    }
    detected = [
      DetectedGroup(
          kind: 'bracket',
          confidence: 0.9,
          photoIds: ['y0', 'y1', 'y2'],
          evSteps: [-2, 0, 2],
          startMs: yest.millisecondsSinceEpoch),
      DetectedGroup(
          kind: 'panorama',
          confidence: 0.9,
          photoIds: ['y3', 'y4', 'y5', 'y6'],
          evSteps: [0, 0, 0, 0],
          startMs: yest.millisecondsSinceEpoch + 180000),
    ];
    latestVideo = VideoItem(
        id: 'v1', uri: 'content://demo/v1', name: 'C0021.MP4', durationMs: 14000, size: 1, addedMs: yest.millisecondsSinceEpoch);
    if (scenario == DemoScenario.transferFailed) {
      final t = _photos.values.firstWhere((e) => e.status == PhotoStatus.done && e.kind == PhotoKind.photo);
      t.status = PhotoStatus.interrupted;
      final a = _photos.values.firstWhere((e) => e.status == PhotoStatus.arriving);
      a.status = PhotoStatus.done;
    }
    if (scenario == DemoScenario.disconnected) {
      linkDown = true;
      final a = _photos.values.firstWhere((e) => e.status == PhotoStatus.arriving);
      a.status = PhotoStatus.done;
    }
  }

  @override
  void dispose() {
    _watchSub?.cancel();
    _ftpSub?.cancel();
    _detectTimer?.cancel();
    _scanDebounce?.cancel();
    _saveTimer?.cancel();
    sessions.dispose();
    super.dispose();
  }
}
