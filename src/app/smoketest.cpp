#include "smoketest.h"

#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace bayan {
namespace {

// Points of the stub engine's test page (in device-independent pixels from the page's top-left corner, so 96 per inch) and the colour
// the stub draws there: the left margin, the heading bar and the first line of placeholder text.
struct Probe {
  QPointF point{};
  QColor color{};
  const char *what = "";
};
const std::array kProbes{
    Probe{.point = {48.0, 48.0}, .color = QColor(255, 255, 255), .what = "page margin"},
    Probe{.point = {408.0, 120.0}, .color = QColor(31, 95, 191), .what = "heading bar"},
    Probe{.point = {200.0, 198.0}, .color = QColor(154, 160, 166), .what = "first text line"},
};

// Scaling and colour management can shift a channel slightly; anything closer than this counts as a match.
constexpr int kTolerance = 12;

bool close(const QColor &actual, const QColor &expected) {
  return std::abs(actual.red() - expected.red()) <= kTolerance && std::abs(actual.green() - expected.green()) <= kTolerance &&
         std::abs(actual.blue() - expected.blue()) <= kTolerance;
}

// QQuickImage::Status::Ready; the class is private to Qt, so its value is used through the "status" property.
constexpr int kImageReady = 1;

} // namespace

SmokeTest::SmokeTest(QQmlApplicationEngine &qml, DocumentSession &session) : qml_(qml), session_(session) {
  poll_.setInterval(50ms);
  connect(&poll_, &QTimer::timeout, this, &SmokeTest::check);
  deadline_.setSingleShot(true);
  deadline_.setInterval(30s);
  connect(&deadline_, &QTimer::timeout, this, [this] { finish(false, u"timed out waiting for the page to be displayed"_s); });
}

void SmokeTest::start() {
  poll_.start();
  deadline_.start();
}

void SmokeTest::check() {
  if (session_.status() == DocumentSession::Status::Failed) {
    finish(false, u"the engine session failed"_s);
    return;
  }
  const QList<QObject *> roots = qml_.rootObjects();
  auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.constFirst());
  auto *page = window == nullptr ? nullptr : window->findChild<QQuickItem *>(u"pageImage"_s);
  if (page == nullptr || page->property("status").toInt() != kImageReady || page->width() <= 0) {
    return; // Not displayed yet.
  }

  const QImage frame = window->grabWindow();
  if (frame.isNull()) {
    finish(false, u"could not capture the window"_s);
    return;
  }
  const qreal ratio = frame.devicePixelRatio();
  for (const Probe &probe : kProbes) {
    const QPointF scenePoint = page->mapToScene(probe.point) * ratio;
    const int x = static_cast<int>(std::floor(scenePoint.x()));
    const int y = static_cast<int>(std::floor(scenePoint.y()));
    if (!frame.valid(x, y)) {
      finish(false, u"the %1 is outside the window"_s.arg(QLatin1StringView(probe.what)));
      return;
    }
    const QColor actual = frame.pixelColor(x, y);
    if (!close(actual, probe.color)) {
      finish(false, u"the %1 has colour %2 instead of %3"_s.arg(QLatin1StringView(probe.what), actual.name(), probe.color.name()));
      return;
    }
  }
  finish(true, u"the stub page is displayed (engine %1, %2 page(s))"_s.arg(session_.engineVersion()).arg(session_.pageCount()));
}

void SmokeTest::finish(bool passed, const QString &detail) {
  poll_.stop();
  deadline_.stop();
  if (passed) {
    qInfo("smoke test passed: %s", qUtf8Printable(detail));
    QCoreApplication::exit(EXIT_SUCCESS);
  } else {
    qCritical("smoke test failed: %s", qUtf8Printable(detail));
    QCoreApplication::exit(EXIT_FAILURE);
  }
}

} // namespace bayan
