#include "documentsession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QLocale>
#include <QtCore/QSysInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>

#include <utility>

using namespace Qt::StringLiterals;

namespace bayan {
namespace {

// Page sizes larger than this (about 140 metres) are treated as a broken message rather than displayed.
constexpr qint64 kMaxPageSide = qint64{1} << 38;

QString themeName() { return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? u"dark"_s : u"light"_s; }

} // namespace

DocumentSession::DocumentSession(Engine &engine, QObject *parent) : QObject(parent), engine_(engine) {
  connect(&engine_, &Engine::replyReceived, this, &DocumentSession::onReply);
  connect(&engine_, &Engine::eventReceived, this, &DocumentSession::onEvent);
  connect(&engine_, &Engine::protocolError, this, &DocumentSession::fail);
}

DocumentSession::~DocumentSession() { releaseDocumentBlob(); }

void DocumentSession::start() {
  const QJsonObject shell{
      {u"name"_s, u"bayan-desktop"_s}, {u"version"_s, QCoreApplication::applicationVersion()}, {u"platform"_s, QSysInfo::productType()}};
  // The scaffold offers no clipboard, input-method or accessibility support yet (DESK-002 adds them).
  const QJsonObject capabilities{{u"clipboard_formats"_s, QJsonArray{}}, {u"ime"_s, false}, {u"accessibility"_s, false}};
  helloRequest_ = engine_.request(u"hello"_s, QJsonObject{{u"protocol_versions"_s, QJsonArray{Engine::kProtocolVersion}},
                                                          {u"shell"_s, shell},
                                                          {u"capabilities"_s, capabilities},
                                                          {u"locale"_s, QLocale().bcp47Name()},
                                                          {u"theme"_s, themeName()}});
  if (helloRequest_ == 0) {
    fail();
  }
}

QSizeF DocumentSession::pageSize() const {
  if (pages_.empty()) {
    return {};
  }
  return {static_cast<qreal>(pages_.front().width) / static_cast<qreal>(kBluPerDip),
          static_cast<qreal>(pages_.front().height) / static_cast<qreal>(kBluPerDip)};
}

std::optional<DocumentSession::Page> DocumentSession::page(int index) const {
  if (index < 0 || static_cast<std::size_t>(index) >= pages_.size()) {
    return std::nullopt;
  }
  return pages_.at(static_cast<std::size_t>(index));
}

void DocumentSession::onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject & /*error*/) {
  if (status_ == Status::Failed || (requestId != helloRequest_ && requestId != openRequest_ && requestId != viewRequest_)) {
    return;
  }
  if (!ok) {
    fail(); // also releases the placeholder blob if doc.open was refused
    return;
  }
  if (requestId == helloRequest_) {
    engineVersion_ = payload.value(u"engine_version"_s).toString();
    emit statusChanged();
    // Placeholder until opening and creating documents exist: an empty blob stands for a new blank document. The stub engine always opens
    // its test page.
    documentBlob_ = engine_.putBlob(QByteArray());
    openRequest_ = documentBlob_ == 0 ? 0 : engine_.request(u"doc.open"_s, QJsonObject{{u"blob"_s, static_cast<qint64>(documentBlob_)}});
    if (openRequest_ == 0) {
      fail();
    }
  } else if (requestId == openRequest_) {
    releaseDocumentBlob();
    documentId_ = payload.value(u"doc_id"_s).toInteger(0);
    if (documentId_ <= 0) {
      fail();
      return;
    }
    // The real viewport comes from the document canvas in DESK-002; until then the view is a fixed print-layout view at 100 %.
    const QJsonObject viewport{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 1024}, {u"height"_s, 768}};
    viewRequest_ = engine_.request(u"view.set"_s, QJsonObject{{u"doc_id"_s, documentId_},
                                                              {u"viewport"_s, viewport},
                                                              {u"zoom"_s, 1.0},
                                                              {u"device_scale"_s, 1.0},
                                                              {u"mode"_s, u"print"_s},
                                                              {u"show_formatting_marks"_s, false}});
    if (viewRequest_ == 0) {
      fail();
    }
  }
}

void DocumentSession::onEvent(const QString &type, const QJsonObject &payload) {
  if (type == u"view.pages"_s) {
    setPages(payload);
  } else if (type == u"render.invalidate"_s) {
    ++revision_;
    emit revisionChanged();
  } else if (type == u"engine.error"_s) {
    if (!payload.value(u"recoverable"_s).toBool(false)) {
      fail();
    }
  }
  // Other events are ignored, as the protocol requires for unknown ones (engine protocol §9).
}

void DocumentSession::setPages(const QJsonObject &payload) {
  if (documentId_ <= 0 || payload.value(u"doc_id"_s).toInteger(0) != documentId_) {
    return;
  }
  std::vector<Page> pages;
  const QJsonArray list = payload.value(u"pages"_s).toArray();
  pages.reserve(static_cast<std::size_t>(list.size()));
  for (const auto &value : list) {
    const QJsonObject object = value.toObject();
    const Page page{.width = object.value(u"width"_s).toInteger(0), .height = object.value(u"height"_s).toInteger(0)};
    if (page.width <= 0 || page.height <= 0 || page.width > kMaxPageSide || page.height > kMaxPageSide) {
      fail();
      return;
    }
    pages.push_back(page);
  }
  pages_ = std::move(pages);
  emit pagesChanged();
  if (status_ == Status::Starting && !pages_.empty()) {
    status_ = Status::Ready;
    emit statusChanged();
  }
}

void DocumentSession::fail() {
  releaseDocumentBlob();
  if (status_ != Status::Failed) {
    status_ = Status::Failed;
    emit statusChanged();
  }
}

void DocumentSession::releaseDocumentBlob() {
  if (documentBlob_ != 0) {
    engine_.releaseBlob(documentBlob_);
    documentBlob_ = 0;
  }
}

} // namespace bayan
