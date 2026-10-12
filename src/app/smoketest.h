// The headless smoke test (`bayan-desktop --smoke-test`): waits until the document canvas shows every visible tile of the first page, then
// checks that the window shows exactly the engine's pixels: for points spread over the visible part of the first page, it asks the engine
// for the single device pixel there and compares it with the pixel on screen. It knows nothing about what the page looks like, so it works
// with the stub engine and with the real one. It also checks that the canvas's accessibility interface exposes the engine's text when the
// engine has an accessibility tree. Exits with status 0 if everything matches (1 otherwise, or after a timeout).
// CI runs it with the offscreen platform plugin, so it needs no screen.

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

class QQmlApplicationEngine;

namespace bayan {

class DocumentSession;

class SmokeTest final : public QObject {
  Q_OBJECT

public:
  SmokeTest(QQmlApplicationEngine &qml, DocumentSession &session);

  void start();

private:
  void check();
  void finish(bool passed, const QString &detail);

  QQmlApplicationEngine &qml_;
  DocumentSession &session_;
  QTimer poll_;
  QTimer deadline_;
};

} // namespace bayan
