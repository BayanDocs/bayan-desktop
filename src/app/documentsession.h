// Drives the conversation with the engine for the one document the window shows: the hello/welcome handshake, opening the document,
// telling the engine what the canvas shows (view.set), and keeping what the engine says about the document: its pages, the overlays to draw
// (caret, selection), the regions to draw again and the accessibility tree. It also relays the user's input as protocol messages.
//
// It contains no document logic: the engine decides everything, the session only relays (ADR-0013 §3). Every message it sends is a request
// with an identifier, because the engine answers each request before sending the events that handling it caused (engine protocol §4, §8).
// The session therefore knows which request caused each event, and tells the canvas, which uses it to tell fresh tiles from stale ones.

#pragma once

#include "engine.h"
#include "protocol.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

namespace bayan {

class DocumentSession final : public QObject {
  Q_OBJECT
  // An explicit name: with automatic naming, qmltyperegistrar would check the namespace-qualified bayan::DocumentSession.
  QML_NAMED_ELEMENT(DocumentSession)
  QML_UNCREATABLE("DocumentSession is created by the application.")
  // QML refers to the status values as DocumentSession.Status.Ready and so on.
  Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")
  Q_PROPERTY(Status status READ status NOTIFY statusChanged FINAL)
  Q_PROPERTY(QString engineVersion READ engineVersion NOTIFY statusChanged FINAL)
  Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged FINAL)

public:
  // quint8 rather than std::uint8_t: Qt's QML tooling knows Qt's integer types.
  enum class Status : quint8 { Starting, Ready, Failed };
  Q_ENUM(Status)

  // What view.set tells the engine (engine protocol §6.2).
  struct View {
    // The visible part of the page area, in device-independent pixels from the view origin.
    QRectF viewport{};
    double zoom = 1.0;
    double deviceScale = 1.0;
    friend bool operator==(const View &, const View &) = default;
  };

  // The engine must outlive the session.
  explicit DocumentSession(Engine &engine, QObject *parent = nullptr);
  // Releases the placeholder blob if the session still holds it.
  ~DocumentSession() override;
  Q_DISABLE_COPY_MOVE(DocumentSession)

  // Starts the conversation: hello, then doc.open, then view.set and query.a11y.
  void start();

  [[nodiscard]] Status status() const { return status_; }
  [[nodiscard]] QString engineVersion() const { return engineVersion_; }
  // The features the engine announced in welcome, such as mock_document.
  [[nodiscard]] QStringList features() const { return features_; }
  [[nodiscard]] int pageCount() const { return static_cast<int>(pages_.size()); }
  [[nodiscard]] const std::vector<protocol::Page> &pages() const { return pages_; }
  [[nodiscard]] const protocol::Overlay &overlay() const { return overlay_; }
  // The accessibility tree, once the engine has sent it; empty if the engine has none (the stub engine).
  [[nodiscard]] const std::optional<protocol::AccessibleTree> &accessibilityTree() const { return accessibilityTree_; }
  [[nodiscard]] std::optional<protocol::AccessibleCaret> accessibilityCaret() const { return accessibilityCaret_; }
  [[nodiscard]] qint64 documentId() const { return documentId_; }
  [[nodiscard]] Engine &engine() const { return engine_; }
  // The view the session last sent, or will send once the document is open.
  [[nodiscard]] const View &view() const { return view_; }

  // Tells the engine what the canvas shows. Sent at once if the document is open and the view differs from the last one sent, otherwise
  // when the document opens.
  void setView(const View &view);

  // Relay input to the engine (engine protocol §6.5). Each returns the request's identifier, or 0 if nothing was sent (no document).
  qint64 sendKey(const protocol::KeyInput &key);
  qint64 sendText(const QString &text);
  qint64 sendComposition(protocol::CompositionPhase phase, const QString &text = {}, int selectionStart = 0, int selectionEnd = 0);
  qint64 sendPointer(const protocol::PointerInput &pointer);

signals:
  void statusChanged();
  void pagesChanged();
  // The engine changed these regions while handling request cause; tiles rendered before that request was handled are out of date.
  void invalidated(const std::vector<bayan::protocol::PageRect> &regions, qint64 cause);
  void overlayChanged();
  // The accessibility tree arrived or changed: changedNodes lists the identifiers of the replaced nodes (empty for a whole new tree).
  void accessibilityChanged(const QList<quint64> &changedNodes);
  // The engine refused an input message, for example text over the mock document's length limit. errorCode is the protocol's error code.
  void inputRefused(const QString &errorCode);

private:
  void onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  void onEvent(const QString &type, const QJsonObject &payload);
  void onOpened(const QJsonObject &payload);
  void setPages(const QJsonObject &payload);
  void setOverlay(const QJsonObject &payload);
  void setAccessibilityTree(const QJsonObject &payload);
  void applyAccessibilityUpdate(const QJsonObject &payload);
  void sendView();
  qint64 sendForDocument(const QString &type, QJsonObject payload);
  // The request whose handling caused the event being received now.
  [[nodiscard]] qint64 currentCause() const;
  void fail();
  void releaseDocumentBlob();

  Engine &engine_;
  Status status_ = Status::Starting;
  QString engineVersion_;
  QStringList features_;
  qint64 documentId_ = 0;
  std::vector<protocol::Page> pages_;
  protocol::Overlay overlay_;
  std::optional<protocol::AccessibleTree> accessibilityTree_;
  std::optional<protocol::AccessibleCaret> accessibilityCaret_;
  View view_{.viewport = QRectF(0, 0, 1024, 768), .zoom = 1.0, .deviceScale = 1.0};
  std::optional<View> sentView_;
  // The placeholder document's bytes, held by the engine from doc.open until the engine has answered it.
  BlobId documentBlob_ = 0;
  qint64 helloRequest_ = 0;
  qint64 openRequest_ = 0;
  qint64 accessibilityRequest_ = 0;
  std::vector<qint64> viewRequests_;
  // The identifier of the last reply received from the engine, for any request.
  qint64 lastReply_ = 0;
};

} // namespace bayan
