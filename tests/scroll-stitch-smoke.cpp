/** @fileoverview Tests scroll-capture frame matching and stitching. */
#include "scroll-stitch-smoke.hpp"

#include "scroll-capture.hpp"

#include <QImage>
#include <QColor>
#include <QPainter>
#include <QRgb>

namespace {
constexpr int kPageWidth = 640;
constexpr int kPageHeight = 4000;
constexpr int kViewportHeight = 700;

/** Deterministic per-pixel noise, so every page row is unique. */
QRgb noisePixel(int x, int y, quint32 salt) {
  quint32 value = static_cast<quint32>(x) * 2654435761U ^
                  static_cast<quint32>(y) * 2246822519U ^ salt;
  value ^= value >> 13;
  value *= 3266489917U;
  value ^= value >> 16;
  return qRgb(static_cast<int>(value & 0xFF),
              static_cast<int>((value >> 8) & 0xFF),
              static_cast<int>((value >> 16) & 0xFF));
}

QImage noiseImage(int width, int height, quint32 salt) {
  QImage image(width, height, QImage::Format_RGB32);
  for (int y = 0; y < height; ++y) {
    auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < width; ++x)
      pixels[x] = noisePixel(x, y, salt);
  }
  return image;
}

/** A viewport frame of `page` at `scroll`, with optional sticky bands. */
QImage viewportFrame(const QImage &page, int scroll, const QImage &header,
                     const QImage &footer) {
  QImage frame = page.copy(0, scroll, page.width(), kViewportHeight);
  QPainter painter(&frame);
  if (!header.isNull())
    painter.drawImage(0, 0, header);
  if (!footer.isNull())
    painter.drawImage(0, kViewportHeight - footer.height(), footer);
  return frame;
}

bool imagesEqual(const QImage &a, const QImage &b) {
  return a.convertToFormat(QImage::Format_RGB32) ==
         b.convertToFormat(QImage::Format_RGB32);
}
} // namespace

bool runScrollStitchSmoke(QString &error) {
  const QImage page = noiseImage(kPageWidth, kPageHeight, 7);

  {
    // A multi-pane window: static chrome on top, a static sidebar on the
    // right, and a scrolling pane. The detected region must be that pane —
    // stitching anything else repeats the static parts with every strip.
    const int width = 800, height = 600, chrome = 60, paneWidth = 600;
    const QImage chromeBand = noiseImage(width, chrome, 500);
    const QImage sidebar = noiseImage(width - paneWidth, height - chrome, 501);
    const auto paneFrame = [&](int scroll) {
      QImage frame(width, height, QImage::Format_RGB32);
      QPainter painter(&frame);
      painter.drawImage(0, 0, chromeBand);
      painter.drawImage(paneWidth, chrome, sidebar);
      painter.drawImage(0, chrome,
                        page.copy(0, scroll, paneWidth, height - chrome));
      return frame;
    };
    const QRect region = detectScrollRegion(paneFrame(0), paneFrame(400));
    if (!region.isValid() || region.x() != 0 ||
        std::abs(region.y() - chrome) > 6 ||
        std::abs(region.right() + 1 - paneWidth) > 20 ||
        region.bottom() + 1 != height) {
      error = QStringLiteral("Pane region detection returned %1,%2 %3x%4")
                  .arg(region.x())
                  .arg(region.y())
                  .arg(region.width())
                  .arg(region.height());
      return false;
    }

    // A centered content column: the blank page margins around it never
    // register as moving, but they belong to the pane and must stay in the
    // region — up to the window edge on the blank side, and up to (not
    // into) the textured sidebar on the other.
    const auto marginFrame = [&](int scroll) {
      QImage frame(width, height, QImage::Format_RGB32);
      frame.fill(QColor(24, 26, 32));
      QPainter painter(&frame);
      painter.drawImage(0, 0, chromeBand);
      painter.drawImage(620, chrome, sidebar.copy(0, 0, 180, height - chrome));
      painter.drawImage(120, chrome,
                        page.copy(0, scroll, 380, height - chrome));
      return frame;
    };
    const QRect margins = detectScrollRegion(marginFrame(0), marginFrame(400));
    if (!margins.isValid() || margins.x() != 0 || margins.right() + 1 > 620 ||
        margins.right() + 1 < 500) {
      error = QStringLiteral("Blank page margins were cut from the region: "
                             "%1,%2 %3x%4")
                  .arg(margins.x())
                  .arg(margins.y())
                  .arg(margins.width())
                  .arg(margins.height());
      return false;
    }

    // Whole-frame scroll must claim the whole window.
    const QImage tall = noiseImage(width, 1200, 502);
    const QRect full = detectScrollRegion(tall.copy(0, 0, width, height),
                                          tall.copy(0, 300, width, height));
    if (full != QRect(0, 0, width, height)) {
      error = QStringLiteral("Full-frame scroll must detect the full window");
      return false;
    }

    // No movement at all: no region.
    const QImage still = paneFrame(0);
    if (detectScrollRegion(still, still).isValid()) {
      error = QStringLiteral("Identical frames must yield no scroll region");
      return false;
    }
  }

  {
    // Plain page: stitching overlapping viewports must reproduce the page.
    ScrollStitcher stitcher(viewportFrame(page, 0, {}, {}));
    int scroll = 0;
    while (scroll + 600 + kViewportHeight <= kPageHeight) {
      scroll += 600;
      const ScrollFrameMatch match =
          stitcher.append(viewportFrame(page, scroll, {}, {}));
      if (!match.matched || match.offset != 600) {
        error = QStringLiteral("Plain scroll match failed at %1: matched %2 "
                               "offset %3")
                    .arg(scroll)
                    .arg(match.matched)
                    .arg(match.offset);
        return false;
      }
    }
    const QImage expected =
        page.copy(0, 0, kPageWidth, scroll + kViewportHeight);
    if (!imagesEqual(stitcher.result(), expected)) {
      error = QStringLiteral("Plain stitch does not reproduce the page");
      return false;
    }
  }

  {
    // Sticky header and footer must appear exactly once, top and bottom.
    const QImage header = noiseImage(kPageWidth, 60, 100);
    const QImage footer = noiseImage(kPageWidth, 40, 200);
    ScrollStitcher stitcher(viewportFrame(page, 0, header, footer));
    int scroll = 0;
    while (scroll + 500 + kViewportHeight <= kPageHeight) {
      scroll += 500;
      const ScrollFrameMatch match =
          stitcher.append(viewportFrame(page, scroll, header, footer));
      if (!match.matched || match.offset != 500 || match.headerRows != 60 ||
          match.footerRows != 40) {
        error = QStringLiteral("Sticky-band match failed at %1: matched %2 "
                               "offset %3 header %4 footer %5")
                    .arg(scroll)
                    .arg(match.matched)
                    .arg(match.offset)
                    .arg(match.headerRows)
                    .arg(match.footerRows);
        return false;
      }
    }
    QImage expected(kPageWidth, scroll + kViewportHeight,
                    QImage::Format_RGB32);
    QPainter painter(&expected);
    painter.drawImage(0, 0, page.copy(0, 0, kPageWidth,
                                      scroll + kViewportHeight - 40));
    painter.drawImage(0, 0, header);
    painter.drawImage(0, scroll + kViewportHeight - 40, footer);
    painter.end();
    if (!imagesEqual(stitcher.result(), expected)) {
      error = QStringLiteral("Sticky-band stitch composed the wrong image");
      return false;
    }
  }

  {
    // An unmoved frame reports the bottom: matched with offset zero.
    const QImage frame = viewportFrame(page, 800, {}, {});
    ScrollStitcher stitcher(frame);
    const ScrollFrameMatch match = stitcher.append(frame);
    if (!match.matched || match.offset != 0) {
      error = QStringLiteral("Identical frames must match at offset zero");
      return false;
    }
    if (!imagesEqual(stitcher.result(), frame)) {
      error = QStringLiteral("A single-viewport page must stay untouched");
      return false;
    }
  }

  {
    // Unrelated frames must be rejected instead of stitched.
    ScrollStitcher stitcher(noiseImage(kPageWidth, kViewportHeight, 1));
    const ScrollFrameMatch match =
        stitcher.append(noiseImage(kPageWidth, kViewportHeight, 2));
    if (match.matched) {
      error = QStringLiteral("Unrelated frames must not match");
      return false;
    }
  }

  {
    // The height cap must stop growth without corrupting the result.
    ScrollStitcher stitcher(viewportFrame(page, 0, {}, {}),
                            kViewportHeight + 200);
    static_cast<void>(stitcher.append(viewportFrame(page, 600, {}, {})));
    if (!stitcher.heightCapped() ||
        stitcher.height() != kViewportHeight + 200) {
      error = QStringLiteral("Height cap was not honored: capped %1 height %2")
                  .arg(stitcher.heightCapped())
                  .arg(stitcher.height());
      return false;
    }
    const QImage expected =
        page.copy(0, 0, kPageWidth, kViewportHeight + 200);
    if (!imagesEqual(stitcher.result(), expected)) {
      error = QStringLiteral("Capped stitch does not match the page prefix");
      return false;
    }
  }

  return true;
}
