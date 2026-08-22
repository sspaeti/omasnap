/** @fileoverview Declares scroll-capture stitching smoke checks. */
#pragma once

#include <QString>

/** Checks frame alignment, sticky bands, and stitched reconstruction. */
[[nodiscard]] bool runScrollStitchSmoke(QString &error);
