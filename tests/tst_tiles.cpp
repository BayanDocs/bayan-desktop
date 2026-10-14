// Unit tests for the canvas's tile arithmetic and tile cache (src/canvas/tilegrid.h, viewgeometry.h, tilecache.h): where tiles and pages
// go at every zoom and device pixel ratio, and when a tile is stale.

#include "tilecache.h"
#include "tilegrid.h"
#include "viewgeometry.h"

#include <QtGui/QImage>
#include <QtTest/QTest>

#include <cmath>
#include <limits>

using namespace bayan;

namespace {

// A US Letter page: 8.5 × 11 inches, 816 × 1056 device-independent pixels.
constexpr protocol::Page kLetter{.index = 0, .width = 15'544'800, .height = 20'116'800, .section = 0, .x = 0, .y = 0};

// Three US Letter pages one below the other with a gap of 1/4 inch, as the real engine's mock document lays them out.
std::vector<protocol::Page> threePages() {
  std::vector<protocol::Page> pages;
  for (int index = 0; index < 3; ++index) {
    protocol::Page page = kLetter;
    page.index = index;
    page.y = qint64{index} * (kLetter.height + 457'200);
    pages.push_back(page);
  }
  return pages;
}

QImage image(QSize size) {
  QImage result(size, QImage::Format_RGBA8888_Premultiplied);
  result.fill(Qt::white);
  return result;
}

bool isWholeDevicePixel(double logical, double ratio) {
  const double device = logical * ratio;
  return std::abs(device - std::round(device)) < 1e-6;
}

} // namespace

class TestTiles final : public QObject {
  Q_OBJECT

private slots:
  void pagesCoverWholeDevicePixels_data();
  void pagesCoverWholeDevicePixels();
  void neighbouringTilesShareTheirEdges_data();
  void neighbouringTilesShareTheirEdges();
  void findsTheTilesOfAnArea();
  void refusesNonsenseSettings();
  void centresAPageAreaNarrowerThanTheView();
  void scrollsAPageAreaWiderThanTheView();
  void placesPagesOnWholeDevicePixels_data();
  void placesPagesOnWholeDevicePixels();
  void limitsTheScrollPosition();
  void convertsPointsForTheEngine();
  void keepsThePointUnderThePointerWhileZooming();
  void stretchesTilesOfAnotherZoom();
  void findsTheVisiblePartOfAPage();
  void tilesRenderedBeforeAChangeAreDirty();
  void changesElsewhereLeaveTilesFresh();
  void aTileRenderedWhileTheDocumentChangedIsDirty();
  void stopsAskingForRefusedTiles();
  void forgetsOldInvalidationsSafely();
  void resetForgetsTheChangesOfTheOldEngine();
  void remembersManyChangedRectanglesAsOne();
  void standInsComeFromTheClosestZoomThatHasThem();
  void evictsTheLeastRecentlyUsedTilesOverBudget();
  void forgetsTilesThatNeverArrived();
};

void TestTiles::pagesCoverWholeDevicePixels_data() {
  QTest::addColumn<double>("scale");
  QTest::addColumn<qint64>("width");
  QTest::addColumn<qint64>("height");
  QTest::newRow("100 %") << 1.0 << qint64{816} << qint64{1056};
  QTest::newRow("125 %") << 1.25 << qint64{1020} << qint64{1320};
  QTest::newRow("150 %") << 1.5 << qint64{1224} << qint64{1584};
  QTest::newRow("Retina") << 2.0 << qint64{1632} << qint64{2112};
  // 816 × 1.1 = 897.6000000000001 in floating point: one pixel more than that, never two.
  QTest::newRow("110 %") << 1.1 << qint64{898} << qint64{1162};
}

void TestTiles::pagesCoverWholeDevicePixels() {
  QFETCH(double, scale);
  QFETCH(qint64, width);
  QFETCH(qint64, height);
  const TileGrid grid(scale, 512);
  QCOMPARE(grid.pageWidthPixels(kLetter), width);
  QCOMPARE(grid.pageHeightPixels(kLetter), height);
  QCOMPARE(grid.columns(kLetter), static_cast<int>((width + 511) / 512));
  QCOMPARE(grid.rows(kLetter), static_cast<int>((height + 511) / 512));
  // The last tile in each direction is smaller and ends at the page's last pixel.
  const DeviceRect last = grid.tilePixels(kLetter, grid.columns(kLetter) - 1, grid.rows(kLetter) - 1);
  QCOMPARE(last.right(), width);
  QCOMPARE(last.bottom(), height);
  QVERIFY(last.width > 0 && last.width <= 512 && last.height > 0 && last.height <= 512);
  // The page's last pixel reaches the page's edge.
  QVERIFY(grid.bluAt(width) >= kLetter.width);
}

void TestTiles::neighbouringTilesShareTheirEdges_data() {
  QTest::addColumn<double>("scale");
  for (const double scale : {0.1, 0.33, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 10.0}) {
    QTest::newRow(QByteArray::number(scale).constData()) << scale;
  }
}

void TestTiles::neighbouringTilesShareTheirEdges() {
  QFETCH(double, scale);
  const TileGrid grid(scale, 256);
  // No seam: each tile starts at exactly the BLU coordinate where its left and upper neighbours end, so nothing is drawn twice or missed.
  for (int row = 0; row < grid.rows(kLetter); ++row) {
    for (int column = 0; column < grid.columns(kLetter); ++column) {
      const protocol::BluRect tile = grid.tileRect(kLetter, column, row);
      QVERIFY(tile.width >= 1 && tile.height >= 1);
      if (column > 0) {
        QCOMPARE(tile.x, grid.tileRect(kLetter, column - 1, row).right());
      }
      if (row > 0) {
        QCOMPARE(tile.y, grid.tileRect(kLetter, column, row - 1).bottom());
      }
    }
  }
  QCOMPARE(grid.tileRect(kLetter, 0, 0).x, 0);
}

void TestTiles::findsTheTilesOfAnArea() {
  const TileGrid grid(1.0, 512);
  // An area across the first two columns and rows, reaching beyond the page on the left.
  const auto tiles = grid.tilesIntersecting(kLetter, {.x = -100, .y = 500, .width = 700, .height = 100});
  QCOMPARE(tiles.size(), std::size_t{4});
  QCOMPARE(tiles.at(0), (TileKey{.page = 0, .scale = 1.0, .column = 0, .row = 0}));
  QCOMPARE(tiles.at(1), (TileKey{.page = 0, .scale = 1.0, .column = 1, .row = 0}));
  QCOMPARE(tiles.at(2), (TileKey{.page = 0, .scale = 1.0, .column = 0, .row = 1}));
  QCOMPARE(tiles.at(3), (TileKey{.page = 0, .scale = 1.0, .column = 1, .row = 1}));
  QVERIFY(grid.tilesIntersecting(kLetter, {.x = 900, .y = 0, .width = 100, .height = 100}).empty()); // beside the page
  QVERIFY(grid.tilesIntersecting(kLetter, {.x = 0, .y = 0, .width = 0, .height = 100}).empty());
  // An area that ends exactly at a tile's edge does not touch the next tile.
  QCOMPARE(grid.tilesIntersecting(kLetter, {.x = 0, .y = 0, .width = 512, .height = 512}).size(), std::size_t{1});
}

void TestTiles::refusesNonsenseSettings() {
  QCOMPARE(TileGrid(0.0, 512).scale(), 1.0);
  QCOMPARE(TileGrid(-2.0, 512).scale(), 1.0);
  QCOMPARE(TileGrid(std::numeric_limits<double>::quiet_NaN(), 512).scale(), 1.0);
  QCOMPARE(TileGrid(1.0, 1).tileSize(), TileGrid::kMinimumTileSize);
  QCOMPARE(TileGrid(1.0, 100'000).tileSize(), TileGrid::kMaximumTileSize);
  const ViewGeometry view({kLetter}, {.zoom = std::numeric_limits<double>::infinity(),
                                      .devicePixelRatio = 0.0,
                                      .viewportSize = QSizeF(-5, std::numeric_limits<double>::quiet_NaN())});
  QCOMPARE(view.scale(), 1.0);
  QCOMPARE(view.state().viewportSize, QSizeF(0, 0));
}

void TestTiles::centresAPageAreaNarrowerThanTheView() {
  const ViewGeometry view({kLetter}, {.zoom = 1.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700)});
  QCOMPARE(view.pageItemRect(kLetter), QRectF(92, 24, 816, 1056));
  QCOMPARE(view.contentSize(), QSizeF(816 + 48, 1056 + 48));
  QCOMPARE(view.maximumContentPosition(), QPointF(0, 1104 - 700));
  // The horizontal scroll position has no effect while everything fits.
  const ViewGeometry scrolled(
      {kLetter}, {.zoom = 1.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700), .contentPosition = QPointF(50, 100)});
  QCOMPARE(scrolled.pageItemRect(kLetter), QRectF(92, -76, 816, 1056));
}

void TestTiles::scrollsAPageAreaWiderThanTheView() {
  const ViewGeometry view({kLetter},
                          {.zoom = 2.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700), .contentPosition = QPointF(100, 0)});
  QCOMPARE(view.contentSize(), QSizeF(1632 + 48, 2112 + 48));
  QCOMPARE(view.pageItemRect(kLetter), QRectF(24 - 100, 24, 1632, 2112));
  // Pages below the first sit where the engine put them, scaled by the zoom.
  const auto pages = threePages();
  const ViewGeometry three(pages, {.zoom = 0.5, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700)});
  const QRectF first = three.pageItemRect(pages.at(0));
  const QRectF second = three.pageItemRect(pages.at(1));
  QCOMPARE(second.top() - first.top(), (1056 + 24) * 0.5);
  QCOMPARE(three.contentSize().height(), ((((3 * 1056) + (2 * 24)) * 0.5) + 48));
}

void TestTiles::placesPagesOnWholeDevicePixels_data() {
  QTest::addColumn<double>("ratio");
  QTest::addColumn<double>("zoom");
  QTest::addColumn<QPointF>("sceneOffset");
  QTest::addColumn<QPointF>("contentPosition");
  QTest::newRow("125 %") << 1.25 << 1.0 << QPointF(0, 56) << QPointF(0, 300);
  QTest::newRow("150 %, zoom 1.1") << 1.5 << 1.1 << QPointF(3.3, 41.7) << QPointF(17.1, 333.3);
  QTest::newRow("175 %, zoom 0.37") << 1.75 << 0.37 << QPointF(0.2, 0.4) << QPointF(0, 12.34);
  QTest::newRow("Retina, zoom 2.5") << 2.0 << 2.5 << QPointF(0, 28) << QPointF(400.25, 1000.75);
}

void TestTiles::placesPagesOnWholeDevicePixels() {
  QFETCH(double, ratio);
  QFETCH(double, zoom);
  QFETCH(QPointF, sceneOffset);
  QFETCH(QPointF, contentPosition);
  const auto pages = threePages();
  const ViewGeometry view(pages, {.zoom = zoom,
                                  .devicePixelRatio = ratio,
                                  .viewportSize = QSizeF(900, 700),
                                  .contentPosition = contentPosition,
                                  .sceneOffset = sceneOffset});
  const TileGrid grid(view.scale(), 512);
  for (const protocol::Page &page : pages) {
    // Every tile of the display's scale starts on a whole device pixel of the window and is exactly its pixels' size.
    for (int row = 0; row < grid.rows(page); ++row) {
      const QRectF tile = view.tileItemRect(page, grid, 0, row);
      QVERIFY2(isWholeDevicePixel(tile.left() + sceneOffset.x(), ratio), qPrintable(QString::number(tile.left())));
      QVERIFY2(isWholeDevicePixel(tile.top() + sceneOffset.y(), ratio), qPrintable(QString::number(tile.top())));
      QCOMPARE(tile.height() * ratio, static_cast<double>(grid.tilePixels(page, 0, row).height));
    }
  }
}

void TestTiles::limitsTheScrollPosition() {
  const ViewGeometry view({kLetter}, {.zoom = 1.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(500, 500)});
  QCOMPARE(view.clamped(QPointF(-10, -10)), QPointF(0, 0));
  QCOMPARE(view.clamped(QPointF(1e9, 1e9)), view.maximumContentPosition());
  QCOMPARE(view.clamped(QPointF(std::numeric_limits<double>::quiet_NaN(), 7)), QPointF(0, 7));
  // A view larger than the content cannot scroll.
  const ViewGeometry large({kLetter}, {.zoom = 0.25, .devicePixelRatio = 1.0, .viewportSize = QSizeF(2000, 2000)});
  QCOMPARE(large.maximumContentPosition(), QPointF(0, 0));
  // Without pages there is only the margin.
  const ViewGeometry empty({}, {.zoom = 1.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(100, 100)});
  QCOMPARE(empty.contentSize(), QSizeF(48, 48));
}

void TestTiles::convertsPointsForTheEngine() {
  const ViewGeometry view({kLetter},
                          {.zoom = 2.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700), .contentPosition = QPointF(0, 100)});
  // The page's top-left corner is the view origin: (0, 0) in device-independent pixels and in BLU.
  const QPointF corner = view.pageItemRect(kLetter).topLeft();
  QCOMPARE(view.viewPoint(corner), QPointF(0, 0));
  QCOMPARE(view.viewSpacePoint(corner), QPointF(0, 0));
  // 10 logical pixels right of it at zoom 2 are 10 device-independent pixels in the view (pointer positions are zoomed, §7) and 5 at
  // zoom 1, which is 95,250 BLU.
  QCOMPARE(view.viewPoint(corner + QPointF(10, 0)), QPointF(10, 0));
  QCOMPARE(view.viewSpacePoint(corner + QPointF(10, 0)), QPointF(95'250, 0));
  // The viewport is the canvas's area in view coordinates.
  QCOMPARE(view.viewport(), QRectF(-corner.x(), -corner.y(), 1000, 700));
}

void TestTiles::keepsThePointUnderThePointerWhileZooming() {
  const auto pages = threePages();
  const ViewState before{.zoom = 1.0, .devicePixelRatio = 1.25, .viewportSize = QSizeF(900, 700), .contentPosition = QPointF(0, 500)};
  const ViewGeometry view(pages, before);
  const QPointF pointer(600, 300);
  const QPointF point = view.viewSpacePoint(pointer);
  for (const double zoom : {1.5, 3.0, 0.5}) {
    ViewState after = before;
    after.zoom = zoom;
    after.contentPosition = view.contentPositionKeeping(point, pointer, zoom);
    const ViewGeometry zoomed(pages, after);
    // Within one device pixel of the new zoom, which is the most that snapping to whole pixels moves it. Horizontally only while the pages
    // are wider than the view: narrower ones are centred, as in Word.
    const double pixel = static_cast<double>(protocol::kBluPerDip) / zoomed.scale();
    QVERIFY(std::abs(zoomed.viewSpacePoint(pointer).y() - point.y()) <= pixel);
    if (zoomed.contentSize().width() > before.viewportSize.width()) {
      QVERIFY(std::abs(zoomed.viewSpacePoint(pointer).x() - point.x()) <= pixel);
    } else {
      // Centred up to the snapping to a whole device pixel.
      QVERIFY(std::abs(zoomed.pageItemRect(pages.front()).center().x() - (before.viewportSize.width() / 2)) <= 1.0);
    }
  }
}

void TestTiles::stretchesTilesOfAnotherZoom() {
  const ViewGeometry view({kLetter}, {.zoom = 2.0, .devicePixelRatio = 1.0, .viewportSize = QSizeF(1000, 700)});
  const TileGrid current(2.0, 512);
  const TileGrid old(1.0, 512);
  // A tile of zoom 1 is drawn twice its size at zoom 2, over the same part of the page as the tiles of zoom 2 that replace it.
  QCOMPARE(view.tileItemRect(kLetter, old, 0, 0), QRectF(view.pageItemRect(kLetter).topLeft(), QSizeF(1024, 1024)));
  const QRectF stretched = view.tileItemRect(kLetter, old, 1, 0);
  QCOMPARE(stretched.left(), view.tileItemRect(kLetter, current, 2, 0).left());
  QCOMPARE(stretched.right(), view.pageItemRect(kLetter).right());
}

void TestTiles::findsTheVisiblePartOfAPage() {
  const auto pages = threePages();
  const ViewGeometry view(pages, {.zoom = 1.0, .devicePixelRatio = 2.0, .viewportSize = QSizeF(900, 700)});
  const TileGrid grid(view.scale(), 512);
  // The first page fills the view below the margin: its first 676 logical rows, 1352 device pixels.
  const DeviceRect first = view.visiblePixels(pages.at(0), grid, 0.0);
  QCOMPARE(first.y, 0);
  QCOMPARE(first.height, (700 - 24) * 2);
  QCOMPARE(first.width, grid.pageWidthPixels(pages.at(0)));
  // The second page is out of sight, unless the margin reaches it.
  QVERIFY(view.visiblePixels(pages.at(1), grid, 0.0).isEmpty());
  QVERIFY(!view.visiblePixels(pages.at(1), grid, 500.0).isEmpty());
  // In another grid's pixels (a stale zoom), the same part of the page.
  const TileGrid half(1.0, 512);
  QCOMPARE(view.visiblePixels(pages.at(0), half, 0.0).height, 676);
}

void TestTiles::tilesRenderedBeforeAChangeAreDirty() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  const QSize size(512, 512);
  QVERIFY(cache.needsRender(key));
  cache.want(key, grid.tileRect(kLetter, 0, 0), size, 1);
  QVERIFY(cache.deliver(key, image(size), 5));
  QVERIFY(!cache.needsRender(key));
  QCOMPARE(cache.find(key)->version, std::uint64_t{1});
  // The engine changed part of the tile while handling request 7: the tile is still shown, but rendered again.
  cache.invalidate(0, {{.x = 100, .y = 100, .width = 10, .height = 10}}, 7);
  QVERIFY(cache.find(key)->dirty && !cache.find(key)->image.isNull());
  QVERIFY(cache.needsRender(key));
  // A tile the engine rendered after handling request 7 is fresh.
  QVERIFY(cache.deliver(key, image(size), 7));
  QVERIFY(!cache.find(key)->dirty);
  QCOMPARE(cache.find(key)->version, std::uint64_t{2});
}

void TestTiles::changesElsewhereLeaveTilesFresh() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  QVERIFY(cache.deliver(key, image(QSize(512, 512)), 5));
  cache.invalidate(1, {grid.tileRect(kLetter, 0, 0)}, 7);                  // another page
  cache.invalidate(0, {grid.tileRect(kLetter, 1, 1)}, 8);                  // another part of the page
  cache.invalidate(0, {{.x = 0, .y = 0, .width = 100, .height = 100}}, 4); // an older change, already in the tile
  QVERIFY(!cache.find(key)->dirty);
  QVERIFY(!cache.needsRender(key));
}

void TestTiles::aTileRenderedWhileTheDocumentChangedIsDirty() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  // The tile thread starts rendering after request 2; meanwhile the user types (request 3), which changes the tile's region.
  cache.invalidate(0, {{.x = 0, .y = 0, .width = 100, .height = 100}}, 3);
  QVERIFY(cache.deliver(key, image(QSize(512, 512)), 2));
  QVERIFY(cache.find(key)->dirty);
  QVERIFY(cache.needsRender(key));
}

void TestTiles::stopsAskingForRefusedTiles() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  for (int attempt = 0; attempt < TileCache::kMaxFailures; ++attempt) {
    QVERIFY(cache.needsRender(key));
    QVERIFY(!cache.deliver(key, QImage(), 1));
  }
  QVERIFY(!cache.needsRender(key));
  // An image of the wrong size counts as a refusal too.
  const TileKey other{.page = 0, .scale = 1.0, .column = 1, .row = 0};
  cache.want(other, grid.tileRect(kLetter, 1, 0), QSize(304, 512), 1);
  QVERIFY(!cache.deliver(other, image(QSize(512, 512)), 1));
  QCOMPARE(cache.find(other)->failures, 1);
  // A change to the region is worth another try.
  cache.invalidate(0, {grid.tileRect(kLetter, 0, 0)}, 2);
  QVERIFY(cache.needsRender(key));
  // A tile that is no longer in the cache is ignored when it arrives.
  QVERIFY(!cache.deliver({.page = 5, .scale = 1.0, .column = 0, .row = 0}, image(QSize(512, 512)), 1));
}

void TestTiles::forgetsOldInvalidationsSafely() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 1, .row = 1};
  cache.want(key, grid.tileRect(kLetter, 1, 1), QSize(304, 512), 1);
  // More changes than the cache remembers, all elsewhere. A tile rendered before the forgotten ones might have missed one of them, so it
  // counts as dirty; one rendered after them is fresh.
  for (qint64 cause = 1; cause <= static_cast<qint64>(TileCache::kRememberedInvalidations) + 10; ++cause) {
    cache.invalidate(0, {{.x = 0, .y = 0, .width = 10, .height = 10}}, cause);
  }
  QVERIFY(cache.deliver(key, image(QSize(304, 512)), 2));
  QVERIFY(cache.find(key)->dirty);
  QVERIFY(cache.deliver(key, image(QSize(304, 512)), 1000));
  QVERIFY(!cache.find(key)->dirty);
}

void TestTiles::resetForgetsTheChangesOfTheOldEngine() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  // Changes caused by requests up to 500 of one engine; clear() keeps them for tiles still being rendered.
  cache.invalidate(0, {grid.tileRect(kLetter, 0, 0)}, 500);
  cache.clear();
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  QVERIFY(cache.deliver(key, image(QSize(512, 512)), 3));
  QVERIFY(cache.find(key)->dirty);
  // Another engine counts its requests from 1 again: after reset(), its tile rendered after request 3 is fresh.
  cache.reset();
  QCOMPARE(cache.size(), std::size_t{0});
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  QVERIFY(cache.deliver(key, image(QSize(512, 512)), 3));
  QVERIFY(!cache.find(key)->dirty);
}

void TestTiles::remembersManyChangedRectanglesAsOne() {
  // Tiles of 256 pixels: four columns. Rectangles changed in columns 0 and 2 do not touch column 1, unless there are so many that the
  // cache remembers the rectangle around them, which does.
  const TileGrid grid(1.0, 256);
  const TileKey key{.page = 0, .scale = 1.0, .column = 1, .row = 0};
  const QSize size(256, 256);
  const protocol::BluRect left{.x = 0, .y = 0, .width = 100, .height = 100};
  const protocol::BluRect right{.x = grid.tileRect(kLetter, 2, 0).x, .y = 0, .width = 100, .height = 100};
  std::vector<protocol::BluRect> many(TileCache::kMaxRectsPerInvalidation, left);
  many.push_back(right);

  TileCache cache;
  cache.want(key, grid.tileRect(kLetter, 1, 0), size, 1);
  QVERIFY(cache.deliver(key, image(size), 5));
  cache.invalidate(0, {left, right}, 6);
  QVERIFY(!cache.find(key)->dirty);
  cache.invalidate(0, many, 7);
  QVERIFY(cache.find(key)->dirty);
  // The tiles still being rendered are judged by the same rectangle.
  TileCache rendering;
  rendering.want(key, grid.tileRect(kLetter, 1, 0), size, 1);
  rendering.invalidate(0, {left, right}, 6);
  QVERIFY(rendering.deliver(key, image(size), 5));
  QVERIFY(!rendering.find(key)->dirty);
  rendering.invalidate(0, many, 7);
  QVERIFY(rendering.deliver(key, image(size), 6));
  QVERIFY(rendering.find(key)->dirty);
}

void TestTiles::standInsComeFromTheClosestZoomThatHasThem() {
  // The missing tile: the top-left 512 × 512 device pixels of the page at 100 %. At 125 % and at 150 % that part of the page lies on two
  // columns and two rows of tiles.
  const protocol::BluRect area = TileGrid(1.0, 512).tileRect(kLetter, 0, 0);
  const TileGrid closer(1.25, 512);
  const TileGrid farther(1.5, 512);
  const std::vector<double> scales{closer.scale(), farther.scale()};
  const auto add = [](TileCache &cache, const TileGrid &grid, int column, int row) {
    const DeviceRect pixels = grid.tilePixels(kLetter, column, row);
    const QSize size(static_cast<int>(pixels.width), static_cast<int>(pixels.height));
    const TileKey key{.page = 0, .scale = grid.scale(), .column = column, .row = row};
    cache.want(key, grid.tileRect(kLetter, column, row), size, 1);
    return cache.deliver(key, image(size), 1);
  };
  const auto ranks = [](const std::vector<TileCache::StandIn> &standIns) {
    std::vector<std::size_t> result;
    result.reserve(standIns.size());
    for (const TileCache::StandIn &standIn : standIns) {
      result.push_back(standIn.rank);
    }
    return result;
  };

  TileCache cache;
  QVERIFY(cache.standInsFor(kLetter, area, scales, 512).empty());
  // The farther zoom has all four tiles, the closer one only the top-left one: both stand in, the closer one first.
  for (int column = 0; column < 2; ++column) {
    for (int row = 0; row < 2; ++row) {
      QVERIFY(add(cache, farther, column, row));
    }
  }
  QVERIFY(add(cache, closer, 0, 0));
  std::vector<TileCache::StandIn> standIns = cache.standInsFor(kLetter, area, scales, 512);
  QVERIFY(ranks(standIns) == (std::vector<std::size_t>{0, 1, 1, 1, 1}));
  QVERIFY(standIns.front().key == (TileKey{.page = 0, .scale = closer.scale(), .column = 0, .row = 0}));
  // Once the closer zoom has all four, the farther one is not needed.
  QVERIFY(add(cache, closer, 1, 0));
  QVERIFY(add(cache, closer, 0, 1));
  QVERIFY(add(cache, closer, 1, 1));
  standIns = cache.standInsFor(kLetter, area, scales, 512);
  QVERIFY(ranks(standIns) == (std::vector<std::size_t>{0, 0, 0, 0}));
}

void TestTiles::evictsTheLeastRecentlyUsedTilesOverBudget() {
  const QSize size(256, 256);
  const qint64 tileBytes = image(size).sizeInBytes();
  TileCache cache(tileBytes * 2);
  const TileGrid grid(1.0, 256);
  for (int column = 0; column < 3; ++column) {
    const TileKey key{.page = 0, .scale = 1.0, .column = column, .row = 0};
    cache.want(key, grid.tileRect(kLetter, column, 0), size, static_cast<std::uint64_t>(column) + 1);
    QVERIFY(cache.deliver(key, image(size), 1));
  }
  QCOMPARE(cache.bytes(), tileBytes * 3);
  // Frame 3 used only the last tile: the oldest goes first, and only as many as needed.
  cache.evict(3);
  QCOMPARE(cache.bytes(), tileBytes * 2);
  QVERIFY(cache.find({.page = 0, .scale = 1.0, .column = 0, .row = 0}) == nullptr);
  QVERIFY(cache.find({.page = 0, .scale = 1.0, .column = 1, .row = 0}) != nullptr);
  // Tiles the current frame shows stay, even over the budget.
  TileCache small(1);
  const TileKey shown{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  small.want(shown, grid.tileRect(kLetter, 0, 0), size, 9);
  QVERIFY(small.deliver(shown, image(size), 1));
  small.evict(9);
  QVERIFY(small.find(shown) != nullptr);
  small.clear();
  QCOMPARE(small.bytes(), 0);
  QCOMPARE(small.size(), std::size_t{0});
}

void TestTiles::forgetsTilesThatNeverArrived() {
  TileCache cache;
  const TileGrid grid(1.0, 512);
  const TileKey key{.page = 0, .scale = 1.0, .column = 0, .row = 0};
  cache.want(key, grid.tileRect(kLetter, 0, 0), QSize(512, 512), 1);
  cache.evict(1 + TileCache::kForgetEmptyAfterFrames);
  QVERIFY(cache.find(key) != nullptr);
  cache.evict(2 + TileCache::kForgetEmptyAfterFrames);
  QVERIFY(cache.find(key) == nullptr);
}

QTEST_GUILESS_MAIN(TestTiles)
#include "tst_tiles.moc"
