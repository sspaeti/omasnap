/** @fileoverview Loads editor preset colors from the user's INI config. */
#pragma once

#include <QColor>
#include <QString>
#include <array>

/** Editor color presets, loaded from the user's INI config or defaults. */
struct PaletteConfig {
  std::array<QColor, 6> palette;
  QColor custom;
};

/** Built-in defaults (the colors previously hardcoded in editor.cpp). */
[[nodiscard]] PaletteConfig defaultPaletteConfig();

/** Reads [colors] palette (comma-separated hex list, up to 6 entries,
 *  invalid entries keep the default for that slot) and [colors] custom from
 *  an INI file. A missing file or key leaves defaults untouched. */
[[nodiscard]] PaletteConfig loadPaletteConfig(const QString &filePath);

/** ~/.config/omasnap/omasnap.conf (XDG config location). */
[[nodiscard]] QString defaultPaletteConfigPath();
