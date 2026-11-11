import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;

import 'ingest.dart';

const MethodChannel _ftp = MethodChannel('dapple/ftp');
const EventChannel _ftpEvents = EventChannel('dapple/ftp_events');

/// A usable local address of the phone (Wi-Fi client or hotspot interface).
class FtpAddress {
  FtpAddress(this.iface, this.ip);
  final String iface;
  final String ip;

  /// Interfaces that are the phone's own hotspot.
  bool get isHotspot => iface.startsWith('ap') || iface.startsWith('swlan') || iface.startsWith('softap');
}

class FtpStatus {
  FtpStatus({this.running = false, this.clients = 0, this.port = 0, this.error, this.root, this.addresses = const []});
  final bool running;
  final int clients;
  final int port;
  final String? error;
  final String? root;
  final List<FtpAddress> addresses;
}

class FtpEvent {
  FtpEvent(this.type, {this.name, this.path, this.bytes, this.size, this.count, this.running, this.error});
  final String type; // clients | start | progress | done | failed | state
  final String? name;
  final String? path;
  final int? bytes;
  final int? size;
  final int? count;
  final bool? running;
  final String? error;

  factory FtpEvent.fromMap(Map m) => FtpEvent(
        m['type'] as String,
        name: m['name'] as String?,
        path: m['path'] as String?,
        bytes: (m['bytes'] as num?)?.toInt(),
        size: (m['size'] as num?)?.toInt(),
        count: (m['count'] as num?)?.toInt(),
        running: m['running'] as bool?,
        error: m['error'] as String?,
      );
}

/// Kotlin FtpService access.
abstract class FtpBackend {
  Future<void> start({required int port, required String user, required String pass});
  Future<void> stop();
  Future<FtpStatus> status();
  Future<List<IncomingFile>> listInbox();
  Stream<FtpEvent> get events;
}

class ChannelFtpBackend implements FtpBackend {
  @override
  Future<void> start({required int port, required String user, required String pass}) async {
    await _ftp.invokeMethod<bool>('start', {'port': port, 'user': user, 'pass': pass});
  }

  @override
  Future<void> stop() async {
    await _ftp.invokeMethod<bool>('stop');
  }

  @override
  Future<FtpStatus> status() async {
    final m = await _ftp.invokeMapMethod<String, dynamic>('status');
    if (m == null) return FtpStatus();
    return FtpStatus(
      running: m['running'] == true,
      clients: (m['clients'] as num?)?.toInt() ?? 0,
      port: (m['port'] as num?)?.toInt() ?? 0,
      error: m['error'] as String?,
      root: m['root'] as String?,
      addresses: [
        for (final a in (m['addresses'] as List? ?? const []))
          FtpAddress((a as Map)['iface'] as String, a['ip'] as String)
      ],
    );
  }

  @override
  Future<List<IncomingFile>> listInbox() async {
    final rows = await _ftp.invokeListMethod<Map>('listInbox');
    return [
      for (final r in rows ?? const <Map>[])
        IncomingFile(
          id: 'ftp:${r['name']}',
          name: r['name'] as String,
          addedMs: (r['modifiedMs'] as num).toInt(),
          path: r['path'] as String,
          size: (r['size'] as num).toInt(),
        )
    ];
  }

  @override
  Stream<FtpEvent> get events => _ftpEvents.receiveBroadcastStream().map((e) => FtpEvent.fromMap(e as Map));
}

/// Camera uploads over FTP: the foreground service writes finished files (never partial ones) into its inbox;
/// this source lists them and signals every completed upload.
class FtpIngestSource implements IngestSource {
  FtpIngestSource(this.backend);
  final FtpBackend backend;

  @override
  String get id => 'ftp';
  @override
  String get label => '相机无线接收';

  @override
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0}) async {
    final all = await backend.listInbox();
    return all.where((f) => f.addedMs ~/ 1000 >= sinceSec).take(limit).toList();
  }

  @override
  Stream<void> get changes => backend.events.where((e) => e.type == 'done').map((_) {});
}

/// Where a RAW and a JPEG of the same exposure meet: same name without extension (DSC01234.ARW / DSC01234.JPG).
String pairStem(String name) => p.basenameWithoutExtension(name).toLowerCase();

const _rawExt = {'.arw', '.raf', '.dng', '.cr2', '.cr3', '.nef', '.orf', '.rw2'};
const _jpgExt = {'.jpg', '.jpeg'};
bool isRawName(String name) => _rawExt.contains(p.extension(name).toLowerCase());
bool isJpegName(String name) => _jpgExt.contains(p.extension(name).toLowerCase());

/// Camera credentials: a short password without look-alike characters.
String generateFtpPassword([Random? rng]) {
  const alphabet = 'abcdefghjkmnpqrstuvwxyzACDEFGHJKLMNPQRTUVWXY346789';
  final r = rng ?? Random.secure();
  return String.fromCharCodes(List.generate(8, (_) => alphabet.codeUnitAt(r.nextInt(alphabet.length))));
}

/// Best address to show first: a hotspot interface when the phone is the access point, else Wi-Fi.
FtpAddress? preferredAddress(List<FtpAddress> l) {
  if (l.isEmpty) return null;
  for (final a in l) {
    if (a.isHotspot) return a;
  }
  for (final a in l) {
    if (a.iface.startsWith('wlan')) return a;
  }
  return l.first;
}

bool fileReadable(String path) {
  try {
    return File(path).existsSync();
  } catch (_) {
    return false;
  }
}
