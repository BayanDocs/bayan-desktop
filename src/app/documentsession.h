// Drives the conversation with the engine for the one document the window shows: the hello/welcome handshake, opening the document,
// telling the engine what the canvas shows (view.set), and keeping what the engine says about the document: its pages, the overlays to draw
// (caret, selection), the regions to draw again and the accessibility tree. It also relays the user's input as protocol messages.
//
// It contains no document logic: the engine decides everything, the session only relays (ADR-0013 §3). Every message it sends is a request
// with an identifier, because the v0 engine answers each request before sending the events that handling it caused. The protocol states
// that order only for view.set (engine protocol §6); for the input messages it is how bayan-engine behaves at the pinned commit (its
// engine.rs replies before it reports the changed line), and the DESK-002 report proposes making it normative. The session therefore knows
// which request caused each event, and tells the canvas, which uses it to tell fresh tiles from stale ones.
//
// It also recovers from engine failures as engine protocol §12.1 says, the policy both shells follow. After an engine.error whose
// recoverable is anything but true, the engine has discarded its session; the session then
// 1. starts a new one with hello, at most three times in any ten minutes (RestartAllowance), and otherwise stops: it sends the engine
//    nothing more, and only starting the app again brings the engine back;
// 2. says each time, in its own words (message, which the interface shows and announces to screen readers), that the engine stopped working
//    and was restarted, which restart this is, and what became of the document; or, at the stop, how to start the engine again. Nothing
//    of the error is used but recoverable: its code, message identifier and arguments come from the engine, which may be repeating what a
//    document made it say;
// 3. never reopens the document that was open, or being opened, when the engine failed, because it may be what made the engine fail: it
//    offers to (offer), and sends doc.open only when the user takes the offer (reopen()). The offer stays through further failures and goes
//    at the stop. documentClosed() tells the interface first, so that keyboard focus in the document can stay in its area;
// 4. ignores what is left of the failed session: the error panic in reply to the message that failed, the answers to requests sent before
//    the new hello (handshake_required, from the new session before its handshake) and events about the old session's document.

#pragma once

#include "engine.h"
#include "protocol.h"
#include "restartallowance.h"

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
  Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
  Q_PROPERTY(Offer offer READ offer NOTIFY offerChanged FINAL)

public:
  // quint8 rather than std::uint8_t: Qt's QML tooling knows Qt's integer types.
  enum class Status : quint8 {
    // The engine session is starting, or the document is being opened.
    Starting,
    // The document is open and its pages are known.
    Ready,
    // The engine works, but no document is open: after a restart (see offer) or when the document could not be opened.
    Closed,
    // The session stopped for good: the engine failed too often, or sent something the shell cannot trust. Nothing more is sent to it.
    Failed,
  };
  Q_ENUM(Status)

  // What the interface offers next to the message after a restart (engine protocol §12.1, point 3).
  enum class Offer : quint8 {
    None,
    // To reopen the document that a failure closed.
    Reopen,
    // To open the document, if a failure came before it was ever open.
    Open,
  };
  Q_ENUM(Offer)

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
  // Takes the offer: opens the document after a restart, when the user asks (engine protocol §12.1, point 3). Does nothing without an
  // offer.
  Q_INVOKABLE void reopen();

  [[nodiscard]] Status status() const { return status_; }
  // The latest message for the user about the engine and the document, in the shell's own words, or empty. Never text from the engine.
  [[nodiscard]] QString message() const { return message_; }
  [[nodiscard]] Offer offer() const { return offer_; }
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
  // A new message, also one with the same text as the last, so that each one is announced.
  void messageChanged();
  void offerChanged();
  // A failure closed the document that was open, or being opened: the session has forgotten it and sends nothing more for it. Sent before
  // the pages and the status change, so that the interface can move keyboard focus while the canvas is still shown.
  void documentClosed();
  void pagesChanged();
  // The engine changed these regions while handling request cause; tiles rendered before that request was handled are out of date.
  void invalidated(const std::vector<bayan::protocol::PageRect> &regions, qint64 cause);
  void overlayChanged();
  // The accessibility tree arrived or changed: changedNodes lists the identifiers of the replaced nodes (empty for a whole new tree).
  void accessibilityChanged(const QList<quint64> &changedNodes);
  // The engine refused an input message, for example text over the mock document's length limit. errorCode is the protocol's error code.
  void inputRefused(const QString &errorCode);

private:
  void startSession();
  void onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  void onWelcome(const QJsonObject &payload);
  void openDocument();
  void onOpenReply(bool ok, const QJsonObject &payload);
  void onEvent(const QString &type, const QJsonObject &payload);
  void onOpened(const QJsonObject &payload);
  // An unrecoverable engine.error: the engine discarded its session (engine protocol §12.1).
  void recover();
  // Forgets the session's document and requests; their replies and events are ignored from now on.
  void forgetSession(bool closesDocument);
  void stop(const QString &message);
  void setStatus(Status status);
  void setMessage(const QString &message);
  void setOffer(Offer offer);
  void setPages(const QJsonObject &payload);
  void setOverlay(const QJsonObject &payload);
  void setAccessibilityTree(const QJsonObject &payload);
  void applyAccessibilityUpdate(const QJsonObject &payload);
  void sendView();
  qint64 sendForDocument(const QString &type, QJsonObject payload);
  // The request whose handling caused the event being received now.
  [[nodiscard]] qint64 currentCause() const;
  // The engine sent something the session cannot trust, or refused something it must not: stops for good.
  void fail();
  void releaseDocumentBlob();

  Engine &engine_;
  Status status_ = Status::Starting;
  QString message_;
  Offer offer_ = Offer::None;
  RestartAllowance restarts_;
  // The current session's hello was answered, so doc.open can be sent.
  bool welcomed_ = false;
  // Send doc.open as soon as the session is welcomed: at the start, and when the user takes the offer before the welcome arrived.
  bool openWhenWelcomed_ = false;
  // doc.open was sent and the document is not open yet, also once the error panic answered it: the engine.error that follows closes it.
  bool opening_ = false;
  // The offer the user took, until the document it opens is shown; it decides how the message says so.
  Offer taken_ = Offer::None;
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
  // The current session's hello. Requests are numbered in the order they are sent, so every request with a smaller number belongs to an
  // earlier session.
  qint64 helloRequest_ = 0;
  qint64 openRequest_ = 0;
  qint64 accessibilityRequest_ = 0;
  std::vector<qint64> viewRequests_;
  // The identifier of the last reply received from the engine, for any request.
  qint64 lastReply_ = 0;
};

} // namespace bayan
