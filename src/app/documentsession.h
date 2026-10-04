// Drives the conversation with the engine for the one document the scaffold shows: the hello/welcome handshake, opening a document, setting
// the view, and keeping the list of pages. It contains no document logic: the engine decides everything, the session only relays.
// DESK-002 replaces the single page image with the real document canvas.

#pragma once

#include "engine.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QSizeF>
#include <QtCore/QString>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace bayan {

class DocumentSession final : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("DocumentSession is created by the application.")
  // QML refers to the status values as DocumentSession.Status.Ready and so on.
  Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")
  Q_PROPERTY(Status status READ status NOTIFY statusChanged FINAL)
  Q_PROPERTY(QString engineVersion READ engineVersion NOTIFY statusChanged FINAL)
  Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged FINAL)
  Q_PROPERTY(QSizeF pageSize READ pageSize NOTIFY pagesChanged FINAL)
  Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)

public:
  enum class Status : std::uint8_t { Starting, Ready, Failed };
  Q_ENUM(Status)

  // A page's size in BLU (ADR-0005).
  struct Page {
    qint64 width = 0;
    qint64 height = 0;
  };

  // Device-independent pixels are 1/96 inch, and 1 inch is 1,828,800 BLU.
  static constexpr qint64 kBluPerDip = 19'050;

  explicit DocumentSession(Engine &engine, QObject *parent = nullptr);

  // Starts the conversation: hello, then doc.open, then view.set.
  void start();

  [[nodiscard]] Status status() const { return status_; }
  [[nodiscard]] QString engineVersion() const { return engineVersion_; }
  [[nodiscard]] int pageCount() const { return static_cast<int>(pages_.size()); }
  // The size of the first page in device-independent pixels at 100 % zoom, or an empty size before the engine has laid out a page.
  [[nodiscard]] QSizeF pageSize() const;
  // Increases whenever the engine says that displayed pixels are out of date (render.invalidate).
  [[nodiscard]] int revision() const { return revision_; }

  [[nodiscard]] qint64 documentId() const { return documentId_; }
  [[nodiscard]] std::optional<Page> page(int index) const;
  [[nodiscard]] Engine &engine() const { return engine_; }

signals:
  void statusChanged();
  void pagesChanged();
  void revisionChanged();

private:
  void onReply(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  void onEvent(const QString &type, const QJsonObject &payload);
  void setPages(const QJsonObject &payload);
  void fail();

  Engine &engine_;
  Status status_ = Status::Starting;
  QString engineVersion_;
  qint64 documentId_ = 0;
  std::vector<Page> pages_;
  int revision_ = 0;
  BlobId documentBlob_ = 0;
  qint64 helloRequest_ = 0;
  qint64 openRequest_ = 0;
  qint64 viewRequest_ = 0;
};

} // namespace bayan
