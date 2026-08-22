/** @fileoverview Captures a scrolling window by paging it and stitching. */
#include "scroll-capture.hpp"

#include "capture.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QRgb>
#include <QThread>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <vector>

namespace {
/** Luma samples per row; enough to anchor alignment, cheap to compare. */
constexpr int kSignatureColumns = 64;
/** Mean per-sample luma difference below which two rows count as equal. */
constexpr double kStaticRowTolerance = 3.0;
/** Mean per-sample luma difference below which an alignment is accepted.
 *  Fractional monitor scales land scroll offsets between sample rows, and
 *  ad-heavy pages keep a floor of churn even at the true offset, so this
 *  sits well above a clean match (<1) but far below a misalignment (>15
 *  once masking and trimming have removed overlays and animation). */
constexpr double kMatchTolerance = 6.0;
constexpr int kMaxScrollFrames = 80;
constexpr int kSettlePollMs = 70;
constexpr int kSettleRelaxMs = 450;
constexpr int kSettleTimeoutMs = 900;

/** Per-row luma samples over the frame's interior columns. The rightmost
 *  band is skipped so an auto-hiding scrollbar cannot poison the match. */
class FrameSignature {
public:
  explicit FrameSignature(const QImage &frame)
      : rows_(frame.height()),
        image_(frame.convertToFormat(QImage::Format_RGB32)) {
    const int width = image_.width();
    left_ = width * 4 / 100;
    right_ = std::max(left_ + 1, width * 92 / 100);
    samples_.resize(static_cast<std::size_t>(rows_) * kSignatureColumns);
    for (int y = 0; y < rows_; ++y) {
      const auto *pixels =
          reinterpret_cast<const QRgb *>(image_.constScanLine(y));
      for (int i = 0; i < kSignatureColumns; ++i) {
        const QRgb pixel = pixels[columnX(i)];
        samples_[static_cast<std::size_t>(y) * kSignatureColumns + i] =
            static_cast<short>((qRed(pixel) * 299 + qGreen(pixel) * 587 +
                                qBlue(pixel) * 114) /
                               1000);
      }
    }
  }

  [[nodiscard]] int rows() const { return rows_; }
  /** Native x of a sample column's center. */
  [[nodiscard]] int columnX(int column) const {
    return left_ +
           (right_ - left_) * (2 * column + 1) / (2 * kSignatureColumns);
  }
  [[nodiscard]] int columnSpacing() const {
    return std::max(1, (right_ - left_) / kSignatureColumns);
  }

  /** Winsorized: each column contributes at most kColumnDiffCap, so one
   *  small fixed widget riding over the content — a floating avatar, a
   *  chat bubble, a scroll-to-top button — cannot drown out a whole row. */
  [[nodiscard]] double rowDifference(int row, const FrameSignature &other,
                                     int otherRow) const {
    const short *a = &samples_[static_cast<std::size_t>(row) *
                               kSignatureColumns];
    const short *b = &other.samples_[static_cast<std::size_t>(otherRow) *
                                     kSignatureColumns];
    int sum = 0;
    for (int i = 0; i < kSignatureColumns; ++i)
      sum += std::min(std::abs(a[i] - b[i]), kColumnDiffCap);
    return static_cast<double>(sum) / kSignatureColumns;
  }

  static constexpr int kColumnDiffCap = 40;

  [[nodiscard]] short sampleAt(int row, int column) const {
    return samples_[static_cast<std::size_t>(row) * kSignatureColumns +
                    static_cast<std::size_t>(column)];
  }

private:
  int rows_ = 0;
  int left_ = 0;
  int right_ = 1;
  QImage image_;
  std::vector<short> samples_;
};

/** Trimmed mean of per-row costs: the worst 30% of rows are dropped, so an
 *  animated ad or video band cannot veto an otherwise exact alignment. */
double trimmedMean(std::vector<double> &rows) {
  if (rows.empty())
    return kMatchTolerance + 1.0;
  const std::size_t kept = rows.size() - rows.size() * 3 / 10;
  std::nth_element(rows.begin(), rows.begin() + static_cast<long>(kept - 1),
                   rows.end());
  double sum = 0.0;
  for (std::size_t i = 0; i < kept; ++i)
    sum += rows[i];
  return sum / static_cast<double>(kept);
}

double alignmentCost(const FrameSignature &previous, const FrameSignature &next,
                     int offset, int top, int bottom) {
  std::vector<double> rows;
  rows.reserve(static_cast<std::size_t>(std::max(0, bottom - top) / 2 + 1));
  for (int y = top; y < bottom; y += 2)
    rows.push_back(next.rowDifference(y, previous, y + offset));
  return trimmedMean(rows);
}

/** Cells that show the same pixels at the same position in both frames.
 *  While the page scrolled, such a cell belongs to something that did not
 *  move with it — a floating avatar, a share bar, plain empty margin. */
std::vector<quint64> staticCellMask(const FrameSignature &previous,
                                    const FrameSignature &next) {
  constexpr int kStaticCellTolerance = 6;
  std::vector<quint64> mask(static_cast<std::size_t>(previous.rows()), 0);
  for (int y = 0; y < previous.rows(); ++y) {
    quint64 bits = 0;
    for (int c = 0; c < kSignatureColumns; ++c) {
      if (std::abs(next.sampleAt(y, c) - previous.sampleAt(y, c)) <=
          kStaticCellTolerance)
        bits |= quint64(1) << c;
    }
    mask[static_cast<std::size_t>(y)] = bits;
  }
  return mask;
}

/**
 * Alignment cost over moving content only. Cells static at the same
 * position in either row of the pair are excluded: a fixed overlay riding
 * over the page can then neither veto the true offset (watson's share bar
 * sits exactly in the one-line overlap a full page-down leaves) nor let a
 * blank region vote for a false one. Returns false when too little moving
 * content remains to judge — the caller falls back to the plain cost.
 */
bool maskedAlignmentCost(const FrameSignature &previous,
                         const FrameSignature &next,
                         const std::vector<quint64> &mask, int offset, int top,
                         int bottom, bool textureGate, double &cost) {
  constexpr int kTextureThreshold = 12;
  std::vector<double> rows;
  rows.reserve(static_cast<std::size_t>(std::max(0, bottom - top) / 2 + 1));
  for (int y = top; y < bottom; y += 2) {
    const quint64 skip = mask[static_cast<std::size_t>(y)] |
                         mask[static_cast<std::size_t>(y + offset)];
    int sum = 0;
    int columns = 0;
    int nextLow = 255, nextHigh = 0, prevLow = 255, prevHigh = 0;
    for (int c = 0; c < kSignatureColumns; ++c) {
      if (skip & (quint64(1) << c))
        continue;
      const int nextSample = next.sampleAt(y, c);
      const int prevSample = previous.sampleAt(y + offset, c);
      sum += std::min(std::abs(nextSample - prevSample),
                      FrameSignature::kColumnDiffCap);
      ++columns;
      nextLow = std::min(nextLow, nextSample);
      nextHigh = std::max(nextHigh, nextSample);
      prevLow = std::min(prevLow, prevSample);
      prevHigh = std::max(prevHigh, prevSample);
    }
    // A row votes only when either side carries content: a blank-on-blank
    // pair costs zero at every offset (interline gaps, empty margins) and
    // enough of them would elect an arbitrary alignment or a false bottom.
    const bool textured = !textureGate ||
                          nextHigh - nextLow > kTextureThreshold ||
                          prevHigh - prevLow > kTextureThreshold;
    if (columns >= 16 && textured)
      rows.push_back(static_cast<double>(sum) / columns);
  }
  if (rows.size() < 8)
    return false;
  cost = trimmedMean(rows);
  return true;
}

/**
 * The full fallback chain for one offset: content-gated masked cost, then
 * masked without the texture gate (paging through a large low-contrast
 * image leaves few textured rows, but the mask must keep protecting
 * against fixed overlays), then the plain trimmed cost as a last resort.
 */
double offsetCost(const FrameSignature &previous, const FrameSignature &next,
                  const std::vector<quint64> &mask, int offset, int top,
                  int bottom) {
  double cost = 0.0;
  if (maskedAlignmentCost(previous, next, mask, offset, top, bottom, true,
                          cost))
    return cost;
  if (maskedAlignmentCost(previous, next, mask, offset, top, bottom, false,
                          cost))
    return cost;
  return alignmentCost(previous, next, offset, top, bottom);
}
} // namespace

ScrollFrameMatch matchScrollFrames(const QImage &previous,
                                   const QImage &next) {
  ScrollFrameMatch match;
  if (previous.size() != next.size() || previous.isNull())
    return match;

  const FrameSignature before(previous);
  const FrameSignature after(next);
  const int height = before.rows();
  const int staticBandCap = height * 3 / 10;

  while (match.headerRows < staticBandCap &&
         after.rowDifference(match.headerRows, before, match.headerRows) <=
             kStaticRowTolerance)
    ++match.headerRows;
  while (match.footerRows < staticBandCap &&
         after.rowDifference(height - 1 - match.footerRows, before,
                             height - 1 - match.footerRows) <=
             kStaticRowTolerance)
    ++match.footerRows;

  const int top = match.headerRows;
  const int contentBottom = height - match.footerRows;
  const std::vector<quint64> mask = staticCellMask(before, after);
  // "Nothing moved" goes through the same gated cost as every offset: on a
  // page that did scroll, the blank row-pairs that remain cheap at offset
  // zero must not be allowed to fake a bottom.
  const double stillCost =
      offsetCost(before, after, mask, 0, top, contentBottom);

  // Every offset is tried: sharp content (text) only dips to a low cost at
  // the exact alignment, so a strided search would step right over it. The
  // overlap floor adapts to the viewport: a browser paged in a quarter-tile
  // window leaves only a dozen shared rows, and a fixed floor of a few dozen
  // rows would place the true offset outside the search range entirely.
  const int minOverlap = std::max(8, (contentBottom - top) / 80);
  const int maxOffset = contentBottom - top - minOverlap;
  std::vector<double> costs;
  costs.reserve(static_cast<std::size_t>(std::max(0, maxOffset)));
  double minCost = kMatchTolerance + 1.0;
  for (int offset = 1; offset <= maxOffset; ++offset) {
    const double cost =
        offsetCost(before, after, mask, offset, top, contentBottom - offset);
    costs.push_back(cost);
    minCost = std::min(minCost, cost);
  }
  // Among near-equal dips, the smallest offset wins: it has the largest
  // overlap behind it, where a large offset may be riding on a few blank
  // rows that would match anywhere.
  int bestOffset = 0;
  double bestCost = kMatchTolerance + 1.0;
  for (std::size_t index = 0; index < costs.size(); ++index) {
    if (costs[index] <= minCost + 0.5) {
      bestOffset = static_cast<int>(index) + 1;
      bestCost = costs[index];
      break;
    }
  }

  match.stillCost = stillCost;
  match.bestCost = bestCost;
  if (stillCost <= kMatchTolerance && stillCost <= bestCost) {
    match.matched = true;
    match.offset = 0;
  } else if (bestOffset > 0 && bestCost <= kMatchTolerance) {
    match.matched = true;
    match.offset = bestOffset;
  }
  if (bestOffset > 0 && !match.matched)
    match.offset = bestOffset; // best candidate, for the debug log only
  return match;
}

QRect detectScrollRegion(const QImage &first, const QImage &second) {
  if (first.size() != second.size() || first.isNull())
    return {};
  const FrameSignature before(first);
  const FrameSignature after(second);
  const int rows = before.rows();
  const int width = first.width();
  const std::vector<quint64> mask = staticCellMask(before, after);

  // A column belongs to the scroll pane when enough of its cells changed.
  std::array<int, kSignatureColumns> movingRows{};
  for (int y = 0; y < rows; ++y) {
    const quint64 moving = ~mask[static_cast<std::size_t>(y)];
    for (int c = 0; c < kSignatureColumns; ++c) {
      if (moving & (quint64(1) << c))
        ++movingRows[static_cast<std::size_t>(c)];
    }
  }

  // The widest contiguous run of moving columns is the pane; an animated
  // widget in a static side panel forms its own, narrower run and loses.
  const int columnThreshold = std::max(8, rows / 50);
  int bestLeft = -1, bestRight = -1, runLeft = -1;
  for (int c = 0; c <= kSignatureColumns; ++c) {
    const bool moving =
        c < kSignatureColumns &&
        movingRows[static_cast<std::size_t>(c)] >= columnThreshold;
    if (moving && runLeft < 0)
      runLeft = c;
    if (!moving && runLeft >= 0) {
      if (bestLeft < 0 || c - 1 - runLeft > bestRight - bestLeft) {
        bestLeft = runLeft;
        bestRight = c - 1;
      }
      runLeft = -1;
    }
  }
  if (bestLeft < 0)
    return {};

  quint64 paneColumns = 0;
  for (int c = bestLeft; c <= bestRight; ++c)
    paneColumns |= quint64(1) << c;
  const int rowThreshold = std::max(2, (bestRight - bestLeft + 1) / 8);
  const auto rowMoves = [&](int y) {
    return std::popcount(~mask[static_cast<std::size_t>(y)] & paneColumns) >=
           rowThreshold;
  };
  // Three consecutive moving rows, so a stray repaint cannot stretch the
  // pane; the same from the bottom.
  int top = -1, bottom = -1;
  for (int y = 0; y + 2 < rows; ++y) {
    if (rowMoves(y) && rowMoves(y + 1) && rowMoves(y + 2)) {
      top = y;
      break;
    }
  }
  for (int y = rows - 1; y >= 2; --y) {
    if (rowMoves(y) && rowMoves(y - 1) && rowMoves(y - 2)) {
      bottom = y;
      break;
    }
  }
  if (top < 0 || bottom < top)
    return {};

  // Blank margins never register as moving — blank scrolling over blank is
  // static — yet they belong to the pane, and cutting them shaves the sides
  // off the capture. Grow the region through featureless static columns; a
  // real side panel carries content and stops the growth.
  const auto blankColumn = [&](int c) {
    if (movingRows[static_cast<std::size_t>(c)] >= columnThreshold)
      return false;
    int low = 255, high = 0;
    for (int y = top; y <= bottom; y += 2) {
      const int sample = after.sampleAt(y, c);
      low = std::min(low, sample);
      high = std::max(high, sample);
    }
    return high - low <= 14;
  };
  while (bestLeft > 0 && blankColumn(bestLeft - 1))
    --bestLeft;
  while (bestRight < kSignatureColumns - 1 && blankColumn(bestRight + 1))
    ++bestRight;

  // Snap to the window edge wherever motion reaches the outermost samples;
  // the sampled band leaves margins that certainly belong to the pane.
  const int spacing = before.columnSpacing();
  const int x0 = bestLeft == 0 ? 0 : before.columnX(bestLeft) - spacing / 2;
  const int x1 = bestRight == kSignatureColumns - 1
                     ? width
                     : before.columnX(bestRight) + spacing / 2 + 1;
  const int y0 = top <= 3 ? 0 : top;
  const int y1 = bottom >= rows - 4 ? rows : bottom + 1;
  const QRect region(x0, y0, x1 - x0, y1 - y0);
  // Too small to be a scroll pane; stitch the whole window instead.
  if (region.width() * region.height() * 4 < width * rows)
    return {0, 0, width, rows};
  return region;
}

ScrollStitcher::ScrollStitcher(QImage firstFrame, int maxHeight)
    : previous_(std::move(firstFrame)), maxHeight_(maxHeight) {
  strips_.append(previous_);
  height_ = previous_.height();
}

ScrollFrameMatch ScrollStitcher::append(const QImage &frame) {
  const ScrollFrameMatch match = matchScrollFrames(previous_, frame);
  if (!match.matched || match.offset == 0)
    return match;
  appendStrip(frame, match.offset, match.footerRows);
  return match;
}

void ScrollStitcher::appendFullPage(const QImage &frame,
                                    const ScrollFrameMatch &match) {
  const int step = frame.height() - match.footerRows - match.headerRows;
  if (step > 0)
    appendStrip(frame, step, match.footerRows);
}

void ScrollStitcher::appendStrip(const QImage &frame, int offset,
                                 int footerRows) {
  const int height = frame.height();
  const int width = frame.width();
  const int footerTop = height - footerRows;

  // The footer lives outside the strips and is re-taken from every frame, so
  // it appears exactly once, at the bottom of the final image.
  if (footer_.isNull() && footerRows > 0) {
    QImage &base = strips_.last();
    base = base.copy(0, 0, width, base.height() - footerRows);
    height_ -= footerRows;
  }
  height_ -= footer_.height();
  footer_ = footerRows > 0 ? frame.copy(0, footerTop, width, footerRows)
                           : QImage();

  const int available = maxHeight_ - height_ - footer_.height();
  const int stripRows = std::min(offset, std::max(0, available));
  heightCapped_ = heightCapped_ || stripRows < offset;
  if (stripRows > 0) {
    strips_.append(frame.copy(0, footerTop - offset, width, stripRows));
    height_ += stripRows;
  }
  height_ += footer_.height();
  previous_ = frame;
}

QImage ScrollStitcher::result() const {
  if (strips_.size() == 1 && footer_.isNull())
    return strips_.first();
  QImage stitched(strips_.first().width(), height_,
                  QImage::Format_ARGB32_Premultiplied);
  stitched.fill(Qt::transparent);
  QPainter painter(&stitched);
  int y = 0;
  for (const QImage &strip : strips_) {
    painter.drawImage(0, y, strip);
    y += strip.height();
  }
  if (!footer_.isNull())
    painter.drawImage(0, y, footer_);
  return stitched;
}

namespace {
struct ActiveWindow {
  QString address;
  QRect logicalRect;
  QString title;
};

bool runHyprctl(const QStringList &arguments, QByteArray &output,
                QString &error) {
  QProcess process;
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(QStringLiteral("hyprctl"), arguments);
  process.closeWriteChannel();
  if (!process.waitForStarted(2000) || !process.waitForFinished(5000) ||
      process.exitCode() != 0) {
    error = QStringLiteral("hyprctl %1 failed: %2")
                .arg(arguments.value(0),
                     QString::fromUtf8(process.readAllStandardError())
                         .trimmed());
    return false;
  }
  output = process.readAllStandardOutput();
  return true;
}

bool queryActiveWindow(ActiveWindow &window, QString &error) {
  QByteArray output;
  if (!runHyprctl({QStringLiteral("activewindow"), QStringLiteral("-j")},
                  output, error))
    return false;
  const QJsonObject object = QJsonDocument::fromJson(output).object();
  const QJsonArray at = object.value(QStringLiteral("at")).toArray();
  const QJsonArray size = object.value(QStringLiteral("size")).toArray();
  window.address = object.value(QStringLiteral("address")).toString();
  window.title = object.value(QStringLiteral("title")).toString();
  window.logicalRect = QRect(at.at(0).toInt(), at.at(1).toInt(),
                             size.at(0).toInt(), size.at(1).toInt());
  if (window.address.isEmpty() || window.logicalRect.isEmpty()) {
    error = QStringLiteral("No focused window to scroll-capture");
    return false;
  }
  return true;
}

bool queryCursorPosition(QPoint &position) {
  QByteArray output;
  QString error;
  if (!runHyprctl({QStringLiteral("cursorpos"), QStringLiteral("-j")}, output,
                  error))
    return false;
  const QJsonObject object = QJsonDocument::fromJson(output).object();
  if (!object.contains(QStringLiteral("x")))
    return false;
  position = QPoint(object.value(QStringLiteral("x")).toInt(),
                    object.value(QStringLiteral("y")).toInt());
  return true;
}

void moveCursor(const QPoint &position) {
  QByteArray output;
  QString error;
  static_cast<void>(runHyprctl(
      {QStringLiteral("dispatch"),
       QStringLiteral("hl.dsp.cursor.move({ x = %1, y = %2 })")
           .arg(position.x())
           .arg(position.y())},
      output, error));
}

/**
 * A monitor corner for the pointer to wait in while frames are captured.
 * Hover styling follows the pointer through the scrolling page — link
 * highlights, the browser's link-target bubble — and repaints a fresh
 * artifact into every frame, so the pointer must not rest on the window.
 * Bottom corners are preferred: the top edge belongs to the bar, whose
 * hover popups could drop down over the window.
 */
QPoint parkedCursorPosition(const MonitorInfo &monitor,
                            const QRect &monitorRect) {
  const QRect bounds(QPoint(0, 0), monitor.geometry.size());
  const QPoint corners[] = {bounds.bottomRight(), bounds.bottomLeft(),
                            bounds.topRight(), bounds.topLeft()};
  for (const QPoint &corner : corners) {
    if (!monitorRect.adjusted(-8, -8, 8, 8).contains(corner))
      return monitor.geometry.topLeft() + corner;
  }
  return monitor.geometry.topLeft() + bounds.bottomRight();
}

/** Parks the pointer for the capture and puts it back afterwards. */
class CursorPark {
public:
  explicit CursorPark(const QPoint &parkAt)
      : hadPosition_(queryCursorPosition(position_)) {
    moveCursor(parkAt);
  }
  ~CursorPark() {
    if (hadPosition_)
      moveCursor(position_);
  }
  CursorPark(const CursorPark &) = delete;
  CursorPark &operator=(const CursorPark &) = delete;

private:
  QPoint position_;
  bool hadPosition_ = false;
};

bool sendKey(const QString &key, const QString &address, QString &error) {
  QByteArray output;
  return runHyprctl(
      {QStringLiteral("dispatch"),
       QStringLiteral("hl.dsp.send_shortcut({ mods = '', key = '%1', "
                      "window = 'address:%2' })")
           .arg(key, address)},
      output, error);
}

bool sendPageDown(const QString &address, QString &error) {
  return sendKey(QStringLiteral("Page_Down"), address, error);
}

bool captureWindowFrame(const MonitorInfo &monitor, const QRect &nativeRect,
                        QImage &frame, QString &error) {
  QImage full;
  if (!captureOutputSurface(monitor, full, error))
    return false;
  frame = full.copy(nativeRect);
  return true;
}

/** Whether two consecutive polls show the same page position. Robust, not
 *  byte-exact: a spinning ad or a video keeps repainting its band forever,
 *  and waiting for pixel-identical frames would always run out the clock. */
bool framesSettled(const QImage &a, const QImage &b) {
  if (a.size() != b.size() || a.isNull())
    return false;
  const FrameSignature first(a);
  const FrameSignature second(b);
  return alignmentCost(first, second, 0, 0, first.rows()) < 1.0;
}

/** Recaptures until two consecutive frames agree, riding out smooth-scroll
 *  animation without a fixed worst-case delay. */
bool captureSettledFrame(const MonitorInfo &monitor, const QRect &nativeRect,
                         QImage &frame, QString &error) {
  QElapsedTimer timer;
  timer.start();
  QImage previous;
  if (!captureWindowFrame(monitor, nativeRect, previous, error))
    return false;
  while (timer.elapsed() < kSettleTimeoutMs) {
    QThread::msleep(kSettlePollMs);
    QImage current;
    if (!captureWindowFrame(monitor, nativeRect, current, error))
      return false;
    // Pixel-identical first: scroll-triggered fade/slide-in effects finish
    // within a few hundred milliseconds and must finish before the frame is
    // stitched, or a heading gets frozen mid-transition. Only when the page
    // never goes quiet (looping ads, videos) does the tolerant comparison
    // take over, so those pages still capture at full speed.
    if (current == previous ||
        (timer.elapsed() > kSettleRelaxMs && framesSettled(previous, current))) {
      frame = current;
      return true;
    }
    previous = current;
  }
  frame = previous;
  return true;
}
} // namespace

bool runScrollCapture(QImage &stitched, QString &error) {
  ActiveWindow window;
  if (!queryActiveWindow(window, error))
    return false;
  MonitorInfo monitor;
  if (!probeFocusedMonitor(monitor, error))
    return false;
  return runScrollCapture(
      monitor, window.address,
      window.logicalRect.translated(-monitor.geometry.topLeft()), window.title,
      stitched, error);
}

bool runScrollCapture(const MonitorInfo &monitor, const QString &address,
                      const QRect &monitorRect, const QString &title,
                      QImage &stitched, QString &error) {
  const QRect nativeRect =
      QRect(qRound(monitorRect.x() * monitor.scale),
            qRound(monitorRect.y() * monitor.scale),
            qRound(monitorRect.width() * monitor.scale),
            qRound(monitorRect.height() * monitor.scale))
          .intersected(QRect(QPoint(0, 0), monitor.pixelSize));
  if (nativeRect.isEmpty()) {
    error = QStringLiteral("The window to scroll is outside the focused "
                           "monitor");
    return false;
  }

  const QString debugDirectory =
      qEnvironmentVariable("OMASNAP_SCROLL_DEBUG");
  if (!debugDirectory.isEmpty())
    QDir().mkpath(debugDirectory);
  const auto debugDump = [&debugDirectory](const QImage &frame, int index,
                                           const ScrollFrameMatch *match) {
    if (debugDirectory.isEmpty())
      return;
    frame.save(QStringLiteral("%1/scroll-frame-%2.png")
                   .arg(debugDirectory)
                   .arg(index, 2, 10, QChar('0')));
    if (match)
      qInfo().noquote()
          << QStringLiteral("scroll debug: frame %1 matched %2 offset %3 "
                            "header %4 footer %5 stillCost %6 bestCost %7")
                 .arg(index)
                 .arg(match->matched)
                 .arg(match->offset)
                 .arg(match->headerRows)
                 .arg(match->footerRows)
                 .arg(match->stillCost, 0, 'f', 2)
                 .arg(match->bestCost, 0, 'f', 2);
  };

  const CursorPark parkedCursor(parkedCursorPosition(monitor, monitorRect));
  // Let hover styling the pointer leaves behind fade before the base frame.
  QThread::msleep(120);

  // The first frame settles too: launched from the capture overlay, the
  // overlay's fade-out is still repainting the screen for a few hundred
  // milliseconds, and a veiled base frame would never match the clean pages.
  QImage first;
  if (!captureSettledFrame(monitor, nativeRect, first, error))
    return false;
  debugDump(first, 0, nullptr);

  int pagesSent = 0;
  const auto restoreScrollPosition = [&address, &pagesSent] {
    // Best effort: one Page_Up per Page_Down puts the reader back where
    // the capture found them, and keeps a re-run from starting at the
    // bottom and finding a one-page "already done" result.
    QString ignored;
    for (int page = 0; page < pagesSent; ++page) {
      if (!sendKey(QStringLiteral("Page_Up"), address, ignored))
        return;
      QThread::msleep(40);
    }
  };

  if (!sendPageDown(address, error))
    return false;
  ++pagesSent;
  QImage second;
  if (!captureSettledFrame(monitor, nativeRect, second, error))
    return false;

  // The scroll pane is whatever moved between the first two frames; side
  // panels, window chrome, and sticky bars stay out of the stitch and are
  // reattached once, from the first and last frame, around the result.
  const QRect region = detectScrollRegion(first, second);
  if (!region.isValid()) {
    stitched = first;
    restoreScrollPosition();
    qInfo().noquote()
        << QStringLiteral("Scroll capture: \"%1\" does not scroll (or was "
                          "already at its end); kept the single %2x%3 frame")
               .arg(title)
               .arg(stitched.width())
               .arg(stitched.height());
    return true;
  }
  if (!debugDirectory.isEmpty())
    qInfo().noquote() << QStringLiteral("scroll debug: region %1,%2 %3x%4 in "
                                        "a %5x%6 window")
                             .arg(region.x())
                             .arg(region.y())
                             .arg(region.width())
                             .arg(region.height())
                             .arg(first.width())
                             .arg(first.height());

  const QImage topBand =
      region.y() > 0
          ? first.copy(region.x(), 0, region.width(), region.y())
          : QImage();
  const int bandBudget =
      ScrollStitcher::kMaxStitchHeight - topBand.height() -
      (first.height() - region.y() - region.height());
  ScrollStitcher stitcher(first.copy(region), bandBudget);

  int frames = 1;
  bool reachedBottom = false;
  QImage lastFrame = first;
  QImage frame = second;
  while (true) {
    ScrollFrameMatch match = stitcher.append(frame.copy(region));
    debugDump(frame, frames, &match);
    if (!match.matched) {
      // One retry after extra settling; animated content can spoil a frame.
      QThread::msleep(250);
      if (!captureSettledFrame(monitor, nativeRect, frame, error))
        return false;
      match = stitcher.append(frame.copy(region));
      debugDump(frame, frames, &match);
    }
    if (!match.matched && match.stillCost > kMatchTolerance) {
      // The pane clearly moved yet shares no pixels with the previous view:
      // the app pages exactly one viewport per Page_Down (Obsidian's
      // reading view), so there is nothing to align. Take the full page;
      // the seam is exact whenever the step really is one viewport.
      stitcher.appendFullPage(frame.copy(region), match);
      if (!debugDirectory.isEmpty())
        qInfo().noquote() << QStringLiteral(
            "scroll debug: no overlap found; assumed an exact one-viewport "
            "step");
      match.matched = true;
      match.offset = region.height() - match.headerRows - match.footerRows;
    }
    if (!match.matched)
      break;
    lastFrame = frame;
    ++frames;
    if (match.offset == 0) {
      reachedBottom = true;
      break;
    }
    if (frames >= kMaxScrollFrames || stitcher.heightCapped())
      break;
    if (!sendPageDown(address, error))
      return false;
    ++pagesSent;
    if (!captureSettledFrame(monitor, nativeRect, frame, error))
      return false;
  }
  restoreScrollPosition();

  const QImage core = stitcher.result();
  const int bottomBandTop = region.y() + region.height();
  const QImage bottomBand =
      bottomBandTop < lastFrame.height()
          ? lastFrame.copy(region.x(), bottomBandTop, region.width(),
                           lastFrame.height() - bottomBandTop)
          : QImage();
  if (topBand.isNull() && bottomBand.isNull()) {
    stitched = core;
  } else {
    stitched = QImage(region.width(),
                      topBand.height() + core.height() + bottomBand.height(),
                      QImage::Format_ARGB32_Premultiplied);
    stitched.fill(Qt::transparent);
    QPainter painter(&stitched);
    painter.drawImage(0, 0, topBand);
    painter.drawImage(0, topBand.height(), core);
    painter.drawImage(0, topBand.height() + core.height(), bottomBand);
  }

  qInfo().noquote()
      << QStringLiteral("Scroll capture stitched %1 frames of \"%2\" into "
                        "%3x%4%5")
             .arg(frames)
             .arg(title)
             .arg(stitched.width())
             .arg(stitched.height())
             .arg(reachedBottom ? QString()
                                : QStringLiteral(" (stopped before the "
                                                 "bottom)"));
  return true;
}
