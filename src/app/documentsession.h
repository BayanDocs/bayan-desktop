// Drives the conversation with the engine for the one document the window shows: the hello/welcome handshake, the UI manifest's key data,
// opening the document, telling the engine what the canvas shows (view.set), and keeping what the engine says about the document: its
// pages, the overlays to draw (caret, selection), the regions to draw again and the accessibility tree. It also relays the user's input as
// protocol messages.
//
// It contains no document logic: the engine decides everything, the session only relays (ADR-0013 §3). It speaks engine protocol v1
// (engine protocol §14):
// - Handshake (§14.1): hello offers version 1 and names the operating system from the protocol's fixed list and the runtime, native. The
//   engine's refusal of hello ends the engine's use without another hello: after unsupported_protocol_version the session shows the update
//   prompt (the installation is incomplete; no update action yet, DESK-104), after any other error the stop message. A panic in reply to it
//   is a failure like any other, which the engine.error that follows handles.
// - Protocol violations (§14.1 item 5): every message's envelope is checked by Engine, and every payload field the session reads, its type
//   and its range, here, before it is used; a message that breaks the protocol stops the session's use of the engine with the stop message,
//   without a new session.
// - Revisions (§14.6): the regions of render.invalidate come with the document's render revision after the change, which the canvas
// compares
//   with each tile's own; nothing depends on the order of tiles and events.
// - The accessibility tree (§14.7 item 1): one beyond the protocol's limits breaks the protocol; a node given with only a part of its
//   children has the rest loaded at once with query.a11y and children ranges, so that screen readers get the whole document.
//
// It also recovers from engine failures as engine protocol §12.1 and §14.2 say, the policy both shells follow. Every message from the
// engine goes through SessionTracker first, which ignores what is left of earlier sessions. After an engine.error whose recoverable is
// false, the engine has discarded its session, and its restart says what follows; the session counts nothing itself (the engine's binding
// does):
// 1. with restart { number, limit }, it starts a new session with hello; with restart null it stops: it sends the engine nothing more, and
//    only starting the app again brings the engine back;
// 2. it says each time, in its own words (message, which the interface shows and announces to screen readers), that the engine stopped
//    working and was restarted, which restart this is ("1 of 3", from restart), and what became of the document; or, at the stop, how to
//    start the engine again. Nothing of the error is used but recoverable and restart: its code, message identifier and arguments come from
//    the engine, which may be repeating what a document made it say;
// 3. it never reopens the document that was open, or being opened, when the engine failed, because it may be what made the engine fail: it
//    offers to (offer), and sends doc.open only when the user takes the offer (reopen()). The offer stays through further failures and goes
//    at the stop. documentClosed() tells the interface first, so that keyboard focus in the document can stay in its area;
// 4. it ignores what is left of the failed session (SessionTracker): the error panic in reply to the message that failed, the answers to
//    requests sent before the new hello (handshake_required, from the new session before its handshake) and events about the old session's
//    document.

#pragma once

#include "engine.h"
#include "keybindings.h"
#include "protocol.h"
#include "sessiontracker.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <optional>
#include <set>
#include <utility>
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
  Q_PROPERTY(bool updateRequired READ updateRequired NOTIFY statusChanged FINAL)
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
    // The session stopped for good: the engine stopped after failing too often, refused the handshake, or broke the protocol. Nothing more
    // is sent to it; updateRequired says whether the installation needs an update.
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

  // The operating systems whose keyboard conventions hello names (engine protocol §14.1 item 2).
  enum class OperatingSystem : quint8 { Windows, MacOS, Linux, IOS, Android, ChromeOS, Other };
  // The operating system the app was built for, as Qt knows it.
  [[nodiscard]] static OperatingSystem operatingSystem();
  // Its name in hello's shell.platform.
  [[nodiscard]] static QString platformName(OperatingSystem system);

  // The engine must outlive the session.
  explicit DocumentSession(Engine &engine, QObject *parent = nullptr);
  // Releases the placeholder blob if the session still holds it.
  ~DocumentSession() override;
  Q_DISABLE_COPY_MOVE(DocumentSession)

  // Starts the conversation: hello, then ui.manifest and doc.open, then view.set and query.a11y.
  void start();
  // Takes the offer: opens the document after a restart, when the user asks (engine protocol §12.1, point 3). Does nothing without an
  // offer.
  Q_INVOKABLE void reopen();

  [[nodiscard]] Status status() const { return status_; }
  // Whether the session stopped because the engine refused the handshake with unsupported_protocol_version: the app and its engine do not
  // fit together, and the installation must be updated or reinstalled (engine protocol §14.1 item 3).
  [[nodiscard]] bool updateRequired() const { return updateRequired_; }
  // The latest message for the user about the engine and the document, in the shell's own words, or empty. Never text from the engine.
  [[nodiscard]] QString message() const { return message_; }
  [[nodiscard]] Offer offer() const { return offer_; }
  [[nodiscard]] QString engineVersion() const { return engineVersion_; }
  // The features the engine announced in welcome, such as mock_document.
  [[nodiscard]] QStringList features() const { return features_; }
  // Which presses go to the engine (engine protocol §14.3), from the UI manifest; none until it has arrived.
  [[nodiscard]] const KeyBindings &keyBindings() const { return keyBindings_; }
  // The labels of a command's shortcuts as the engine formats them for the platform, for wherever the interface shows them (§14.3).
  Q_INVOKABLE [[nodiscard]] QStringList shortcutLabels(const QString &commandId) const { return keyBindings_.labels(commandId); }
  [[nodiscard]] int pageCount() const { return static_cast<int>(pages_.size()); }
  [[nodiscard]] const std::vector<protocol::Page> &pages() const { return pages_; }
  [[nodiscard]] const protocol::Overlay &overlay() const { return overlay_; }
  // The accessibility tree, once the engine has sent it; empty if the engine has none (the stub engine). Nodes whose other children are
  // being loaded hold only some of them meanwhile.
  [[nodiscard]] const std::optional<protocol::AccessibleTree> &accessibilityTree() const { return accessibilityTree_; }
  [[nodiscard]] std::optional<protocol::AccessibleCaret> accessibilityCaret() const { return accessibilityCaret_; }
  // Whether parts of the accessibility tree are still being loaded.
  [[nodiscard]] bool loadingAccessibilityParts() const { return !accessibilityParts_.empty(); }
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
  // The UI manifest's key data arrived.
  void keyBindingsChanged();
  // A failure closed the document that was open, or being opened: the session has forgotten it and sends nothing more for it. Sent before
  // the pages and the status change, so that the interface can move keyboard focus while the canvas is still shown.
  void documentClosed();
  void pagesChanged();
  // The engine changed these regions; revision is the document's render revision after the change (engine protocol §14.6). A tile that
  // shows an earlier revision is out of date where a region covers it.
  void invalidated(const std::vector<bayan::protocol::PageRect> &regions, quint64 revision);
  void overlayChanged();
  // The accessibility tree arrived or changed: changedNodes lists the identifiers of the replaced nodes (empty for a whole new tree).
  void accessibilityChanged(const QList<quint64> &changedNodes);
  // The engine refused an input message, for example text over the mock document's length limit. errorCode is the protocol's error code.
  void inputRefused(const QString &errorCode);

private:
  void startSession();
  // Sends a request and tells the tracker; 0 if the engine refused it.
  qint64 request(const QString &type, const QJsonObject &payload);
  void onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  void onHelloReply(bool ok, const QJsonObject &payload, const QJsonObject &error);
  void onManifestReply(bool ok, const QJsonObject &payload);
  void openDocument();
  void onOpenReply(bool ok, const QJsonObject &payload);
  void onEvent(const QString &type, const QJsonObject &payload);
  void onOpened(const QJsonObject &payload);
  // An unrecoverable engine.error that grants a restart: the engine discarded its session (engine protocol §12.1, §14.2).
  void recover(const SessionTracker::Restart &restart);
  // Forgets the session's document and requests; their replies and events are ignored from now on.
  void forgetSession(bool closesDocument);
  void stop(const QString &message);
  void setStatus(Status status);
  void setMessage(const QString &message);
  void setOffer(Offer offer);
  void setPages(const QJsonObject &payload);
  void setOverlay(const QJsonObject &payload);
  void onInvalidate(const QJsonObject &payload);
  void setAccessibilityTree(const QJsonObject &payload);
  void addAccessibilityPart(qint64 requestId, bool ok, const QJsonObject &payload);
  void applyAccessibilityUpdate(const QJsonObject &payload);
  // Asks for the whole tree again, when the session can no longer tell whether its tree matches the engine's.
  void reloadAccessibilityTree();
  // Asks for the children that nodes of the tree lack, a few ranges at a time (engine protocol §14.7 item 1).
  void loadAccessibilityParts();
  void sendView();
  qint64 sendForDocument(const QString &type, QJsonObject payload);
  // The engine broke the protocol, or stopped after using up its restarts: the session stops for good, with the stop message.
  void fail();
  void releaseDocumentBlob();

  Engine &engine_;
  SessionTracker tracker_;
  Status status_ = Status::Starting;
  bool updateRequired_ = false;
  QString message_;
  Offer offer_ = Offer::None;
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
  KeyBindings keyBindings_;
  qint64 documentId_ = 0;
  std::vector<protocol::Page> pages_;
  protocol::Overlay overlay_;
  std::optional<protocol::AccessibleTree> accessibilityTree_;
  std::optional<protocol::AccessibleCaret> accessibilityCaret_;
  View view_{.viewport = QRectF(0, 0, 1024, 768), .zoom = 1.0, .deviceScale = 1.0};
  std::optional<View> sentView_;
  // The placeholder document's bytes, held by the engine from doc.open until the engine has answered it.
  BlobId documentBlob_ = 0;
  // The current session's requests that the session waits for.
  qint64 helloRequest_ = 0;
  qint64 manifestRequest_ = 0;
  qint64 openRequest_ = 0;
  qint64 accessibilityRequest_ = 0;
  std::vector<qint64> viewRequests_;
  // The query.a11y requests for parts of the accessibility tree, and the children each asks for; and the ranges the engine refused, by node
  // and first child, which are not asked for again until the tree or the node changes.
  std::map<qint64, protocol::MissingChildren> accessibilityParts_;
  std::set<std::pair<quint64, quint32>> refusedParts_;
};

} // namespace bayan
