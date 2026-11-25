#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QLocale>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// How many query.a11y requests for parts of the accessibility tree may wait for their answers at once; the rest follow as answers arrive.
constexpr std::size_t kMaxPartRequests = 8;
// The largest document identifier: below 2^53, like every identifier of the protocol (engine protocol §4).
constexpr qint64 kMaxDocumentId = static_cast<qint64>(quint64{1} << 53U) - 1;

QString themeName() { return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? u"dark"_s : u"light"_s; }

QJsonObject toJson(const QRectF &rect) {
  return QJsonObject{{u"x"_s, rect.x()}, {u"y"_s, rect.y()}, {u"width"_s, rect.width()}, {u"height"_s, rect.height()}};
}

// The document identifier of a payload: a positive integer below 2^53, or 0 if it is not one.
qint64 documentIdOf(const QJsonObject &payload) {
  const QJsonValue value = payload.value(u"doc_id"_s);
  const double number = value.toDouble(0);
  if (!value.isDouble() || !std::isfinite(number) || std::trunc(number) != number || number < 1 ||
      number > static_cast<double>(kMaxDocumentId)) {
    return 0;
  }
  return value.toInteger();
}

// The stop message, after which the session sends the engine nothing more: the same after the engine stopped once its restarts were used
// up, after it broke the protocol and after it refused the handshake (engine protocol §12.1 step 2, §14.1 items 3 and 5).
QString stopMessage() { return DocumentSession::tr("The engine stopped. Close BayanDocs and open it again to restart the engine."); }

} // namespace

DocumentSession::OperatingSystem DocumentSession::operatingSystem() {
#ifdef Q_OS_WIN
  return OperatingSystem::Windows;
#elif defined(Q_OS_MACOS)
  return OperatingSystem::MacOS;
#elif defined(Q_OS_IOS)
  return OperatingSystem::IOS;
#elif defined(Q_OS_ANDROID)
  return OperatingSystem::Android;
#elif defined(Q_OS_LINUX)
  return OperatingSystem::Linux;
#else
  return OperatingSystem::Other;
#endif
}

QString DocumentSession::platformName(OperatingSystem system) {
  switch (system) {
  case OperatingSystem::Windows:
    return u"windows"_s;
  case OperatingSystem::MacOS:
    return u"macos"_s;
  case OperatingSystem::Linux:
    return u"linux"_s;
  case OperatingSystem::IOS:
    return u"ios"_s;
  case OperatingSystem::Android:
    return u"android"_s;
  case OperatingSystem::ChromeOS:
    return u"chromeos"_s;
  case OperatingSystem::Other:
    break;
  }
  return u"other"_s;
}

DocumentSession::DocumentSession(Engine &engine, QObject *parent) : QObject(parent), engine_(engine) {
  connect(&engine_, &Engine::replyReceived, this, &DocumentSession::onReply);
  connect(&engine_, &Engine::eventReceived, this, &DocumentSession::onEvent);
  connect(&engine_, &Engine::protocolError, this, &DocumentSession::fail);
}

DocumentSession::~DocumentSession() { releaseDocumentBlob(); }

void DocumentSession::start() {
  openWhenWelcomed_ = true;
  startSession();
}

qint64 DocumentSession::request(const QString &type, const QJsonObject &payload) {
  const qint64 id = engine_.request(type, payload);
  if (id > 0) {
    tracker_.sent(type, id);
  }
  return id;
}

void DocumentSession::startSession() {
  // The operating system whose keyboard conventions apply, from Qt's knowledge of the system the app was built for, not its free-text
  // product name, and the runtime: the engine uses them only to resolve the manifest's key bindings (engine protocol §14.1 item 2).
  const QJsonObject shell{
      {u"name"_s, u"bayan-desktop"_s},
      {u"version"_s, QCoreApplication::applicationVersion()},
      {u"platform"_s, platformName(operatingSystem())},
      {u"runtime"_s, u"native"_s},
  };
  // The canvas forwards input-method composition and bridges the accessibility tree to the platform (DESK-002); no clipboard yet.
  const QJsonObject capabilities{{u"clipboard_formats"_s, QJsonArray{}}, {u"ime"_s, true}, {u"accessibility"_s, true}};
  welcomed_ = false;
  helloRequest_ = request(u"hello"_s, QJsonObject{
                                          {u"protocol_versions"_s, QJsonArray{Engine::kProtocolVersion}},
                                          {u"shell"_s, shell},
                                          {u"capabilities"_s, capabilities},
                                          {u"locale"_s, QLocale().bcp47Name()},
                                          {u"theme"_s, themeName()},
                                      });
  if (helloRequest_ == 0) {
    fail();
  }
}

void DocumentSession::reopen() {
  if (status_ == Status::Failed || offer_ == Offer::None) {
    return;
  }
  taken_ = offer_;
  setOffer(Offer::None);
  setStatus(Status::Starting);
  setMessage(taken_ == Offer::Reopen ? tr("Reopening the document…") : tr("Opening the document…"));
  if (welcomed_) {
    openDocument();
  } else {
    // The new session's welcome has not arrived yet; doc.open follows it.
    openWhenWelcomed_ = true;
  }
}

void DocumentSession::onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error) {
  // Nothing after the stop.
  if (status_ == Status::Failed) {
    return;
  }
  // The error panic in reply to doc.open: the engine is done with the document's bytes, and the document counts as being opened until the
  // engine.error that follows closes it (engine protocol §12.1, point 3). The code is read only to recognise the reply; nothing of the
  // error is shown.
  if (!ok && requestId == openRequest_ && error.value(u"code"_s).toString() == u"panic"_s) {
    openRequest_ = 0;
    releaseDocumentBlob();
  }
  if (tracker_.reply(requestId, ok, payload, error).action != SessionTracker::Action::Handle) {
    return;
  }
  if (requestId == helloRequest_) {
    onHelloReply(ok, payload, error);
  } else if (requestId == manifestRequest_) {
    manifestRequest_ = 0;
    onManifestReply(ok, payload);
  } else if (requestId == openRequest_) {
    onOpenReply(ok, payload);
  } else if (const auto view = std::ranges::find(viewRequests_, requestId); view != viewRequests_.end()) {
    viewRequests_.erase(view);
    // A view the engine refused breaks nothing (engine protocol §14.1 item 4): the canvas keeps showing what it has.
  } else if (requestId == accessibilityRequest_) {
    accessibilityRequest_ = 0;
    if (ok) {
      setAccessibilityTree(payload);
    } else {
      // An engine without an accessibility tree (the stub engine answers unsupported_message): the canvas has no text to expose.
      accessibilityTree_.reset();
      accessibilityCaret_.reset();
      emit accessibilityChanged({});
    }
  } else if (accessibilityParts_.contains(requestId)) {
    addAccessibilityPart(requestId, ok, payload);
  } else if (!ok) {
    emit inputRefused(error.value(u"code"_s).toString());
  }
}

void DocumentSession::onHelloReply(bool ok, const QJsonObject &payload, const QJsonObject &error) {
  if (!ok) {
    // A refused hello is not a failure that a new session repairs, so the session stops without another hello (engine protocol §14.1
    // item 3). unsupported_protocol_version is the one refusal the user can fix: the app and its engine do not fit together, so the
    // installation is incomplete and must be updated or reinstalled. There is no update action yet (DESK-104).
    forgetSession(documentId_ > 0 || opening_);
    if (error.value(u"code"_s).toString() == u"unsupported_protocol_version"_s) {
      updateRequired_ = true;
      stop(tr("This installation of BayanDocs is incomplete: the app and its document engine do not fit together. Update BayanDocs, or "
              "install it again."));
    } else {
      stop(stopMessage());
    }
    return;
  }
  // The session's version is the one the shell offered; a welcome that names another breaks the protocol (§14.1 item 3).
  const auto welcome = protocol::parseWelcome(payload);
  if (!welcome || welcome->protocolVersion != Engine::kProtocolVersion) {
    fail();
    return;
  }
  welcomed_ = true;
  engineVersion_ = welcome->engineVersion;
  features_ = welcome->features;
  emit statusChanged();
  // The key data that decide which presses reach the engine (§14.3); the engine resolves them for the platform and runtime of hello.
  manifestRequest_ = request(u"ui.manifest"_s, QJsonObject{{u"locale"_s, QLocale().bcp47Name()}});
  if (manifestRequest_ == 0) {
    fail();
    return;
  }
  if (openWhenWelcomed_) {
    openDocument();
  }
}

void DocumentSession::onManifestReply(bool ok, const QJsonObject &payload) {
  if (!ok) {
    // An error in this reply ends nothing (engine protocol §14.1 item 4): without key data no press goes to the engine, and typed text
    // still does.
    return;
  }
  auto manifest = protocol::parseUiManifest(payload);
  if (!manifest) {
    fail();
    return;
  }
  keyBindings_ = KeyBindings(std::move(*manifest));
  emit keyBindingsChanged();
}

void DocumentSession::onOpenReply(bool ok, const QJsonObject &payload) {
  openRequest_ = 0;
  opening_ = false;
  releaseDocumentBlob();
  if (!ok) {
    // The engine refused the document and kept its session: there is nothing to restart, and nothing is offered.
    taken_ = Offer::None;
    setStatus(Status::Closed);
    setMessage(tr("The document could not be opened."));
    return;
  }
  onOpened(payload);
}

void DocumentSession::openDocument() {
  openWhenWelcomed_ = false;
  // Placeholder until opening and creating documents exist: an empty blob stands for a new blank document. The engine opens its mock
  // document whatever the blob holds, and the stub engine its test page.
  documentBlob_ = engine_.putBlob(QByteArray());
  openRequest_ = documentBlob_ == 0 ? 0 : request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(documentBlob_)}});
  if (openRequest_ == 0) {
    fail();
    return;
  }
  opening_ = true;
  setStatus(Status::Starting);
}

void DocumentSession::onOpened(const QJsonObject &payload) {
  // doc.opened's required fields (engine protocol §6.1, §14.6): the document, its render revision, and the estimate, the warnings and the
  // fonts, which the shell does not show yet.
  documentId_ = documentIdOf(payload);
  if (documentId_ <= 0 || !protocol::parseRevision(payload.value(u"revision"_s)) || !payload.value(u"page_count_estimate"_s).isDouble() ||
      !payload.value(u"warnings"_s).isArray() || !payload.value(u"fonts"_s).isObject()) {
    fail();
    return;
  }
  sendView();
  accessibilityRequest_ = request(u"query.a11y"_s, QJsonObject{{u"doc_id"_s, documentId_}});
  if (accessibilityRequest_ == 0) {
    fail();
  }
}

void DocumentSession::setView(const View &view) {
  view_ = view;
  if (documentId_ > 0 && status_ != Status::Failed) {
    sendView();
  }
}

void DocumentSession::sendView() {
  if (sentView_ == view_) {
    return;
  }
  // In print mode a new viewport, zoom or device scale invalidates nothing (engine protocol §14.6 item 2): tiles are requested for a scale,
  // and the canvas requests those of a new scale by itself.
  const qint64 id = request(u"view.set"_s, QJsonObject{
                                               {u"doc_id"_s, documentId_},
                                               {u"viewport"_s, toJson(view_.viewport)},
                                               {u"zoom"_s, view_.zoom},
                                               {u"device_scale"_s, view_.deviceScale},
                                               {u"mode"_s, u"print"_s},
                                               {u"show_formatting_marks"_s, false},
                                           });
  if (id == 0) {
    fail();
    return;
  }
  viewRequests_.push_back(id);
  sentView_ = view_;
}

void DocumentSession::onEvent(const QString &type, const QJsonObject &payload) {
  if (status_ == Status::Failed) {
    return;
  }
  const SessionTracker::Decision decision = tracker_.event(type, payload);
  switch (decision.action) {
  case SessionTracker::Action::Ignore:
    return;
  case SessionTracker::Action::Stop:
    fail();
    return;
  case SessionTracker::Action::Restart:
    recover(decision.restart);
    return;
  case SessionTracker::Action::Handle:
    break;
  }
  if (type == u"engine.error"_s) {
    // An error the engine recovered from, such as the refusal of a message without an identifier, ends nothing (§14.1 item 4). Its fields
    // must still have their types.
    if (!payload.value(u"code"_s).isString() || !payload.value(u"message_id"_s).isString() || !payload.value(u"args"_s).isObject()) {
      fail();
    }
    return;
  }
  // The other events the session reads concern the open document; the tracker has ignored those about documents of earlier sessions. Other
  // events (view.layout.progress, and any the engine adds later) are ignored, as the protocol requires for unknown ones (§9).
  const bool read = type == u"view.pages"_s || type == u"render.invalidate"_s || type == u"overlay.update"_s || type == u"a11y.update"_s;
  if (!read) {
    return;
  }
  const qint64 document = documentIdOf(payload);
  if (document == 0) {
    // A doc_id that is no document identifier breaks the protocol.
    fail();
    return;
  }
  if (document != documentId_) {
    return;
  }
  if (type == u"view.pages"_s) {
    setPages(payload);
  } else if (type == u"render.invalidate"_s) {
    onInvalidate(payload);
  } else if (type == u"overlay.update"_s) {
    setOverlay(payload);
  } else {
    applyAccessibilityUpdate(payload);
  }
}

void DocumentSession::onInvalidate(const QJsonObject &payload) {
  const auto revision = protocol::parseRevision(payload.value(u"revision"_s));
  const QJsonValue list = payload.value(u"regions"_s);
  const auto regions = protocol::parseRegions(list.toArray(), pageCount());
  if (!revision || !regions || !list.isArray()) {
    fail();
    return;
  }
  emit invalidated(*regions, *revision);
}

void DocumentSession::setPages(const QJsonObject &payload) {
  const QJsonValue list = payload.value(u"pages"_s);
  auto pages = protocol::parsePages(list.toArray());
  if (!pages || !list.isArray()) {
    fail();
    return;
  }
  if (*pages != pages_) {
    pages_ = std::move(*pages);
    emit pagesChanged();
  }
  if (status_ == Status::Starting && !pages_.empty()) {
    setStatus(Status::Ready);
    if (taken_ != Offer::None) {
      setMessage(taken_ == Offer::Reopen ? tr("The document was reopened.") : tr("The document was opened."));
      taken_ = Offer::None;
    }
  }
}

void DocumentSession::setOverlay(const QJsonObject &payload) {
  auto overlay = protocol::parseOverlay(payload, pageCount());
  if (!overlay) {
    fail();
    return;
  }
  if (*overlay != overlay_) {
    overlay_ = std::move(*overlay);
    emit overlayChanged();
  }
}

void DocumentSession::setAccessibilityTree(const QJsonObject &payload) {
  // A tree beyond the protocol's limits, or one the session cannot read, breaks the protocol (engine protocol §14.7 item 1).
  auto tree = protocol::parseAccessibleTree(payload.value(u"root"_s), pageCount());
  const auto caret = protocol::parseAccessibleCaret(payload.value(u"caret"_s));
  if (documentIdOf(payload) != documentId_ || !tree || !caret) {
    fail();
    return;
  }
  accessibilityTree_ = std::move(*tree);
  accessibilityCaret_ = *caret;
  accessibilityParts_.clear();
  refusedParts_.clear();
  emit accessibilityChanged({});
  loadAccessibilityParts();
}

void DocumentSession::addAccessibilityPart(qint64 requestId, bool ok, const QJsonObject &payload) {
  const protocol::MissingChildren asked = accessibilityParts_.extract(requestId).mapped();
  const protocol::AccessibleNode *node = accessibilityTree_ ? accessibilityTree_->find(asked.node) : nullptr;
  if (!ok) {
    // An error here ends nothing (engine protocol §14.1 item 4). The range is not asked for again until the tree or the node changes, so
    // that a refusal cannot make the session ask for ever; screen readers lack those children meanwhile.
    refusedParts_.emplace(asked.node, asked.start);
    loadAccessibilityParts();
    return;
  }
  if (!accessibilityTree_ || node == nullptr) {
    // An a11y.update has replaced the node meanwhile, and the session asked for that one's parts anew if it lacks any.
    loadAccessibilityParts();
    return;
  }
  protocol::AccessibleTree &tree = *accessibilityTree_;
  auto part = protocol::parseAccessibleTree(payload.value(u"root"_s), pageCount(), tree.levelOf(asked.node));
  const auto caret = protocol::parseAccessibleCaret(payload.value(u"caret"_s));
  // The answer holds the child at the range's start whenever the node has one (engine protocol §14.7 item 1); one that does not would
  // leave the same range missing, and the session would ask for it for ever.
  if (documentIdOf(payload) != documentId_ || !part || !caret || part->root().id != asked.node ||
      !protocol::holdsChildFrom(part->root(), asked.start)) {
    fail();
    return;
  }
  // The engine answers in order with its a11y.update events, which the session has applied, so a part that does not fit the tree (an
  // identifier that the tree holds elsewhere) breaks the protocol too; reloading the tree would only bring the same part back.
  if (!tree.addChildren(*part)) {
    fail();
    return;
  }
  accessibilityCaret_ = *caret;
  emit accessibilityChanged({asked.node});
  loadAccessibilityParts();
}

void DocumentSession::loadAccessibilityParts() {
  if (!accessibilityTree_ || accessibilityRequest_ != 0) {
    return;
  }
  for (const protocol::MissingChildren &missing : accessibilityTree_->missingChildren()) {
    if (accessibilityParts_.size() >= kMaxPartRequests) {
      return;
    }
    // A range already asked for is not asked for again; its answer brings as many children as fit, and the rest are asked for then.
    const bool asked = std::ranges::any_of(accessibilityParts_, [&missing](const auto &part) {
      return part.second.node == missing.node && part.second.start == missing.start;
    });
    if (asked || refusedParts_.contains({missing.node, missing.start})) {
      continue;
    }
    const qint64 id =
        request(u"query.a11y"_s, QJsonObject{
                                     {u"doc_id"_s, documentId_},
                                     {u"node"_s, static_cast<qint64>(missing.node)},
                                     {u"children"_s, QJsonObject{{u"start"_s, qint64{missing.start}}, {u"count"_s, qint64{missing.count}}}},
                                 });
    if (id == 0) {
      fail();
      return;
    }
    accessibilityParts_.emplace(id, missing);
  }
}

void DocumentSession::reloadAccessibilityTree() {
  accessibilityTree_.reset();
  accessibilityCaret_.reset();
  accessibilityParts_.clear();
  refusedParts_.clear();
  accessibilityRequest_ = request(u"query.a11y"_s, QJsonObject{{u"doc_id"_s, documentId_}});
  if (accessibilityRequest_ == 0) {
    fail();
    return;
  }
  emit accessibilityChanged({});
}

void DocumentSession::applyAccessibilityUpdate(const QJsonObject &payload) {
  // Every change is checked before anything uses it, also while the session waits for the tree or for parts of it (engine protocol §14.1
  // item 5): a change is an object with a kind, and a node that is an object with an identifier in the protocol's range.
  const QJsonValue changes = payload.value(u"changes"_s);
  const auto caret = protocol::parseAccessibleCaret(payload.value(u"caret"_s));
  if (!changes.isArray() || !caret) {
    fail();
    return;
  }
  struct Change {
    bool replace = false;
    quint64 id = 0;
    QJsonValue node;
  };
  std::vector<Change> checked;
  for (const auto &change : changes.toArray()) {
    const QJsonObject object = change.toObject();
    const QJsonValue node = object.value(u"node"_s);
    const auto id = protocol::parseNodeId(node);
    if (!change.isObject() || !object.value(u"kind"_s).isString() || !id) {
      fail();
      return;
    }
    checked.push_back({.replace = object.value(u"kind"_s).toString() == u"replace"_s, .id = *id, .node = node});
  }
  if (!accessibilityTree_ || accessibilityRequest_ != 0) {
    // No tree yet, or a fresh one is on its way and will include these changes.
    return;
  }
  QList<quint64> changed;
  for (const Change &change : checked) {
    if (!change.replace) {
      // A kind of change this shell does not know: its tree can no longer be trusted to match the engine's.
      reloadAccessibilityTree();
      return;
    }
    // The node's level follows its parents in the tree, so each change costs the tree's depth, not its size.
    const int level = accessibilityTree_->levelOf(change.id);
    // A change to a node makes its parts worth asking for again.
    std::erase_if(refusedParts_, [&change](const auto &refused) { return refused.first == change.id; });
    if (level == 0) {
      // A node that the tree does not hold: one in a part still being loaded, whose answer will hold it as it is now, or one that the
      // session's tree lacks for another reason, and then the whole tree is asked for again.
      if (accessibilityParts_.empty()) {
        reloadAccessibilityTree();
        return;
      }
      continue;
    }
    // Each change is within the limits too, counted from the replaced node's level (engine protocol §14.7 item 1).
    const auto subtree = protocol::parseAccessibleTree(change.node, pageCount(), level);
    if (!subtree) {
      fail();
      return;
    }
    if (!accessibilityTree_->replace(*subtree)) {
      reloadAccessibilityTree();
      return;
    }
    changed.append(subtree->root().id);
  }
  accessibilityCaret_ = *caret;
  emit accessibilityChanged(changed);
  loadAccessibilityParts();
}

qint64 DocumentSession::sendForDocument(const QString &type, QJsonObject payload) {
  if (status_ == Status::Failed || documentId_ <= 0) {
    return 0;
  }
  payload.insert(u"doc_id"_s, documentId_);
  return request(type, payload);
}

qint64 DocumentSession::sendKey(const protocol::KeyInput &key) { return sendForDocument(u"input.key"_s, protocol::keyPayload(key)); }

qint64 DocumentSession::sendText(const QString &text) { return sendForDocument(u"input.text"_s, QJsonObject{{u"text"_s, text}}); }

qint64 DocumentSession::sendComposition(protocol::CompositionPhase phase, const QString &text, int selectionStart, int selectionEnd) {
  return sendForDocument(u"input.composition"_s, protocol::compositionPayload(phase, text, selectionStart, selectionEnd));
}

qint64 DocumentSession::sendPointer(const protocol::PointerInput &pointer) {
  return sendForDocument(u"input.pointer"_s, protocol::pointerPayload(pointer));
}

void DocumentSession::recover(const SessionTracker::Restart &restart) {
  if (helloRequest_ == 0) {
    // Never started: there is no session to recover.
    return;
  }
  // What the failure closed decides the message and the offer (engine protocol §12.1, points 2 and 3): the document that was open or being
  // opened can be reopened, as can one a failure closed earlier while it is still closed (the offer stays, also when the user took it but
  // its doc.open has not gone out yet); otherwise the document was never open, and can be opened now.
  const bool closed = documentId_ > 0 || opening_;
  const Offer pending = offer_ != Offer::None ? offer_ : taken_;
  const Offer offer = closed || pending == Offer::Reopen ? Offer::Reopen : Offer::Open;
  forgetSession(closed);
  QString documentState;
  if (closed) {
    documentState = tr("The document was closed and can be reopened.");
  } else if (offer == Offer::Reopen) {
    documentState = tr("The document is still closed and can be reopened.");
  } else {
    documentState = tr("The document can be opened now.");
  }
  setOffer(offer);
  setStatus(Status::Closed);
  // Which restart this is comes from the engine, whose binding counts them (§14.2); "1 of 3", so that a screen reader announces a second
  // restart too instead of skipping words it has just read.
  setMessage(tr("The engine stopped working and was restarted (%1 of %2). %3")
                 .arg(QString::number(restart.number), QString::number(restart.limit), documentState));
  // A new session, without the document: doc.open waits for the user (§12.1, point 3).
  startSession();
}

void DocumentSession::forgetSession(bool closesDocument) {
  releaseDocumentBlob();
  welcomed_ = false;
  openWhenWelcomed_ = false;
  opening_ = false;
  taken_ = Offer::None;
  documentId_ = 0;
  manifestRequest_ = 0;
  openRequest_ = 0;
  accessibilityRequest_ = 0;
  accessibilityParts_.clear();
  refusedParts_.clear();
  viewRequests_.clear();
  // The next document gets its own view.set.
  sentView_.reset();
  // Only now that the document is forgotten, so that nothing more can be sent for it: the interface then moves keyboard focus off the
  // canvas, which ends an input-method composition in progress, and that must not reach the engine that failed (engine protocol §12.1,
  // point 1). Still before the pages and the status change, so that the canvas is visible while the focus moves.
  if (closesDocument) {
    emit documentClosed();
  }
  if (!pages_.empty()) {
    pages_.clear();
    emit pagesChanged();
  }
  if (overlay_ != protocol::Overlay{}) {
    overlay_ = {};
    emit overlayChanged();
  }
  if (accessibilityTree_ || accessibilityCaret_) {
    accessibilityTree_.reset();
    accessibilityCaret_.reset();
    emit accessibilityChanged({});
  }
}

void DocumentSession::fail() {
  if (status_ == Status::Failed) {
    return;
  }
  forgetSession(documentId_ > 0 || opening_);
  stop(stopMessage());
}

void DocumentSession::stop(const QString &message) {
  // The offer goes at the stop (engine protocol §12.1, point 3), and nothing more is sent to the engine.
  setOffer(Offer::None);
  setStatus(Status::Failed);
  setMessage(message);
}

void DocumentSession::setStatus(Status status) {
  if (status != status_) {
    status_ = status;
    emit statusChanged();
  }
}

void DocumentSession::setMessage(const QString &message) {
  message_ = message;
  emit messageChanged();
}

void DocumentSession::setOffer(Offer offer) {
  if (offer != offer_) {
    offer_ = offer;
    emit offerChanged();
  }
}

void DocumentSession::releaseDocumentBlob() {
  if (documentBlob_ != 0) {
    engine_.releaseBlob(documentBlob_);
    documentBlob_ = 0;
  }
}

} // namespace bayan
