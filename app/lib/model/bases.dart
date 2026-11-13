// Rendering bases (docs/engine/pipeline-architecture.md section 37): a photo's base is one film stock or one of three modern
// digital renderings, both carried by the one `film` key (strength `film_strength`); none = the standard rendering. The ids of the
// modern renderings are engine ids and never film stocks.
import 'film_catalog.dart';

class ModernBase {
  const ModernBase(this.id, this.name, this.blurb);
  final String id, name, blurb;
}

const kModernBases = [
  ModernBase('modern_clean', '通透', '中性干净'),
  ModernBase('modern_vivid', '鲜明', '浓郁有力'),
  ModernBase('modern_soft', '柔和', '轻盈低反差'),
];

bool isModernId(String? id) => id != null && id.startsWith('modern_') && kModernBases.any((m) => m.id == id);

ModernBase? modernById(String? id) {
  if (id == null) return null;
  for (final m in kModernBases) {
    if (m.id == id) return m;
  }
  return null;
}

/// Display name of a base id (film stock, modern rendering, or null = 标准).
String baseNameOf(FilmCatalog film, String? id) => film.stock(id)?.name ?? modernById(id)?.name ?? '标准';

/// Every base the user can pick, in a fixed order: the standard rendering (null), then film, then the modern renderings.
List<String?> allBaseIds(FilmCatalog film) => [null, for (final s in film.stocks) s.id, for (final m in kModernBases) m.id];
