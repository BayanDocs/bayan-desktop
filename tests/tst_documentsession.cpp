// Unit tests for the document session (src/app/documentsession.h), run against the stub engine.

#include "documentsession.h"
#include "engine.h"

#include <QtCore/QJsonObject>
#include <QtCore/QSizeF>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <memory>

using namespace Qt::StringLiterals;

namespace {

using Status = bayan::DocumentSession::Status;

// An engine whose stub answers doc.open as <mode> says ("normal", "fail" or "ignore"; see src/engine/stub/bayan_engine_stub.cpp).
std::unique_ptr<bayan::Engine> engineWithDocOpen(const QString &mode) {
  return bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"doc_open"_s, mode}}}});
}

// How many blobs the engine still holds. The stub numbers blobs 1, 2, 3 and so on, and these tests create at most a few, so a leaked blob
// shows up among the first identifiers.
int liveBlobs(bayan::Engine &engine) {
  int count = 0;
  for (bayan::BlobId id = 1; id <= 16; ++id) {
    if (engine.blob(id).has_value()) {
      ++count;
    }
  }
  return count;
}

} // namespace

class TestDocumentSession final : public QObject {
  Q_OBJECT

private slots:
  void opensTheDocumentAndReleasesTheBlob();
  void releasesTheBlobWhenOpeningFails();
  void releasesTheBlobWhenDestroyedWhileOpening();
  void stubRejectsUnknownTestSwitches();
};

void TestDocumentSession::opensTheDocumentAndReleasesTheBlob() {
  const auto engine = engineWithDocOpen(u"normal"_s);
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  session.start();
  QTRY_COMPARE(session.status(), Status::Ready);
  QCOMPARE(session.pageCount(), 1);
  QCOMPARE(session.pageSize(), QSizeF(816, 1056));
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::releasesTheBlobWhenOpeningFails() {
  const auto engine = engineWithDocOpen(u"fail"_s);
  QVERIFY(engine);
  bayan::DocumentSession session(*engine);
  QSignalSpy statusChanges(&session, &bayan::DocumentSession::statusChanged);
  session.start();
  QTRY_COMPARE(session.status(), Status::Failed);
  QCOMPARE(liveBlobs(*engine), 0);
  QVERIFY(statusChanges.count() >= 1);
}

void TestDocumentSession::releasesTheBlobWhenDestroyedWhileOpening() {
  const auto engine = engineWithDocOpen(u"ignore"_s);
  QVERIFY(engine);
  auto session = std::make_unique<bayan::DocumentSession>(*engine);
  session->start();
  // The session creates the placeholder blob after the handshake and then waits for doc.open, which this engine never answers.
  QTRY_COMPARE(liveBlobs(*engine), 1);
  QCOMPARE(session->status(), Status::Starting);
  session.reset();
  QCOMPARE(liveBlobs(*engine), 0);
}

void TestDocumentSession::stubRejectsUnknownTestSwitches() {
  QVERIFY(!engineWithDocOpen(u"sometimes"_s));
  QVERIFY(!bayan::Engine::create(QJsonObject{{u"test"_s, QJsonObject{{u"unknown_switch"_s, true}}}}));
}

QTEST_MAIN(TestDocumentSession)
#include "tst_documentsession.moc"
