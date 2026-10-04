// The desktop shell's only door to the engine: a C++ wrapper over the C interface in bayan_ffi.h (engine protocol §3, ADR-0012).
//
// - It owns the engine handle (RAII: destroying the Engine destroys the engine, and no callback can arrive afterwards).
// - It sends protocol messages as JSON and turns the engine's replies and events into Qt signals.
// - The engine calls back on its own thread; the wrapper copies each message and hands it to the thread that owns the Engine object
//   (normally the main thread), so signals are always emitted there. Nothing ever calls back into the engine from the engine thread.
// Use an Engine only from the thread that created it.

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

struct BayanEngine;

namespace bayan {

// Identifies a blob kept by the engine (bulk bytes such as file contents). 0 means "no blob".
using BlobId = std::uint64_t;

class Engine final : public QObject {
  Q_OBJECT

public:
  // The protocol version this shell speaks (engine protocol §9: version 0 is the unstable pre-release version).
  static constexpr int kProtocolVersion = 0;
  // Largest message accepted from the engine; bigger ones are dropped and reported as a protocol error.
  static constexpr qsizetype kMaxMessageBytes = qsizetype{16} * 1024 * 1024;
  // Largest tile side, in pixels, that the shell asks for.
  static constexpr int kMaxTileSide = 4096;

  // Creates an engine with a JSON configuration. Returns nullptr if the engine could not be created.
  static std::unique_ptr<Engine> create(const QJsonObject &config = {});

  ~Engine() override;
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Engine(Engine &&) = delete;
  Engine &operator=(Engine &&) = delete;

  // The engine's version string.
  static QString version();

  // Sends a fire-and-forget message. Returns false if the engine refused it.
  bool post(const QString &type, const QJsonObject &payload = {});

  // Sends a request and returns its identifier (greater than 0), or 0 if the engine refused it. The answer arrives through replyReceived.
  qint64 request(const QString &type, const QJsonObject &payload = {});

  // Copies bytes into a new engine blob. Returns 0 on failure.
  BlobId putBlob(const QByteArray &bytes);
  // Returns a copy of a blob's bytes, or nothing if the blob does not exist.
  std::optional<QByteArray> blob(BlobId id);
  // Releases a blob. Returns false if it did not exist.
  bool releaseBlob(BlobId id);

  // Renders a tile synchronously: request is a render.tile payload (engine protocol §6) and pixelSize the tile's size in pixels.
  // Returns a premultiplied RGBA image, or a null image if the engine refused the request.
  QImage renderTile(const QJsonObject &request, QSize pixelSize);

  // Receives one message on the engine thread: copies it and queues it for this object's thread. Called by the engine callback; public so
  // that tests can play the engine thread.
  void deliverFromEngineThread(const std::uint8_t *json, std::size_t length) noexcept;

signals:
  // An answer to request(). On success, ok is true and payload holds the answer; otherwise error holds { code, message_id, args }.
  void replyReceived(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  // An event pushed by the engine, such as view.pages or engine.error.
  void eventReceived(const QString &type, const QJsonObject &payload);
  // The engine sent something that is not a valid protocol message. The reason is a fixed description, never message content.
  void protocolError(const QString &reason);

private:
  struct HandleDeleter {
    void operator()(BayanEngine *handle) const noexcept;
  };

  explicit Engine(std::unique_ptr<BayanEngine, HandleDeleter> handle);
  bool send(const QJsonObject &envelope);
  void dispatch(const QByteArray &json);

  std::unique_ptr<BayanEngine, HandleDeleter> handle_;
  qint64 nextRequestId_ = 1;
  // Messages that could not be copied on the engine thread (out of memory); reported with the next message that arrives.
  std::atomic<int> lostMessages_{0};
};

} // namespace bayan
