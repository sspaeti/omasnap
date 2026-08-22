/** @fileoverview Captures a scrolling window by paging it and stitching. */
#include "scroll-capture.hpp"

#include "capture.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QRgb>
#include <QThread>

#include <algorithm>
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
constexpr int kSettleTimeoutMs = 900;

/** Per-row luma samples over the frame's interior columns. The rightmost
 *  band is skipped so an auto-hiding scrollbar cannot poison the match. */
class FrameSignature {
public:
  explicit FrameSignature(const QImage &frame)
      : rows_(frame.height()),
        image_(frame.convertToFormat(QImage::Format_RGB32)) {
    const int width = image_.width();
    const int left = width * 4 / 100;
    const int right = std::max(left + 1, width * 92 / 100);
    samples_.resize(static_cast<std::size_t>(rows_) * kSignatureColumns);
    for (int y = 0; y < rows_; ++y) {
      const auto *pixels =
          reinterpret_cast<const QRgb *>(image_.constScanLine(y));
      for (int i = 0; i < kSignatureColumns; ++i) {
        const int x =
            left + (right - left) * (2 * i + 1) / (2 * kSignatureColumns);
        const QRgb pixel = pixels[x];
        samples_[static_cast<std::size_t>(y) * kSignatureColumns + i] =
            static_cast<short>((qRed(pixel) * 299 + qGreen(pixel) * 587 +
                                qBlue(pixel) * 114) /
                               1000);
      }
    }
  }

  [[nodiscard]] int rows() const { return rows_; }

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
                         int bottom, double &cost) {
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
    const bool textured = nextHigh - nextLow > kTextureThreshold ||
                          prevHigh - prevLow > kTextureThreshold;
    if (columns >= 16 && textured)
      rows.push_back(static_cast<double>(sum) / columns);
  }
  if (rows.size() < 8)
    return false;
  cost = trimmedMean(rows);
  return true;
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
  double stillCost = 0.0;
  if (!maskedAlignmentCost(before, after, mask, 0, top, contentBottom,
                           stillCost))
    stillCost = alignmentCost(before, after, 0, top, contentBottom);

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
    double cost = 0.0;
    if (!maskedAlignmentCost(before, after, mask, offset, top,
                             contentBottom - offset, cost))
      cost = alignmentCost(before, after, offset, top, contentBottom - offset);
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

ScrollStitcher::ScrollStitcher(QImage firstFrame, int maxHeight)
    : previous_(std::move(firstFrame)), maxHeight_(maxHeight) {
  strips_.append(previous_);
  height_ = previous_.height();
}

ScrollFrameMatch ScrollStitcher::append(const QImage &frame) {
  const ScrollFrameMatch match = matchScrollFrames(previous_, frame);
  if (!match.matched || match.offset == 0)
    return match;

  const int height = frame.height();
  const int width = frame.width();
  const int footerTop = height - match.footerRows;

  // The footer lives outside the strips and is re-taken from every frame, so
  // it appears exactly once, at the bottom of the final image.
  if (footer_.isNull() && match.footerRows > 0) {
    QImage &base = strips_.last();
    base = base.copy(0, 0, width, base.height() - match.footerRows);
    height_ -= match.footerRows;
  }
  height_ -= footer_.height();
  footer_ = match.footerRows > 0
                ? frame.copy(0, footerTop, width, match.footerRows)
                : QImage();

  const int available = maxHeight_ - height_ - footer_.height();
  const int stripRows = std::min(match.offset, std::max(0, available));
  heightCapped_ = heightCapped_ || stripRows < match.offset;
  if (stripRows > 0) {
    strips_.append(frame.copy(0, footerTop - match.offset, width, stripRows));
    height_ += stripRows;
  }
  height_ += footer_.height();
  previous_ = frame;
  return match;
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

bool sendPageDown(const QString &address, QString &error) {
  QByteArray output;
  return runHyprctl(
      {QStringLiteral("dispatch"),
       QStringLiteral("hl.dsp.send_shortcut({ mods = '', key = 'Page_Down', "
                      "window = 'address:%1' })")
           .arg(address)},
      output, error);
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
    if (framesSettled(previous, current)) {
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
  ScrollStitcher stitcher(first);
  debugDump(first, 0, nullptr);

  int frames = 1;
  bool reachedBottom = false;
  while (frames < kMaxScrollFrames && !stitcher.heightCapped()) {
    if (!sendPageDown(address, error))
      return false;
    QImage frame;
    if (!captureSettledFrame(monitor, nativeRect, frame, error))
      return false;
    ScrollFrameMatch match = stitcher.append(frame);
    debugDump(frame, frames, &match);
    if (!match.matched) {
      // One retry after extra settling; animated content can spoil a frame.
      QThread::msleep(250);
      if (!captureSettledFrame(monitor, nativeRect, frame, error))
        return false;
      match = stitcher.append(frame);
      debugDump(frame, frames, &match);
    }
    if (!match.matched)
      break;
    ++frames;
    if (match.offset == 0) {
      reachedBottom = true;
      break;
    }
  }

  stitched = stitcher.result();
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
