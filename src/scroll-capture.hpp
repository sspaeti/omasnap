/** @fileoverview Declares scrolling window capture: paging and stitching. */
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

/** Vertical alignment of two viewport frames of the same scrolling window. */
struct ScrollFrameMatch {
  bool matched = false;
  /** Rows the content moved up between the frames; 0 means no movement. */
  int offset = 0;
  /** Static band pinned to the top of the viewport (sticky header). */
  int headerRows = 0;
  /** Static band pinned to the bottom of the viewport (sticky footer). */
  int footerRows = 0;
};

/**
 * Aligns `next` against `previous` after a downward scroll. Static top and
 * bottom bands (sticky headers, footers, toolbars) are detected first and
 * excluded from the alignment, so they never break the offset search.
 */
[[nodiscard]] ScrollFrameMatch matchScrollFrames(const QImage &previous,
                                                 const QImage &next);

/**
 * Accumulates viewport frames of a scrolling window into one tall image.
 * Sticky headers stay only in the first frame, sticky footers only in the
 * last; every appended strip is the freshly revealed content in between.
 */
class ScrollStitcher {
public:
  explicit ScrollStitcher(QImage firstFrame, int maxHeight = kMaxStitchHeight);

  /** Alignment and append in one step; inspect `matched`/`offset`. */
  ScrollFrameMatch append(const QImage &frame);
  [[nodiscard]] QImage result() const;
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] bool heightCapped() const { return heightCapped_; }

  static constexpr int kMaxStitchHeight = 32000;

private:
  QImage previous_;
  QVector<QImage> strips_;
  QImage footer_;
  int height_ = 0;
  int maxHeight_ = kMaxStitchHeight;
  bool heightCapped_ = false;
};

/**
 * Captures the focused window while paging it down, and stitches the frames
 * into one tall image. Scrolling is driven by sending Page_Down directly to
 * the window through the compositor, so focus never moves; frames come from
 * the same in-process output capture as every other Omasnap screenshot.
 */
[[nodiscard]] bool runScrollCapture(QImage &stitched, QString &error);
