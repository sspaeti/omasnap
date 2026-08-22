/** @fileoverview Declares scrolling window capture: paging and stitching. */
#pragma once

#include <QImage>
#include <QRect>
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
  /**
   * Bottom rows containing fixed widgets that ride over the content — a
   * floating avatar, a share bar, an editor's status pill. Wider than
   * `footerRows` (which needs the whole row static): strips are extracted
   * above this band so the widgets appear once instead of once per page,
   * while the offset search still uses the full `footerRows` band.
   */
  int overlayFooterRows = 0;
  /** Mean luma cost of "nothing moved", for OMASNAP_SCROLL_DEBUG logs. */
  double stillCost = 0.0;
  /** Mean luma cost of the best non-zero offset, for the same logs. */
  double bestCost = 0.0;
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
  /**
   * Appends a frame that shares no pixels with the previous one. Some apps
   * page exactly one viewport per Page_Down (Obsidian's reading view), so
   * there is nothing to align; the whole content band is taken as-is and
   * the seam is exact whenever the app's step really is one viewport.
   */
  void appendFullPage(const QImage &frame, const ScrollFrameMatch &match);
  /**
   * Fixes the bottom band cut for the whole capture. Strips only tile
   * seamlessly when every one is cut at the same distance from the bottom;
   * per-frame footer estimates jitter by a few rows and would shift every
   * seam by the difference. Set once, from the first frame pair.
   */
  void lockExtractionFooter(int rows) { extractionFooterLock_ = rows; }
  [[nodiscard]] QImage result() const;
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] bool heightCapped() const { return heightCapped_; }

  static constexpr int kMaxStitchHeight = 32000;

private:
  void appendStrip(const QImage &frame, int offset, int footerRows,
                   int headerRows);
  [[nodiscard]] int effectiveFooter(const ScrollFrameMatch &match) const {
    return extractionFooterLock_ >= 0 ? extractionFooterLock_
                                      : match.footerRows;
  }

  QImage previous_;
  QVector<QImage> strips_;
  int extractionFooterLock_ = -1;
  QImage footer_;
  int height_ = 0;
  int maxHeight_ = kMaxStitchHeight;
  bool heightCapped_ = false;
};

struct MonitorInfo;

/**
 * The sub-rectangle of the window that actually moved between two frames
 * taken one page apart: the scrolling pane. Multi-pane windows (an editor
 * with sidebars, a browser with its chrome) scroll only part of themselves;
 * stitching anything else would repeat the static panes with every strip.
 * Returns an invalid rect when nothing moved, and the whole frame when the
 * moving area is implausibly small to be a scroll pane.
 */
[[nodiscard]] QRect detectScrollRegion(const QImage &first,
                                       const QImage &second);

/**
 * Captures the focused window while paging it down, and stitches the frames
 * into one tall image. Scrolling is driven by sending Page_Down directly to
 * the window through the compositor, so focus never moves; frames come from
 * the same in-process output capture as every other Omasnap screenshot.
 */
[[nodiscard]] bool runScrollCapture(QImage &stitched, QString &error);

/**
 * Scroll-captures one specific window on `monitor`. `address` is the
 * compositor window address and `monitorRect` the window's monitor-relative
 * logical rectangle — both as the capture overlay's window list carries them.
 */
[[nodiscard]] bool runScrollCapture(const MonitorInfo &monitor,
                                    const QString &address,
                                    const QRect &monitorRect,
                                    const QString &title, QImage &stitched,
                                    QString &error);
