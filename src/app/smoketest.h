// The headless smoke test (`bayan-desktop --smoke-test`): waits until the window shows the engine's page, checks a few pixels of the
// rendered window against the stub engine's known test page, and exits with status 0 if they match (1 otherwise, or after a timeout).
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
