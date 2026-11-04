#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QLocale>
#include <QtCore/QSysInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

QString themeName() { return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? u"dark"_s : u"light"_s; }

QJsonObject toJson(const QRectF &rect) {
  return QJsonObject{{u"x"_s, rect.x()}, {u"y"_s, rect.y()}, {u"width"_s, rect.width()}, {u"height"_s, rect.height()}};
}

} // namespace

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

void DocumentSession::startSession() {
  const QJsonObject shell{
      {u"name"_s, u"bayan-desktop"_s}, {u"version"_s, QCoreApplication::applicationVersion()}, {u"platform"_s, QSysInfo::productType()}};
  // The canvas forwards input-method composition and bridges the accessibility tree to the platform (DESK-002); no clipboard yet.
  const QJsonObject capabilities{{u"clipboard_formats"_s, QJsonArray{}}, {u"ime"_s, true}, {u"accessibility"_s, true}};
  welcomed_ = false;
  helloRequest_ = engine_.request(u"hello"_s, QJsonObject{{u"protocol_versions"_s, QJsonArray{Engine::kProtocolVersion}},
                                                          {u"shell"_s, shell},
                                                          {u"capabilities"_s, capabilities},
                                                          {u"locale"_s, QLocale().bcp47Name()},
                                                          {u"theme"_s, themeName()}});
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

qint64 DocumentSession::currentCause() const {
  // The v0 engine answers every request before it sends the events that handling it caused (see the file comment of documentsession.h: the
  // protocol states it only for view.set), and every message the session sends is a request. So an event belongs to the request whose reply
  // arrived last. Before any reply, assume the worst: the last request posted.
  return lastReply_ > 0 ? lastReply_ : engine_.lastPostedRequest();
}

void DocumentSession::onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error) {
  lastReply_ = requestId;
  // Nothing after the stop, and nothing for an earlier session: a request sent before this session's hello was answered by the session the
  // engine discarded, or with handshake_required by this one before its handshake (engine protocol §12.1, point 4).
  if (status_ == Status::Failed || requestId < helloRequest_) {
    return;
  }
  // The engine panicked while handling the request. It has discarded its session, and the engine.error that follows decides what happens
  // (§12.1, point 4), so the reply itself changes nothing, except that the engine is done with the document's bytes; a document whose
  // doc.open it answers still counts as being opened until the engine.error closes it (§12.1, point 3). The code is read only to recognise
  // the reply; nothing of the error is shown.
  if (!ok && error.value(u"code"_s).toString() == u"panic"_s) {
    if (requestId == openRequest_) {
      openRequest_ = 0;
      releaseDocumentBlob();
    }
    return;
  }
  if (requestId == helloRequest_) {
    if (ok) {
      onWelcome(payload);
    } else {
      fail();
    }
  } else if (requestId == openRequest_) {
    onOpenReply(ok, payload);
  } else if (const auto view = std::ranges::find(viewRequests_, requestId); view != viewRequests_.end()) {
    viewRequests_.erase(view);
    if (!ok) {
      fail();
    }
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
  } else if (!ok) {
    emit inputRefused(error.value(u"code"_s).toString());
  }
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

void DocumentSession::onWelcome(const QJsonObject &payload) {
  welcomed_ = true;
  engineVersion_ = payload.value(u"engine_version"_s).toString();
  features_.clear();
  for (const auto &feature : payload.value(u"features"_s).toArray()) {
    features_.append(feature.toString());
  }
  emit statusChanged();
  if (openWhenWelcomed_) {
    openDocument();
  }
}

void DocumentSession::openDocument() {
  openWhenWelcomed_ = false;
  // Placeholder until opening and creating documents exist: an empty blob stands for a new blank document. The v0 engine opens its mock
  // document whatever the blob holds, and the stub engine its test page.
  documentBlob_ = engine_.putBlob(QByteArray());
  openRequest_ = documentBlob_ == 0 ? 0 : engine_.request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(documentBlob_)}});
  if (openRequest_ == 0) {
    fail();
    return;
  }
  opening_ = true;
  setStatus(Status::Starting);
}

void DocumentSession::onOpened(const QJsonObject &payload) {
  documentId_ = payload.value(u"doc_id"_s).toInteger(0);
  if (documentId_ <= 0) {
    fail();
    return;
  }
  sendView();
  accessibilityRequest_ = engine_.request(u"query.a11y"_s, QJsonObject{{u"doc_id"_s, documentId_}});
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
  const qint64 request = engine_.request(u"view.set"_s, QJsonObject{{u"doc_id"_s, documentId_},
                                                                    {u"viewport"_s, toJson(view_.viewport)},
                                                                    {u"zoom"_s, view_.zoom},
                                                                    {u"device_scale"_s, view_.deviceScale},
                                                                    {u"mode"_s, u"print"_s},
                                                                    {u"show_formatting_marks"_s, false}});
  if (request == 0) {
    fail();
    return;
  }
  viewRequests_.push_back(request);
  sentView_ = view_;
}

void DocumentSession::onEvent(const QString &type, const QJsonObject &payload) {
  if (type == u"engine.error"_s) {
    // Only recoverable decides, and anything but true means that the engine discarded its session (engine protocol §12.1).
    if (!payload.value(u"recoverable"_s).toBool(false)) {
      recover();
    }
    return;
  }
  // Every other event the session handles concerns the open document.
  if (status_ == Status::Failed || documentId_ <= 0 || payload.value(u"doc_id"_s).toInteger(0) != documentId_) {
    return;
  }
  if (type == u"view.pages"_s) {
    setPages(payload);
  } else if (type == u"render.invalidate"_s) {
    const auto regions = protocol::parseRegions(payload.value(u"regions"_s).toArray(), pageCount());
    if (!regions || !payload.value(u"regions"_s).isArray()) {
      fail();
      return;
    }
    emit invalidated(*regions, currentCause());
  } else if (type == u"overlay.update"_s) {
    setOverlay(payload);
  } else if (type == u"a11y.update"_s) {
    applyAccessibilityUpdate(payload);
  }
  // Other events (view.layout.progress, and any the engine adds later) are ignored, as the protocol requires for unknown ones (§9).
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
  auto tree = protocol::parseAccessibleTree(payload.value(u"root"_s), pageCount());
  const auto caret = protocol::parseAccessibleCaret(payload.value(u"caret"_s));
  if (payload.value(u"doc_id"_s).toInteger(0) != documentId_ || !tree || !caret) {
    // A tree the shell cannot read is dropped rather than half-trusted; the document stays usable without it.
    accessibilityTree_.reset();
    accessibilityCaret_.reset();
  } else {
    accessibilityTree_ = std::move(*tree);
    accessibilityCaret_ = *caret;
  }
  emit accessibilityChanged({});
}

void DocumentSession::applyAccessibilityUpdate(const QJsonObject &payload) {
  if (!accessibilityTree_ || accessibilityRequest_ != 0) {
    // No tree yet, or a fresh one is on its way and will include this change.
    return;
  }
  QList<quint64> changed;
  bool understood = payload.value(u"changes"_s).isArray();
  for (const auto &change : payload.value(u"changes"_s).toArray()) {
    const QJsonObject object = change.toObject();
    const auto subtree = protocol::parseAccessibleTree(object.value(u"node"_s), pageCount());
    if (object.value(u"kind"_s).toString() != u"replace"_s || !subtree || !accessibilityTree_->replace(*subtree)) {
      understood = false;
      break;
    }
    changed.append(subtree->root().id);
  }
  const auto caret = protocol::parseAccessibleCaret(payload.value(u"caret"_s));
  if (!understood || !caret) {
    // The tree can no longer be trusted to match the engine's: ask for all of it again.
    accessibilityTree_.reset();
    accessibilityCaret_.reset();
    accessibilityRequest_ = engine_.request(u"query.a11y"_s, QJsonObject{{u"doc_id"_s, documentId_}});
    emit accessibilityChanged({});
    return;
  }
  accessibilityCaret_ = *caret;
  emit accessibilityChanged(changed);
}

qint64 DocumentSession::sendForDocument(const QString &type, QJsonObject payload) {
  if (status_ == Status::Failed || documentId_ <= 0) {
    return 0;
  }
  payload.insert(u"doc_id"_s, documentId_);
  return engine_.request(type, payload);
}

qint64 DocumentSession::sendKey(const protocol::KeyInput &key) { return sendForDocument(u"input.key"_s, protocol::keyPayload(key)); }

qint64 DocumentSession::sendText(const QString &text) { return sendForDocument(u"input.text"_s, QJsonObject{{u"text"_s, text}}); }

qint64 DocumentSession::sendComposition(protocol::CompositionPhase phase, const QString &text, int selectionStart, int selectionEnd) {
  return sendForDocument(u"input.composition"_s, protocol::compositionPayload(phase, text, selectionStart, selectionEnd));
}

qint64 DocumentSession::sendPointer(const protocol::PointerInput &pointer) {
  return sendForDocument(u"input.pointer"_s, protocol::pointerPayload(pointer));
}

void DocumentSession::recover() {
  if (status_ == Status::Failed || helloRequest_ == 0) {
    // Stopped already, or never started: there is no session to recover.
    return;
  }
  // What the failure closed decides the message and the offer (engine protocol §12.1, points 2 and 3): the document that was open or being
  // opened can be reopened, as can one a failure closed earlier while it is still closed (the offer stays, also when the user took it but
  // its doc.open has not gone out yet); otherwise the document was never open, and can be opened now.
  const bool closed = documentId_ > 0 || opening_;
  const Offer pending = offer_ != Offer::None ? offer_ : taken_;
  const Offer offer = closed || pending == Offer::Reopen ? Offer::Reopen : Offer::Open;
  forgetSession(closed);
  const std::optional<int> restart = restarts_.take();
  if (!restart) {
    stop(tr("The engine stopped working again, so it was not restarted. Close BayanDocs and open it again to restart the engine."));
    return;
  }
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
  setMessage(tr("The engine stopped working and was restarted (%1 of %2). %3")
                 .arg(QString::number(*restart), QString::number(RestartAllowance::kMaxRestarts), documentState));
  // A new session, without the document: doc.open waits for the user (§12.1, point 3).
  startSession();
}

void DocumentSession::forgetSession(bool closesDocument) {
  if (closesDocument) {
    emit documentClosed();
  }
  releaseDocumentBlob();
  welcomed_ = false;
  openWhenWelcomed_ = false;
  opening_ = false;
  taken_ = Offer::None;
  documentId_ = 0;
  openRequest_ = 0;
  accessibilityRequest_ = 0;
  viewRequests_.clear();
  // The next document gets its own view.set.
  sentView_.reset();
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
  stop(tr("The engine stopped. Close BayanDocs and open it again to restart the engine."));
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
