/** @fileoverview Tests INI palette loading: full, partial, invalid, missing. */
#include "palette-config-smoke.hpp"

#include "palette-config.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {
bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}
} // namespace

bool runPaletteConfigSmoke(QString &error) {
  QTemporaryDir dir;
  if (!dir.isValid()) {
    error = QStringLiteral("could not create temporary directory");
    return false;
  }
  const PaletteConfig defaults = defaultPaletteConfig();

  // Missing file -> defaults.
  const PaletteConfig missing =
      loadPaletteConfig(dir.filePath(QStringLiteral("absent.conf")));
  if (missing.palette != defaults.palette || missing.custom != defaults.custom) {
    error = QStringLiteral("missing file did not fall back to defaults");
    return false;
  }

  // Full config overrides all six slots and the custom seed.
  const QString full = dir.filePath(QStringLiteral("full.conf"));
  if (!writeFile(full, "[colors]\n"
                       "palette=#FF5D62,#3150AA,#98BB6C,#FFA066,#D27E99,#DCD7BA\n"
                       "custom=#FF5D62\n")) {
    error = QStringLiteral("could not write full config");
    return false;
  }
  const PaletteConfig loaded = loadPaletteConfig(full);
  if (loaded.palette.at(0) != QColor(QStringLiteral("#FF5D62")) ||
      loaded.palette.at(5) != QColor(QStringLiteral("#DCD7BA")) ||
      loaded.custom != QColor(QStringLiteral("#FF5D62"))) {
    error = QStringLiteral("full config not applied");
    return false;
  }

  // Partial palette overrides only the first N; an invalid entry keeps the
  // default for its slot.
  const QString partial = dir.filePath(QStringLiteral("partial.conf"));
  if (!writeFile(partial, "[colors]\npalette=#112233,nonsense\n")) {
    error = QStringLiteral("could not write partial config");
    return false;
  }
  const PaletteConfig sparse = loadPaletteConfig(partial);
  if (sparse.palette.at(0) != QColor(QStringLiteral("#112233")) ||
      sparse.palette.at(1) != defaults.palette.at(1) ||
      sparse.palette.at(2) != defaults.palette.at(2) ||
      sparse.custom != defaults.custom) {
    error = QStringLiteral("partial config handling wrong");
    return false;
  }

  return true;
}
