// bayan-desktop: starts the engine, the QML interface and, with --smoke-test or --benchmark, the headless self-check used by CI or the
// measurement mode of DESK-002.

#include "benchmark.h"
#include "canvasaccessible.h"
#include "documentcanvas.h"
#include "documentsession.h"
#include "engine.h"
#include "smoketest.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlExtensionPlugin>
#include <QtQuick/QQuickWindow>
#include <QtQuickControls2/QQuickStyle>

#include <cstdlib>
#include <memory>

// The interface is a static QML module (src/CMakeLists.txt); this links its types and files into the program.
Q_IMPORT_QML_PLUGIN(Bayan_DesktopPlugin)

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
                                           u"Check that the canvas shows the engine's pixels, then exit with status 0 on success."_s);
  const QCommandLineOption benchmarkOption(
      u"benchmark"_s, u"Scroll, zoom and type through the document, measure frame times and latencies, and write them as JSON to <file>."_s,
      u"file"_s);
  const QCommandLineOption benchmarkScaleOption(u"benchmark-scale"_s, u"Multiply the length of the benchmark's phases by <factor>."_s,
                                                u"factor"_s, u"1"_s);
  const QCommandLineOption tileSizeOption(u"tile-size"_s, u"Render the document in square tiles of <pixels> device pixels."_s, u"pixels"_s);
  parser.addOptions({smokeTestOption, benchmarkOption, benchmarkScaleOption, tileSizeOption});
  parser.process(app);

  // Fusion looks the same on every platform and follows the system's light or dark colour scheme.
  QQuickStyle::setStyle(u"Fusion"_s);
  // Screen readers find the document canvas's text through this factory (DESK-002).
  bayan::installCanvasAccessibleFactory();

  const std::unique_ptr<bayan::Engine> engine = bayan::Engine::create();
  if (!engine) {
    qCritical("The document engine could not be started.");
    return EXIT_FAILURE;
  }
  bayan::DocumentSession session(*engine);

  // Declared after the engine and the session, so it is destroyed before them: the document canvas it owns uses both.
  QQmlApplicationEngine qml;
  qml.setInitialProperties({{u"session"_s, QVariant::fromValue(&session)}});
  QObject::connect(
      &qml, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
  qml.loadFromModule(u"Bayan.Desktop"_s, u"Main"_s);

  if (parser.isSet(tileSizeOption)) {
    const QList<QObject *> roots = qml.rootObjects();
    auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.constFirst());
    if (auto *canvas = window == nullptr ? nullptr : window->findChild<bayan::DocumentCanvas *>(u"documentCanvas"_s)) {
      canvas->setTileSize(parser.value(tileSizeOption).toInt());
    }
  }

  std::unique_ptr<bayan::SmokeTest> smokeTest;
  std::unique_ptr<bayan::Benchmark> benchmark;
  if (parser.isSet(smokeTestOption)) {
    smokeTest = std::make_unique<bayan::SmokeTest>(qml, session);
    smokeTest->start();
  } else if (parser.isSet(benchmarkOption)) {
    bool valid = false;
    const double scale = parser.value(benchmarkScaleOption).toDouble(&valid);
    benchmark = std::make_unique<bayan::Benchmark>(
        qml, session,
        bayan::Benchmark::Options{.outputPath = parser.value(benchmarkOption), .durationScale = valid && scale > 0 ? scale : 1.0});
    benchmark->start();
  }

  session.start();
  return QGuiApplication::exec();
}
