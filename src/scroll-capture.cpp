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
/** Mean per-sample luma difference below which an alignment is accepted. */
constexpr double kMatchTolerance = 4.0;
/** Rows two frames must share for the offset search to be trustworthy. */
constexpr int kMinOverlapRows = 48;
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

  [[nodiscard]] double rowDifference(int row, const FrameSignature &other,
                                     int otherRow) const {
    const short *a = &samples_[static_cast<std::size_t>(row) *
                               kSignatureColumns];
    const short *b = &other.samples_[static_cast<std::size_t>(otherRow) *
                                     kSignatureColumns];
    int sum = 0;
    for (int i = 0; i < kSignatureColumns; ++i)
      sum += std::abs(a[i] - b[i]);
    return static_cast<double>(sum) / kSignatureColumns;
  }

private:
  int rows_ = 0;
  QImage image_;
  std::vector<short> samples_;
};

/** Mean row difference of `next` rows [top, bottom) against `previous`
 *  shifted down by `offset`. */
double alignmentCost(const FrameSignature &previous, const FrameSignature &next,
                     int offset, int top, int bottom) {
  double sum = 0.0;
  int count = 0;
  for (int y = top; y < bottom; y += 2) {
    sum += next.rowDifference(y, previous, y + offset);
    ++count;
  }
  return count > 0 ? sum / count : kMatchTolerance + 1.0;
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
  const double stillCost =
      alignmentCost(before, after, 0, top, contentBottom);

  // Every offset is tried: sharp content (text) only dips to a low cost at
  // the exact alignment, so a strided search would step right over it.
  int bestOffset = 0;
  double bestCost = kMatchTolerance + 1.0;
  const int maxOffset = contentBottom - top - kMinOverlapRows;
  for (int offset = 1; offset <= maxOffset; ++offset) {
    const double cost =
        alignmentCost(before, after, offset, top, contentBottom - offset);
    if (cost < bestCost) {
      bestCost = cost;
      bestOffset = offset;
      if (bestCost < 0.5)
        break;
    }
  }

  if (stillCost <= kMatchTolerance && stillCost <= bestCost) {
    match.matched = true;
    match.offset = 0;
  } else if (bestOffset > 0 && bestCost <= kMatchTolerance) {
    match.matched = true;
    match.offset = bestOffset;
  }
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
    if (current == previous) {
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

  const QRect translated =
      window.logicalRect.translated(-monitor.geometry.topLeft());
  const QRect nativeRect =
      QRect(qRound(translated.x() * monitor.scale),
            qRound(translated.y() * monitor.scale),
            qRound(translated.width() * monitor.scale),
            qRound(translated.height() * monitor.scale))
          .intersected(QRect(QPoint(0, 0), monitor.pixelSize));
  if (nativeRect.isEmpty()) {
    error = QStringLiteral("Focused window is outside the focused monitor");
    return false;
  }

  QImage first;
  if (!captureWindowFrame(monitor, nativeRect, first, error))
    return false;
  ScrollStitcher stitcher(first);

  int frames = 1;
  bool reachedBottom = false;
  while (frames < kMaxScrollFrames && !stitcher.heightCapped()) {
    if (!sendPageDown(window.address, error))
      return false;
    QImage frame;
    if (!captureSettledFrame(monitor, nativeRect, frame, error))
      return false;
    ScrollFrameMatch match = stitcher.append(frame);
    if (!match.matched) {
      // One retry after extra settling; animated content can spoil a frame.
      QThread::msleep(250);
      if (!captureSettledFrame(monitor, nativeRect, frame, error))
        return false;
      match = stitcher.append(frame);
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
             .arg(window.title)
             .arg(stitched.width())
             .arg(stitched.height())
             .arg(reachedBottom ? QString()
                                : QStringLiteral(" (stopped before the "
                                                 "bottom)"));
  return true;
}
