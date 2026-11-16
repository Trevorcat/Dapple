import 'dart:io';
import 'dart:isolate';

import 'package:path/path.dart' as p;

/// Why a source file could not be opened, as far as the editor and the library care.
enum SourceFailure {
  /// The file does not exist any more.
  gone,

  /// It exists (or may exist) but could not be read or decoded.
  unreadable,
}

/// Sorts an error from opening a photo's source into [SourceFailure.gone] (the file is not there) or [SourceFailure.unreadable]
/// (anything else: permission, decode, engine). Plain string matching: the errors come from Dart IO, the platform channel and the engine.
SourceFailure classifySourceError(Object e) {
  final s = e.toString().toLowerCase();
  const gone = ['找不到', 'notfound', 'not found', 'no such file', 'enoent', 'does not exist', 'filenotfound'];
  return gone.any(s.contains) ? SourceFailure.gone : SourceFailure.unreadable;
}

/// One library entry whose source is to be looked up: the places its file may be.
typedef SourceProbe = ({String id, List<String> paths});

/// Ids of the entries whose source file is certainly gone: every candidate path answers "no such file" (errno 2) AND the folder
/// it should be in exists and can be listed. Anything else (permission denied, folder missing because a card is out, any other
/// error) counts as "cannot tell" and the entry stays. Runs off the UI thread, one batch.
Future<Set<String>> missingSourceIds(List<SourceProbe> probes) {
  if (probes.isEmpty) return Future.value(<String>{});
  return Isolate.run(() => missingSourceIdsSync(probes));
}

/// The synchronous body of [missingSourceIds] (tests call it directly).
Set<String> missingSourceIdsSync(List<SourceProbe> probes) {
  final dead = <String>{};
  for (final pr in probes) {
    var allGone = pr.paths.isNotEmpty;
    for (final path in pr.paths) {
      if (!_certainlyAbsent(path)) {
        allGone = false;
        break;
      }
    }
    if (allGone) dead.add(pr.id);
  }
  return dead;
}

bool _certainlyAbsent(String path) {
  try {
    final r = File(path).openSync();
    r.closeSync();
    return false; // it opens: present
  } on FileSystemException catch (e) {
    final code = e.osError?.errorCode;
    if (code != 2 && e is! PathNotFoundException) return false; // permission / I/O / anything but "does not exist"
  } catch (_) {
    return false;
  }
  // The file is missing: only believe it when the place it lived on is really there (an unmounted card would answer the same for every
  // file on it). A folder that was removed on a mounted volume counts as gone with its files.
  try {
    return FileStat.statSync(volumeRootOf(path)).type == FileSystemEntityType.directory;
  } catch (_) {
    return false;
  }
}

/// The folder that must exist for "no such file" to be believed: the primary shared storage (`/sdcard`, `/storage/emulated/N`), the
/// mount of a removable volume (`/storage/XXXX-XXXX`), otherwise the file's own folder.
String volumeRootOf(String path) {
  final parts = path.split('/');
  if (parts.length > 2 && parts[0].isEmpty) {
    if (parts[1] == 'sdcard') return '/sdcard';
    if (parts[1] == 'storage' && parts.length > 3 && parts[2] == 'emulated') return '/storage/emulated/${parts[3]}';
    if (parts[1] == 'storage' && parts.length > 3) return '/storage/${parts[2]}';
  }
  return p.dirname(path);
}
