// The seam between the shared Dart app and what only the platform side can do (docs/ios/platform-interface.md).
//
// Android implements it with the Kotlin plugins behind the `dapple/*` method channels (the existing classes used below are the
// Android implementations; nothing about them changed). iOS implements it with Swift (VideoToolbox / AVFoundation / PhotoKit,
// planned) and, until that exists, with the graceful stubs here and in ios_services.dart: every call returns "nothing" /
// "unsupported", never throws into the UI and never needs a channel that has no native handler.
//
// A feature asks `PlatformServices.current.<area>.available` before it shows an entry point; code that calls anyway gets a
// documented neutral result.
import 'dart:io';

import '../services/capture.dart';
import '../services/ftp_ingest.dart';
import '../services/ingest.dart';
import '../services/live_bridge.dart';
import '../services/share_service.dart';
import 'ios_services.dart';

/// Reading photos that arrive on the phone and writing results back to the user's library.
abstract class MediaLibrary {
  /// The listing / change source the library screen uses (Android: MediaStore, iOS: the app's Documents/Photos folder now,
  /// PhotoKit later).
  IngestSource get source;

  /// False when the platform needs a runtime permission that is not granted yet.
  Future<bool> permissionGranted();
  Future<bool> requestPermission();

  /// Makes the item readable by path for the engine (Android content:// copy). Null when it cannot be read.
  Future<String?> copyToFile(IncomingFile f, String destPath);

  /// Adds a finished file to the user's photo library (Android Pictures/Dapple, iOS Photos album "Dapple"). False when the
  /// platform cannot do it (yet).
  Future<bool> saveToLibrary(String path, String name, {bool replace = false});
}

/// Video side: listing, probing, the codec capability report (docs/engine/codec-capability.md) and Motion / Live Photo tools.
abstract class VideoPlatform {
  bool get available;
  Future<List<VideoItem>> listVideos({int limit = 50});

  /// `{decodable: bool, ...}`; an empty map when unsupported.
  Future<Map<String, dynamic>> probe(String uri);

  /// Capability report JSON (declared decoders / encoders, ladders, self tests); empty string when unsupported.
  Future<String> codecReport({bool run = false, bool force = false});

  /// `{main10Encoder: name|null, ultraHdr: bool, videoLib: bool}`; an empty map when unsupported.
  Future<Map<String, dynamic>> capabilities();
}

/// Everything the platform side provides, one object per process.
abstract class PlatformServices {
  /// Replaceable in tests.
  static PlatformServices current = _forThisPlatform();

  String get name; // 'android', 'ios', 'host'
  MediaLibrary get media;
  CaptureBackend get capture; // phone camera, RAW / ProRAW where the sensor offers it
  FtpBackend get wirelessTransfer; // camera -> phone transfer server (Android: FTP service)
  VideoPlatform get video;
  ShareService get share;

  /// True when `dapple/view` (a GPU texture the engine draws into) exists; otherwise the readback path is used.
  bool get gpuView;
}

PlatformServices _forThisPlatform() {
  if (Platform.isAndroid) return AndroidPlatformServices();
  if (Platform.isIOS) return IosPlatformServices();
  return NullServices('host');
}

// ----------------------------------------------------------------------------------------------------------- Android
class AndroidPlatformServices implements PlatformServices {
  @override
  String get name => 'android';
  @override
  final MediaLibrary media = _AndroidMedia();
  @override
  final CaptureBackend capture = ChannelCaptureBackend();
  @override
  final FtpBackend wirelessTransfer = ChannelFtpBackend();
  @override
  final VideoPlatform video = _AndroidVideo();
  @override
  ShareService get share => ShareService.instance;
  @override
  bool get gpuView => true;
}

class _AndroidMedia implements MediaLibrary {
  @override
  final IngestSource source = MediaStoreIngestSource();
  @override
  Future<bool> permissionGranted() async => true; // asked through MediaPermission (app_state), unchanged
  @override
  Future<bool> requestPermission() async => true;
  @override
  Future<String?> copyToFile(IncomingFile f, String destPath) async => f.uri == null ? f.path : MediaBridge.copyToFile(f.uri!, destPath);
  @override
  Future<bool> saveToLibrary(String path, String name, {bool replace = false}) async {
    await MediaBridge.saveToGallery(path, name, replace: replace);
    return true;
  }
}

class _AndroidVideo implements VideoPlatform {
  @override
  bool get available => true;
  @override
  Future<List<VideoItem>> listVideos({int limit = 50}) => LiveBridge.listVideos(limit: limit);
  @override
  Future<Map<String, dynamic>> probe(String uri) => LiveBridge.probe(uri);
  @override
  Future<String> codecReport({bool run = false, bool force = false}) => LiveBridge.codecReport(run: run, force: force);
  @override
  Future<Map<String, dynamic>> capabilities() => LiveBridge.capabilities();
}

// -------------------------------------------------------------------------------------------- stubs (iOS today, host tests)
/// The graceful "this platform has none of it" implementation.
class NullServices implements PlatformServices {
  NullServices(this.name);
  @override
  final String name;
  @override
  final MediaLibrary media = NullMediaLibrary();
  @override
  final CaptureBackend capture = NullCaptureBackend();
  @override
  final FtpBackend wirelessTransfer = NullFtpBackend();
  @override
  final VideoPlatform video = NullVideoPlatform();
  @override
  ShareService get share => ShareService.instance;
  @override
  bool get gpuView => false;
}

class NullMediaLibrary implements MediaLibrary {
  @override
  final IngestSource source = EmptyIngestSource();
  @override
  Future<bool> permissionGranted() async => true;
  @override
  Future<bool> requestPermission() async => true;
  @override
  Future<String?> copyToFile(IncomingFile f, String destPath) async => f.path;
  @override
  Future<bool> saveToLibrary(String path, String name, {bool replace = false}) async => false;
}

class EmptyIngestSource implements IngestSource {
  @override
  String get id => 'none';
  @override
  String get label => '';
  @override
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0}) async => const [];
  @override
  Stream<void> get changes => const Stream.empty();
}

class NullCaptureBackend implements CaptureBackend {
  @override
  bool get available => false;
  @override
  Future<CapturePerm> permission({bool request = false}) async => CapturePerm.denied;
  @override
  Future<bool> openSettings() async => false;
  @override
  Future<CaptureResult?> capture() async => null;
  @override
  Future<void> saveToGallery(String path, String name, String mime) async {}
}

class NullFtpBackend implements FtpBackend {
  @override
  Future<void> start({required int port, required String user, required String pass}) async {}
  @override
  Future<void> stop() async {}
  @override
  Future<FtpStatus> status() async => FtpStatus(error: 'unsupported');
  @override
  Future<List<IncomingFile>> listInbox() async => const [];
  @override
  Stream<FtpEvent> get events => const Stream.empty();
}

class NullVideoPlatform implements VideoPlatform {
  @override
  bool get available => false;
  @override
  Future<List<VideoItem>> listVideos({int limit = 50}) async => const [];
  @override
  Future<Map<String, dynamic>> probe(String uri) async => const {};
  @override
  Future<String> codecReport({bool run = false, bool force = false}) async => '';
  @override
  Future<Map<String, dynamic>> capabilities() async => const {};
}
