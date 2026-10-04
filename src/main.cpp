// bayan-desktop: starts the engine, the QML interface and, with --smoke-test, the headless self-check used by CI.

#include "documentsession.h"
#include "engine.h"
#include "smoketest.h"
#include "tileimageprovider.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>

#include <cstdlib>
#include <memory>

using namespace Qt::StringLiterals;

int main(int argc, char *argv[]) {
  const QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(u"BayanDocs"_s);
  QGuiApplication::setOrganizationName(u"BayanDocs"_s);
  QGuiApplication::setApplicationVersion(QString::fromLatin1(BAYAN_DESKTOP_VERSION));

  QCommandLineParser parser;
  parser.setApplicationDescription(u"BayanDocs, a free word processor."_s);
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption smokeTestOption(u"smoke-test"_s,
                                           u"Check that the engine's page is displayed, then exit with status 0 on success."_s);
  parser.addOption(smokeTestOption);
  parser.process(app);

  // Fusion looks the same on every platform and follows the system's light or dark colour scheme.
  QQuickStyle::setStyle(u"Fusion"_s);

  const std::unique_ptr<bayan::Engine> engine = bayan::Engine::create();
  if (!engine) {
    qCritical("The document engine could not be started.");
    return EXIT_FAILURE;
  }
  bayan::DocumentSession session(*engine);

  // Declared after the engine and the session, so it is destroyed before them: the image provider it owns uses the session.
  QQmlApplicationEngine qml;
  qml.addImageProvider(u"tiles"_s,
                       new bayan::TileImageProvider(session)); // NOLINT(cppcoreguidelines-owning-memory): the QML engine takes ownership.
  qml.setInitialProperties({{u"session"_s, QVariant::fromValue(&session)}});
  QObject::connect(
      &qml, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
  qml.loadFromModule(u"Bayan.Desktop"_s, u"Main"_s);

  std::unique_ptr<bayan::SmokeTest> smokeTest;
  if (parser.isSet(smokeTestOption)) {
    smokeTest = std::make_unique<bayan::SmokeTest>(qml, session);
    smokeTest->start();
  }

  session.start();
  return QGuiApplication::exec();
}
