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
import '../services/live_edit.dart';
import '../services/safe_name.dart';
import '../platform/platform_services.dart';
import '../services/media_permission.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../model/catalog.dart';
import '../model/film_catalog.dart';
import '../model/bases.dart' show baseNameOf;
import '../model/film_names.dart' show parseFilmSwatches;
import '../model/params.dart';
import '../models.dart';
import '../native/dapple_session.dart';
import '../services/blank_check.dart';
import '../services/capture.dart';
import '../services/detection.dart';
import '../platform/usb_camera.dart';
import '../services/ftp_ingest.dart';
import '../services/usb_ingest.dart';
import '../services/ingest.dart';
import '../services/ingest_log.dart';
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

  /// Phone-camera capture (testing convenience, requirements 4.2.6); tests substitute a fake.
  CaptureBackend captureBackend = PlatformServices.current.capture;
  Directory? captureInboxOverride; // tests
  /// Embeds the viewfinder JPEG of a RAW-only capture into its DNG (tests substitute a fake).
  Future<bool> Function(String dng, String jpeg) dngThumbnailEmbedder = DappleSession.embedDngThumbnail;

  /// Engine entry points of the HDR / panorama / Motion Photo tools (tests substitute a fake).
  MergeEngine engine = const NativeMergeEngine();

  /// False where the platform has no video layer (iOS today): the 视频 → 动态照片 entry is not offered. Demo / tests: true.
  late bool videoLayer = isDemo ? true : PlatformServices.current.video.available;
  FtpBackend ftpBackend = ChannelFtpBackend();
  late final FtpIngestSource ftpSource = FtpIngestSource(ftpBackend);
  StreamSubscription<FtpEvent>? _ftpSub;

  /// Camera on the USB cable (wired import); tests substitute the backend before [init].
  UsbCameraBackend usbBackend = UsbCameraBackend.forPlatform();
  late final UsbIngest usb = UsbIngest(usbBackend);
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
      if (!film.isEmpty) {
        unawaited(_loadFilmSwatches(bytes.length).whenComplete(_prebakeFilmWhenIdle));
      }
    } catch (e) {
      debugPrint('film pack failed: $e');
    }
  }

  /// Bump when the engine's swatch patches change (invalidates the cached swatches).
  static const _swatchVersion = 2;

  /// Swatches of the stocks, rendered by the engine from fixed test patches (a few seconds, once per pack): read from
  /// the cache file when its key (version, pack size, stock ids) matches, else computed in a background isolate and cached.
  /// Once the swatches are done and the GPU probe has finished (so the tables reach the disk cache): bakes every stock's tables in the background.
  Future<void> _prebakeFilmWhenIdle() async {
    for (var i = 0; i < 60; i++) {
      final st = DappleSession.gpuStatusJson() ?? '';
      if (st.contains('"state":"ready"') || st.contains('"state":"failed"')) break;
      await Future<void>.delayed(const Duration(milliseconds: 500));
    }
    DappleSession.startFilmPrebake();
  }

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

  /// Finer than [mediaPermitted]: limited = only the photos the user picked are visible; blocked = refused, only system Settings can change it.
  MediaAccess mediaAccess = MediaAccess.full;
  bool get mediaLimited => mediaPermitted && mediaAccess == MediaAccess.limited;
  bool get mediaBlocked => !mediaPermitted && mediaAccess == MediaAccess.blocked;

  /// True once a library scan has finished since the app started (the empty state must not claim "no new photos" before that).
  bool scanDone = false;
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

  /// A camera transfer that broke (a photo that could not be developed has its own wording on its tile and no effect on the camera status).
  static bool _transferBroken(Photo e) => e.status == PhotoStatus.interrupted || (e.status == PhotoStatus.failed && !e.developFailed);

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
    if (usbCardActive) {
      return switch (usb.phase) {
        UsbPhase.ready => ConnState.connected,
        UsbPhase.unplugged => ConnState.disconnected,
        UsbPhase.error => ConnState.interrupted,
        _ => ConnState.idle,
      };
    }
    if (linkDown) return ConnState.disconnected;
    if (_photos.isEmpty) return ConnState.idle;
    if (_photos.values.any(_transferBroken)) {
      return ConnState.interrupted;
    }
    if (!cameraSource) return ConnState.idle;
    // FTP receiver on but no camera talking to it yet: waiting, not connected.
    if (!isDemo && ftpRunning && ftpClients == 0 && arrivingCount == 0) return ConnState.idle;
    return ConnState.connected;
  }

  int get interruptedCount =>
      _photos.values.where(_transferBroken).length;
  int get arrivingCount => _photos.values.where((e) => e.status == PhotoStatus.arriving).length;
  int get processedCount => _photos.values.where((e) => e.status == PhotoStatus.done).length;
  int get processingCount => _photos.values.where((e) => e.processing).length;

  /// Progress of the arriving photo (0..1) or null.
  double? get arrivingProgress {
    final a = _photos.values.where((e) => e.status == PhotoStatus.arriving);
    if (a.isEmpty) return null;
    return a.first.progress;
  }

  /// The production app without the photo permission: not a lost link, just nothing to read yet.
  bool get needsMediaPermission => !isDemo && !mediaPermitted;

  String get connTitle {
    if (usbCardActive) return usb.cardTitle;
    if (needsMediaPermission) return '还不能读取照片';
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
    if (usbCardActive) return usb.cardLine;
    // the action says what to do (允许访问 / 打开设置): the line only states what happened, or nothing (audit C)
    if (needsMediaPermission) return mediaBlocked ? '已被系统拒绝' : '';
    switch (connState) {
      case ConnState.disconnected:
        return '最后收到第 ${_photos.length} 张';
      case ConnState.idle:
        // the address is a settings detail (audit F22); the card only says where to find it
        if (!isDemo && ftpRunning) return ftpAddressText == null ? '未连接 Wi-Fi 或热点' : '设置里看地址';
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
    if (usbCardActive) return usb.cardAction;
    if (needsMediaPermission) return mediaBlocked ? '打开设置' : '允许访问';
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
    if (usbCardActive) {
      // the picker of the card's photos is opened by the card itself (it needs a context); asking again is all that is left here
      if (usb.phase == UsbPhase.permission || usb.phase == UsbPhase.error) await usb.retry();
      return;
    }
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
          // Wireless transfer works (camera FTP ingest, see the settings section): only the gallery permission can be (re)granted here.
          if (!mediaPermitted) {
            await requestPermission();
            if (!mediaPermitted && !mediaBlocked) showToast('需要允许访问照片');
          } else {
            showToast('等待相机');
          }
        }
        break;
      case ConnState.connected:
        break;
    }
  }

  void retryAll() {
    for (final ph in _photos.values.where(_transferBroken)) {
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
    applyEdit(id, (_) => freshEdit);
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

  /// The rendering base new photos start with (默认成像): a film stock id, a modern id, or null = standard. Existing photos keep
  /// their own base: it is copied into the edit at ingest ([_withDefaultBase]).
  void setDefaultBase(String? baseId) {
    final old = settings.defaultBaseId;
    if (old == baseId) return;
    EditState.defaultBase = baseId;
    updateSettings((s) => s.defaultBaseId = baseId);
    showToast('新照片默认使用「${baseNameOf(film, baseId)}」', actionLabel: '撤销', onAction: () {
      EditState.defaultBase = old;
      updateSettings((s) => s.defaultBaseId = old);
    });
  }

  /// A photo as it arrives: the default rendering base is its starting edit (the same way the default look is its starting look).
  Photo _withDefaultBase(Photo ph) {
    final b = settings.defaultBaseId;
    if (b != null && ph.edit.film == null) ph.edit = ph.edit.copyWith(film: b);
    return ph;
  }

  /// Edit of a photo nobody has touched: empty but for the default base.
  EditState get freshEdit => settings.defaultBaseId == null ? EditState.empty : EditState.empty.copyWith(film: settings.defaultBaseId);

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
      defaultBaseId: _prefs.getString('defaultBase'),
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
    EditState.defaultBase = settings.defaultBaseId;
    await _prefs.setString('ftpPass', settings.ftpPass);
    nativeOk = DappleSession.isAvailable;
    if (!nativeOk) nativeError = '原生库未加载';
    await _loadFilm();
    await _loadLibrary();
    _repairPaths();
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
    unawaited(initUsb());
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

  /// The app container moves when the app is reinstalled (iOS) and the library stores absolute paths: a thumbnail or an imported original
  /// is looked up again by file name in its folder, and a developed photo whose thumbnail is really gone is developed again.
  /// Called once after the library is loaded.
  void _repairPaths() {
    String? find(String? path, Directory dir) {
      if (path == null || File(path).existsSync()) return path;
      final c = p.join(dir.path, p.basename(path));
      return File(c).existsSync() ? c : path;
    }

    var n = 0;
    for (final ph in _photos.values) {
      final src = find(ph.path, inbox), prev = find(ph.previewPath, previews);
      if (src != ph.path || prev != ph.previewPath) n++;
      ph.path = src;
      ph.previewPath = prev;
      if (ph.status == PhotoStatus.done && ph.previewPath != null && !File(ph.previewPath!).existsSync()) {
        ph.previewPath = null;
        ph.status = PhotoStatus.pending;
        n++;
        ingestLog('thumbnail of ${ph.name} is missing, developing again');
      }
    }
    if (n > 0) ingestLog('library repaired: $n photo(s) re-linked or queued');
  }

  final Set<String> _previewRetried = {};

  /// A tile could not load its thumbnail file although the photo is done: develop it again (once per run).
  void previewMissing(Photo ph) {
    if (ph.status != PhotoStatus.done || !_previewRetried.add(ph.id)) return;
    ingestLog('thumbnail of ${ph.name} cannot be read, developing again');
    ph.previewPath = null;
    ph.status = PhotoStatus.pending;
    changed();
    if (!isDemo) _enqueue(ph.id);
  }

  Timer? _saveTimer;
  void _saveSoon() {
    if (isDemo) return;
    _saveTimer?.cancel();
    _saveTimer = Timer(const Duration(milliseconds: 500), () async {
      await _libFile.writeAsString(jsonEncode(_photos.values.where((e) => !e.id.startsWith('ftp:arr:') && !e.id.startsWith('usb:arr:')).map((e) => e.toJson()).toList()));
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
    if (settings.defaultBaseId == null) {
      await _prefs.remove('defaultBase');
    } else {
      await _prefs.setString('defaultBase', settings.defaultBaseId!);
    }
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
  @visibleForTesting
  MediaPermission mediaPerm = MediaPermission();
  @visibleForTesting
  bool permissionPlatform = Platform.isAndroid || Platform.isIOS;

  void _setAccess(MediaAccess a) {
    mediaAccess = a;
    mediaPermitted = a.listing;
    linkDown = !mediaPermitted;
  }

  /// Re-reads the system state (start, return from Settings or the system prompt); a newly granted access starts the scan at once.
  Future<void> refreshPermission() async {
    if (!permissionPlatform || isDemo) return;
    final was = mediaPermitted;
    _setAccess(await mediaPerm.level()); // READ_MEDIA_IMAGES on 13+, READ_EXTERNAL_STORAGE before; PhotoKit on iOS
    notifyListeners();
    if (mediaPermitted && !was) unawaited(scan());
  }

  /// The card's / settings row's action: the system prompt while it can be shown, the app's Settings page once the user has refused.
  Future<void> requestPermission() async {
    if (!permissionPlatform || isDemo) return;
    if (mediaBlocked) {
      await mediaPerm.openSettings(); // the state is re-read when the app resumes
      return;
    }
    _setAccess(await mediaPerm.requestLevel());
    notifyListeners();
    if (mediaPermitted) unawaited(scan());
  }

  /// Limited access: let the user pick more photos, then look again.
  Future<void> chooseMorePhotos() async {
    if (!permissionPlatform || isDemo) return;
    await mediaPerm.chooseMore();
    await refreshPermission();
    unawaited(scan());
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
    if (isDemo || !permissionPlatform || !mediaPermitted) return;
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
        ingestLog('scan: ${files.length} new from ${mediaSource.id}');
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
      scanDone = true;
      notifyListeners();
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
      _photos[f.id] = _withDefaultBase(Photo(id: f.id, name: f.name, addedMs: f.addedMs, uri: f.uri, path: f.path));
      if (frozen) _held.add(f.id);
      added = true;
    }
    if (!added) return;
    changed();
    for (final ph in _photos.values) {
      if (ph.status == PhotoStatus.pending) _enqueue(ph.id);
    }
  }

  /// Capture ids whose developed JPEG still has to be written to the gallery (a DNG-only shot has no camera JPEG).
  final Set<String> _galleryJpegPending = {};

  /// A phone-camera shot becomes a library photo like a camera arrival: the DNG is the source when the lens gave one
  /// (the camera JPEG, when there is one, is kept as its instant preview and the gallery copy), else the JPEG is.
  /// A DNG without a camera JPEG gets its thumbnail and its gallery JPEG (Pictures/Dapple, next to the DNG) from the
  /// engine's own develop of the DNG, as soon as that has run. Returns the new photo; development starts right away.
  Future<Photo> ingestCapture(CaptureResult r) async {
    final dir = captureInboxOverride ?? inbox;
    await dir.create(recursive: true);
    final stem = r.stem;
    final nowMs = DateTime.now().millisecondsSinceEpoch;
    final jpg = r.jpegPath == null ? null : await _moveInto(r.jpegPath!, p.join(dir.path, '$stem.jpg'));
    final raw = r.dngPath == null ? null : await _moveInto(r.dngPath!, p.join(dir.path, '$stem.dng'));
    final thumb = r.thumbPath;
    if (raw != null && jpg == null && thumb != null) {
      // RAW only: the DNG has no preview, so the engine's per-file exposure anchor would fall back to scene statistics; put the viewfinder frame of
      // the shot into the file itself (works after the file is copied elsewhere too). Failure is harmless: the anchor falls back.
      try {
        if (!await dngThumbnailEmbedder(raw, thumb)) debugPrint('capture: thumbnail not embedded in $raw');
      } catch (e) {
        debugPrint('capture: thumbnail embedding failed: $e');
      }
    }
    if (thumb != null) {
      try {
        await File(thumb).delete();
      } catch (_) {}
    }
    final src = raw ?? jpg!;
    final id = 'cap:$stem';
    final ph = Photo(
      id: id,
      name: p.basename(src),
      addedMs: nowMs,
      path: src,
      pairedJpeg: raw == null ? null : jpg,
      previewPath: jpg,
      takenMs: nowMs,
    );
    _photos[id] = _withDefaultBase(ph);
    if (jpg == null) _galleryJpegPending.add(id);
    changed();
    _enqueue(id);
    _scheduleDetect();
    unawaited(() async {
      try {
        if (jpg != null) await captureBackend.saveToGallery(jpg, '$stem.jpg', 'image/jpeg');
        if (raw != null) await captureBackend.saveToGallery(raw, '$stem.dng', 'image/x-adobe-dng');
      } catch (e) {
        debugPrint('capture gallery save failed: $e');
      }
    }());
    return ph;
  }

  /// Capture ids still waiting for their developed gallery JPEG (tests).
  @visibleForTesting
  Set<String> get galleryJpegPending => _galleryJpegPending;

  /// The developed JPEG of a DNG-only capture goes to the gallery next to the DNG (full size per the save-size setting).
  Future<void> saveCaptureJpegToGallery(Photo ph) async {
    if (!_galleryJpegPending.remove(ph.id)) return;
    try {
      final out = await exportStill(ph);
      await captureBackend.saveToGallery(out, '${p.basenameWithoutExtension(ph.name)}.jpg', 'image/jpeg');
    } catch (e) {
      debugPrint('capture gallery jpeg failed: $e');
    }
  }

  /// A photo whose original is gone or broken (audit F06): it leaves the list (the app's own preview goes with it; the phone's files are
  /// not touched). 撤销 puts it back.
  void removeFromLibrary(Photo ph) {
    if (_photos.remove(ph.id) == null) return;
    changed();
    showToast('已移除「${ph.title}」', actionLabel: '撤销', onAction: () {
      _photos[ph.id] = ph;
      changed();
    });
  }

  /// Debug builds only (DebugOpen `{"purgeCaptures":true}`): removes every in-app capture (`cap:` ids) and its private files from the library.
  /// The library has no delete action; this is how test shots are cleaned up on a device.
  int purgeCaptures() {
    final ids = _photos.keys.where((k) => k.startsWith('cap:')).toList();
    for (final id in ids) {
      final ph = _photos.remove(id)!;
      _tryDelete(ph.path);
      _tryDelete(ph.pairedJpeg);
      _tryDelete(ph.previewPath);
    }
    if (ids.isNotEmpty) changed();
    return ids.length;
  }

  Future<String> _moveInto(String from, String to) async {
    final f = File(from);
    try {
      await f.rename(to);
    } on FileSystemException {
      await f.copy(to);
      try {
        await f.delete();
      } catch (_) {}
    }
    return to;
  }

  /// True while picked originals are being copied into the app (large RAWs take a while); the home screen shows a status bar.
  bool importing = false;

  /// Replaces the native picker (tests). Gets the source ("photos" / "files") and the ids already in the library.
  Future<List<PickedItem>> Function(String source, List<String> known)? pickBackend;

  /// The library id a picked [key] already has, if any (the scan's ids are `ph:ASSET:RESOURCE` on iOS, `ms:ID` on Android).
  String? _existingFor(String key) {
    if (_photos.containsKey(key)) return key;
    if (key.startsWith('ph:')) {
      for (final id in _photos.keys) {
        if (id.startsWith('$key:')) return id;
      }
    }
    return null;
  }

  /// "从相册选择" / "从文件选择": the system picker (several photos), the originals are copied into the inbox and developed like any
  /// arrival. A photo that is already in the library is not added again. Returns the id of the photo to open when exactly one was
  /// chosen (it may be the existing one), else null. Cancelling is silent; failures and skipped files get a plain message.
  Future<String?> importFromPicker({String source = 'photos'}) async {
    if (importing || (isDemo && pickBackend == null)) return null;
    importing = true;
    notifyListeners();
    try {
      final known = _photos.keys.toList();
      final items = await (pickBackend?.call(source, known) ?? pickerSource.pickItems(source: source, known: known));
      if (items.isEmpty) return null;
      final fresh = <IncomingFile>[];
      var dups = 0, unsupported = 0, failed = 0;
      String? only;
      for (final it in items) {
        ingestLog('picked ${it.name}: ${it.dup ? 'already in library' : it.error ?? 'copied'}');
        final existing = _existingFor(it.key);
        if (existing != null) {
          dups++;
          only = existing;
          _discard(it.file?.path);
        } else if (it.error == 'unsupported') {
          unsupported++;
        } else if (it.file == null) {
          failed++;
        } else if (!fresh.any((f) => f.id == it.file!.id)) {
          fresh.add(it.file!);
          only = it.file!.id;
        }
      }
      _addIncoming(fresh);
      if (fresh.isNotEmpty && frozen) releaseNew(); // a pick is the user's own action: show it now
      final notes = <String>[
        if (unsupported > 0) '$unsupported 个文件格式不支持，已跳过',
        if (failed > 0) '$failed 张没读出来',
        if (dups > 0 && fresh.isEmpty) (dups == 1 ? '这张已经在照片里' : '所选照片已经都在照片里'),
      ];
      // reading again can help with the files that were not read; skipped formats and duplicates need nothing
      if (notes.isNotEmpty) {
        showToast(notes.join('；'), actionLabel: failed > 0 ? '重试' : null, onAction: failed > 0 ? () => importFromPicker(source: source) : null);
      }
      return items.length == 1 ? only : null;
    } catch (e) {
      showToast('没导入成功', actionLabel: '重试', onAction: () => importFromPicker(source: source));
      return null;
    } finally {
      importing = false;
      notifyListeners();
    }
  }

  /// Tests: puts a bare photo with [id] into the library.
  @visibleForTesting
  void simulatePhotoForTest(Object idOrPhoto) {
    final ph = idOrPhoto is Photo ? idOrPhoto : Photo(id: idOrPhoto as String, name: idOrPhoto, addedMs: 1, status: PhotoStatus.done);
    _photos[ph.id] = ph;
  }

  void _discard(String? path) {
    if (path == null) return;
    try {
      File(path).deleteSync();
    } catch (_) {}
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
  /// Types the engine does not read itself: converted to a 16-bit PNG in their own colour space by the platform decoder (ImageIO on iOS,
  /// ImageDecoder on Android 9+, an sRGB JPEG before; capture date, camera and orientation kept) into the cache, the caller deletes it
  /// ([temp]). JPEG and PNG go to the engine as they are: it reads their profile / tags and depth.
  static const _convertExts = {'.heic', '.heif', '.tif', '.tiff', '.webp', '.avif'};

  Future<({String path, bool temp})> resolveSource(Photo ph) async {
    final r = await _resolveRaw(ph);
    final ext = p.extension(r.path).toLowerCase();
    if (!_convertExts.contains(ext) || !(Platform.isIOS || Platform.isAndroid)) return r;
    final dest = p.join(_cache.path, 'src', '${DateTime.now().microsecondsSinceEpoch}_${p.basenameWithoutExtension(r.path)}.png');
    String written = dest;
    try {
      await Directory(p.dirname(dest)).create(recursive: true);
      written = await MediaBridge.toPng16(r.path, dest);
    } finally {
      if (r.temp) {
        try {
          await File(r.path).delete();
        } catch (_) {}
      }
    }
    return (path: written, temp: true);
  }

  Future<({String path, bool temp})> _resolveRaw(Photo ph) async {
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
        _cache.path, 'src', '${DateTime.now().microsecondsSinceEpoch}_${safeFileStem(ph.id)}_${safeFileName(ph.name)}');
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
    if (ph.id.startsWith('ftp:arr:') || ph.id.startsWith('usb:arr:')) {
      // An interrupted camera upload can only be resent by the camera.
      _photos.remove(ph.id);
      if (!quiet) showToast('已移除 · 从相机重新发送这张');
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
        if (ph.status == PhotoStatus.done && _galleryJpegPending.contains(ph.id)) await saveCaptureJpegToGallery(ph);
      }
    } finally {
      _working = false;
    }
    _scheduleDetect();
  }

  String previewPathFor(Photo ph) => p.join(previews.path, '${safeFileStem(ph.id)}.jpg');

  Future<void> _develop(Photo ph) async {
    ph.status = PhotoStatus.developing;
    changed(save: false);
    final sw = Stopwatch()..start();
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
      ingestLog('developed ${ph.name} ${ph.pxW}x${ph.pxH} in ${sw.elapsedMilliseconds} ms');
      if (ph.camera != null && cameraSource) cameraName = ph.camera!;
    } catch (e) {
      ph.status = PhotoStatus.failed;
      ph.error = '$e';
      ingestLog('FAILED ${ph.name} after ${sw.elapsedMilliseconds} ms: $e');
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
    showToast('已移除一张空白的合成结果');
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
  Future<String> exportJpeg(Photo ph, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    if (isDemo) return '/demo/${ph.id}.jpg';
    // A Motion Photo made from a video exports as a Motion Photo: the edited still plus the clip rendered with the same edit (film included).
    if (ph.kind == PhotoKind.live && ph.liveSrc != null && !isDemo) {
      final live = await exportLive(ph, longEdge: longEdge, cropOverride: cropOverride, stripLocation: stripLocation, hdr: hdr);
      if (live != null) return live;
    }
    return exportStill(ph, longEdge: longEdge, cropOverride: cropOverride, hdr: hdr, stripLocation: stripLocation, wideGamut: wideGamut);
  }

  final Map<String, Future<double>> _wideGamut = {};

  /// Share (0..1) of the picture of [ph], after its edit, that has colour outside sRGB (what a Display P3 export would keep). 0 for a film look
  /// (authored for sRGB), a Motion Photo, the demo. Cached per edit.
  Future<double> wideGamutOf(Photo ph) {
    if (isDemo || (ph.kind == PhotoKind.live && ph.liveSrc != null)) return Future.value(0);
    final params = wireParams(ph);
    final key = '${ph.id}|${jsonEncode(params)}';
    return _wideGamut.putIfAbsent(key, () async {
      try {
        final src = await resolveSource(ph);
        try {
          return await DappleSession.wideGamutFraction(src.path, params);
        } finally {
          if (src.temp) _tryDelete(src.path);
        }
      } catch (_) {
        return 0.0;
      }
    });
  }

  /// The wide-gamut export is offered when at least this share of the picture lies outside sRGB.
  static const double wideGamutMinShare = 0.01;

  final Map<String, Future<double>> _hdrHeadroom = {};

  /// Highlight headroom (stops above SDR white) the edit of [ph] has, 0 when an HDR export (Ultra HDR gain-map JPEG) is not worth offering: no
  /// highlights above white, a film look, a Motion Photo, the demo. Cached per edit.
  Future<double> hdrHeadroomOf(Photo ph) {
    if (isDemo || (ph.kind == PhotoKind.live && ph.liveSrc != null)) return Future.value(0);
    final params = wireParams(ph);
    final key = '${ph.id}|${jsonEncode(params)}';
    return _hdrHeadroom.putIfAbsent(key, () async {
      try {
        final src = await resolveSource(ph);
        try {
          return await DappleSession.hdrHeadroom(src.path, params);
        } finally {
          if (src.temp) _tryDelete(src.path);
        }
      } catch (_) {
        return 0.0;
      }
    });
  }

  /// Does the clip of [ph] go through the node graph? (the still has an edit, the automatic correction applies, or a destination crop is asked for)
  bool liveEdited(Photo ph, {String? cropOverride}) => liveClipEdited(
      userEdited: jsonEncode(ph.edit.toJson()) != jsonEncode(freshEdit.toJson()),
      autoCorrectionApplied: settings.autoProcess && (ph.correction?.isNotEmpty ?? false),
      cropOverride: cropOverride);

  /// The Motion Photo export: the edited still ([exportStill]) + the clip trimmed with [LiveBridge.trim] (with the still's edit when the still is
  /// edited, as recorded otherwise) + the container. Null = the clip could not be produced (source video gone, decoder failure): the caller falls back
  /// to the still alone, as before. [hdr] = the user chose HDR in the export options: an HDR clip's cover then becomes an Ultra HDR JPEG (the SDR still
  /// byte for byte + gain map) already at a lower headroom threshold; an HDR clip gets the gain map without the option when its cover frame has
  /// highlight headroom ([liveHdrCover], docs section 30.14).
  Future<String?> exportLive(Photo ph, {int? longEdge, String? cropOverride, bool? stripLocation, bool hdr = false}) async {
    final src = ph.liveSrc, startMs = ph.liveStartMs, coverMs = ph.liveCoverMs;
    if (src == null || startMs == null || coverMs == null) return null;
    final le = longEdge ?? settings.exportLongEdge;
    final extra = <String, dynamic>{if (cropOverride != null && cropOverride != 'orig') 'crop': cropOverride};
    final wire = wireParams(ph, override: extra);
    final edited = liveEdited(ph, cropOverride: cropOverride);
    final strip = stripLocation ?? settings.stripLocation;
    final tag = jsonEncode([wire, le, settings.jpegQuality, edited, startMs, coverMs, ph.liveMute, 'meta1', strip, 'cov1', hdr]).hashCode.toRadixString(16);
    final stem = p.basenameWithoutExtension(ph.name).replaceFirst(RegExp(r'_MP$'), '');
    final out = p.join(exports.path, '${stem}_dapple_${tag}_MP.jpg');
    if (await File(out).exists()) return out;
    final clip = p.join(exports.path, '${stem}_dapple_$tag.clip.mp4');
    final coverFile = p.join(exports.path, '${stem}_dapple_$tag.cover.jpg');
    try {
      final still = await exportStill(ph, longEdge: longEdge, cropOverride: cropOverride, stripLocation: strip);
      final edit = editedClipFor(wire: wire, edited: edited, coverMs: coverMs, stillLongEdge: le);
      final t = await LiveBridge.trim(src,
          startMs: startMs,
          durationMs: (LiveDraft.windowSec * 1000).round(),
          dest: clip,
          mute: ph.liveMute,
          source: LiveSource.fromCode(ph.liveSource),
          edit: edit);
      final rel = ((coverMs - t.actualStartMs) / 1000.0).clamp(0.0, LiveDraft.windowSec + 1.0).toDouble();
      final cover = await liveHdrCover(ph, still: still, wire: wire, le: le, coverMs: coverMs, hdr: hdr, dest: coverFile);
      await engine.writeMotionPhoto(jpeg: cover, mp4: clip, out: out, coverSec: rel);
      return out;
    } catch (e) {
      debugPrint('exportLive: $e (the still is exported alone)');
      _tryDelete(out);
      return null;
    } finally {
      _tryDelete(clip);
      _tryDelete(coverFile);
    }
  }

  /// The cover of a Motion Photo export: [still] itself, or, for an HDR clip whose cover frame has highlight headroom, [dest] = [still] byte for byte plus a
  /// gain map (Ultra HDR JPEG; the engine renders the cover frame through the still's edit, [LiveBridge.coverHdr]). Never throws: any failure leaves the SDR
  /// still as the cover.
  @visibleForTesting
  Future<String> liveHdrCover(Photo ph,
      {required String still, required Map<String, dynamic> wire, required int le, required int coverMs, required bool hdr, required String dest}) async {
    final src = ph.liveSrc;
    if (src == null) return still;
    try {
      final colorKind = ((await LiveBridge.probe(src))['colorKind'] as num?)?.toInt() ?? 0;
      if (!liveCoverWantsHdr(hdrOption: hdr, colorKind: colorKind)) return still;
      final r = await LiveBridge.coverHdr(src, coverMs,
          sdr: still,
          out: dest,
          edit: coverEditFor(wire: wire, coverMs: coverMs, stillLongEdge: le),
          source: LiveSource.fromCode(ph.liveSource),
          force: hdr);
      debugPrint('live cover: ${r.written ? 'Ultra HDR (peak ${r.peak.toStringAsFixed(2)}, gain ${r.gainMax.toStringAsFixed(2)} stops)' : 'SDR (reason ${r.reason})'}');
      return r.written ? r.path : still;
    } catch (e) {
      debugPrint('live cover: $e (SDR cover)');
      return still;
    }
  }

  /// Renders a full-quality JPEG of the still of [ph] (no clip).
  /// [stripLocation] null = the setting "导出时去除位置信息" (default on, requirements 4.11.2: GPS is stripped unless the user keeps it). The file carries
  /// Exif (camera, exposure, capture time) and an sRGB profile; GPS only when the original has a fix and the location is kept.
  Future<String> exportStill(Photo ph, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    final le = longEdge ?? settings.exportLongEdge; // 0 = original size
    final extra = <String, dynamic>{if (cropOverride != null && cropOverride != 'orig') 'crop': cropOverride};
    final params = wireParams(ph, override: extra);
    final strip = stripLocation ?? settings.stripLocation;
    final tag = jsonEncode([params, le, settings.jpegQuality, if (hdr) 'uhdr', if (wideGamut) 'p3', 'meta1', strip]).hashCode.toRadixString(16);
    final out = p.join(exports.path, '${p.basenameWithoutExtension(ph.name)}_dapple_$tag.jpg');
    if (await File(out).exists()) return out;
    final src = await _resolveWithOriginal(ph);
    try {
      await DappleSession.exportOneShot(src.path, params,
          full: true,
          decodeLongEdge: le,
          outLongEdge: le,
          quality: settings.jpegQuality,
          hdr: hdr,
          exportJson: exportMetaJson(original: src.original, stripLocation: strip, wideGamut: wideGamut),
          outPath: out);
    } finally {
      await src.cleanup();
    }
    return out;
  }

  /// The options string of the native export calls (dapple_c.h): Exif source file, location choice, colour space.
  @visibleForTesting
  static String exportMetaJson({required String original, required bool stripLocation, bool wideGamut = false}) =>
      jsonEncode({'metaSource': original, 'stripLocation': stripLocation, 'colorSpace': wideGamut ? 'p3' : 'srgb'});

  /// [resolveSource] that also names the file the user imported: a HEIC / TIFF / WebP is converted to PNG first and the copy has lost its Exif and GPS,
  /// so the export reads them from [original]. [cleanup] removes every temporary file.
  Future<({String path, String original, Future<void> Function() cleanup})> _resolveWithOriginal(Photo ph) async {
    final r = await _resolveRaw(ph);
    Future<void> drop(String f) async {
      try {
        await File(f).delete();
      } catch (_) {}
    }

    final ext = p.extension(r.path).toLowerCase();
    if (!_convertExts.contains(ext) || !(Platform.isIOS || Platform.isAndroid)) {
      return (path: r.path, original: r.path, cleanup: () async => r.temp ? drop(r.path) : null);
    }
    final dest = p.join(_cache.path, 'src', '${DateTime.now().microsecondsSinceEpoch}_${p.basenameWithoutExtension(r.path)}.png');
    try {
      await Directory(p.dirname(dest)).create(recursive: true);
      final written = await MediaBridge.toPng16(r.path, dest);
      return (
        path: written,
        original: r.path,
        cleanup: () async {
          await drop(written);
          if (r.temp) await drop(r.path);
        }
      );
    } catch (_) {
      if (r.temp) await drop(r.path);
      rethrow;
    }
  }

  final Set<String> _saving = {};

  /// True while [saveOneToAlbum] is exporting [id].
  bool isSaving(String id) => _saving.contains(id);

  /// Open publish / export sheets (the publish page counts itself in and out).
  int _exportSheets = 0;
  // called from a page's initState / dispose: listeners hear it after the frame, never during a build
  void exportSheetOpened() {
    _exportSheets++;
    Future.microtask(notifyListeners);
  }

  void exportSheetClosed() {
    if (_exportSheets > 0) _exportSheets--;
    Future.microtask(notifyListeners);
  }

  /// An export or share is being prepared: the editor hides its working overlays (the 局部 red tint), which belong to editing only.
  /// Exports are rendered by the engine from the edit parameters and never contain those overlays.
  bool get exportInProgress => _exportSheets > 0 || _saving.isNotEmpty;

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

  Future<int> saveToGallery(List<Photo> list, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    var n = 0;
    for (final ph in list) {
      try {
        if (isDemo) {
          n++;
          continue;
        }
        final f = await exportJpeg(ph, longEdge: longEdge, cropOverride: cropOverride, hdr: hdr, stripLocation: stripLocation, wideGamut: wideGamut);
        await MediaBridge.saveToGallery(f, f.endsWith('_MP.jpg') ? '${p.basenameWithoutExtension(ph.name).replaceFirst(RegExp(r'_MP$'), '')}_dapple_MP.jpg' : '${p.basenameWithoutExtension(ph.name)}_dapple.jpg');
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
        final raw = _photos.values.where((e) => _isWireless(e.id) && isRawName(e.name) && pairStem(e.name) == stem);
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
            _photos.values.where((e) => _isWireless(e.id) && isJpegName(e.name) && pairStem(e.name) == stem).toList();
        for (final j in jpg) {
          pair = j.path;
          _photos.remove(j.id);
        }
      }
      final ph = Photo(id: f.id, name: f.name, addedMs: f.addedMs, path: f.path, pairedJpeg: pair, previewPath: pair);
      if (frozen) _held.add(f.id);
      _photos[f.id] = _withDefaultBase(ph);
      added = true;
    }
    if (!added) return;
    changed();
    for (final ph in _photos.values) {
      if (ph.status == PhotoStatus.pending) _enqueue(ph.id);
    }
    _scheduleDetect();
  }

  // ---------------------------------------------------------------- camera on the USB cable (有线导入)
  bool get usbCardActive => usb.cardActive;

  /// Photos that came from the camera (FTP or the USB cable) are paired RAW + JPEG the same way.
  static bool _isWireless(String id) => id.startsWith('ftp:') || id.startsWith('usb:');

  Future<void> initUsb({String? inboxPath}) async {
    if (!usbBackend.available) return;
    usb.isKnown = usbKnown;
    usb.onArrivalStart = (name) {
      final id = 'usb:arr:$name';
      _photos[id] = Photo(
          id: id,
          name: name,
          addedMs: DateTime.now().millisecondsSinceEpoch,
          status: PhotoStatus.arriving,
          camera: usb.model.isEmpty ? null : usb.model);
      notifyListeners();
    };
    usb.onArrivalProgress = (name, f) {
      final ph = _photos['usb:arr:$name'];
      if (ph != null) ph.progress = f.clamp(0.02, 0.95).toDouble();
    };
    usb.onArrivalEnd = (name, ok) {
      if (_photos.remove('usb:arr:$name') != null) notifyListeners();
    };
    usb.onFiles = ingestFtpFiles;
    usb.addListener(notifyListeners);
    await usb.start(inboxPath ?? inbox.path);
  }

  /// A card file is already in the library: same identity key, or a photo with the same name and size (the shot came over FTP).
  bool usbKnown(UsbFile f) {
    if (_photos.containsKey(f.key)) return true;
    final n = f.name.toLowerCase();
    bool same(String? path) {
      if (path == null || p.basename(path).toLowerCase() != n) return false;
      try {
        return File(path).lengthSync() == f.size;
      } catch (_) {
        return false;
      }
    }

    return _photos.values.any((e) => same(e.path) || same(e.pairedJpeg));
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
    usb.dispose();
    _detectTimer?.cancel();
    _scanDebounce?.cancel();
    _saveTimer?.cancel();
    sessions.dispose();
    super.dispose();
  }
}
