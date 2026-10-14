#include "smoketest.h"

#include "documentcanvas.h"
#include "documentsession.h"
#include "pixelprobe.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QAccessible>
#include <QtGui/QImage>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <cstdlib>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace bayan {

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
  auto *canvas = window == nullptr ? nullptr : window->findChild<DocumentCanvas *>(u"documentCanvas"_s);
  if (canvas == nullptr || session_.status() != DocumentSession::Status::Ready || !canvas->tilesComplete() || session_.pages().empty()) {
    return; // Not displayed yet.
  }
  // The engine's accessibility tree, if it has one, reaches the platform through the canvas's interface.
  if (session_.accessibilityTree()) {
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(canvas);
    if (accessible == nullptr || accessible->textInterface() == nullptr || accessible->textInterface()->characterCount() == 0) {
      finish(false, u"the canvas's accessibility interface does not expose the document's text"_s);
      return;
    }
  }

  const QImage frame = window->grabWindow();
  if (frame.isNull()) {
    finish(false, u"could not capture the window"_s);
    return;
  }
  if (const auto problem = compareWithEnginePixels(*canvas, frame)) {
    finish(false, *problem);
    return;
  }
  finish(true, u"the canvas shows the engine's pixels (engine %1, %2 page(s), %3 accessible characters)"_s.arg(session_.engineVersion())
                   .arg(session_.pageCount())
                   .arg(canvas->accessibleText().text().size()));
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
