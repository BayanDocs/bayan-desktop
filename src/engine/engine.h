// The desktop shell's only door to the engine: a C++ wrapper over the C interface in bayan_ffi.h (engine protocol §3, ADR-0012).
//
// - It owns the engine handle (RAII: destroying the Engine destroys the engine, and no callback can arrive afterwards).
// - It sends protocol messages as JSON and turns the engine's replies and events into Qt signals.
// - The engine calls back on its own thread; the wrapper copies each message and hands it to the thread that owns the Engine object
//   (normally the main thread), so signals are always emitted there. Nothing ever calls back into the engine from the engine thread.
// - It checks every message's envelope there before anything uses it (engine protocol §4, §14.1 item 5): a JSON object, v the protocol
//   version this shell speaks (only the reply to a hello may carry another, so that the shell learns which versions an engine of another
//   version speaks), an event with a type and a sequence number, a reply with ok and, if it failed, an error with a code, and every field
//   of the type the protocol gives it. A reply must answer a request that was posted and not answered yet. Anything else is reported as
//   protocolError, which ends the shell's use of the engine; the payloads of the message types the shell reads are checked where they are
//   read (DocumentSession).
// Use an Engine only from the thread that created it, except renderTile() and lastPostedRequest(), which any thread may call while the
// Engine exists (the document canvas renders tiles on a thread of its own, so that scrolling never waits for the engine).

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
#include <functional>
#include <map>
#include <memory>
#include <optional>

struct BayanEngine;

namespace bayan {

// Identifies a blob kept by the engine (bulk bytes such as file contents). 0 means "no blob".
using BlobId = std::uint64_t;

class Engine final : public QObject {
  Q_OBJECT

public:
  // The protocol version this shell speaks (engine protocol §9, §14: version 1, the first stable version).
  static constexpr int kProtocolVersion = 1;
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
  // Identifiers increase with every request, so they also count the requests in the order they were posted.
  qint64 request(const QString &type, const QJsonObject &payload = {});

  // The identifier of the last request that was posted successfully, or 0. Any thread may read it. Only tests use it, to see whether the
  // session sent anything.
  [[nodiscard]] qint64 lastPostedRequest() const { return lastPostedRequest_.load(std::memory_order_acquire); }

  // Copies bytes into a new engine blob. Returns 0 on failure.
  BlobId putBlob(const QByteArray &bytes);
  // Returns a copy of a blob's bytes, or nothing if the blob does not exist.
  std::optional<QByteArray> blob(BlobId id);
  // Releases a blob. Returns false if it did not exist.
  bool releaseBlob(BlobId id);

  // A tile the engine rendered: premultiplied RGBA pixels, or a null image if the engine refused the request, and the render revision that
  // they show (engine protocol §14.6), a positive integer below 2^53.
  struct Tile {
    QImage image;
    quint64 revision = 0;
  };
  // Renders a tile synchronously: request is a render.tile payload (engine protocol §6.4) and pixelSize the tile's size in pixels. Any
  // thread may call it while the Engine exists, except the engine's own thread; it waits until the engine has handled every message posted
  // before it. A revision outside its range is a protocol violation: the tile is refused, and protocolError reaches this object's thread.
  Tile renderTile(const QJsonObject &request, QSize pixelSize);

  // Receives one message on the engine thread: copies it and queues it for this object's thread. Called by the engine callback; public so
  // that tests can play the engine thread.
  void deliverFromEngineThread(const std::uint8_t *json, std::size_t length) noexcept;

  // For the tests of the recovery after an engine failure (engine protocol §12.1): sends every later request of this type as diag.panic,
  // with the same identifier and no payload, so that an engine created with test.allow_panic fails while handling it and answers that very
  // request with the error panic, as it would for a document that makes it fail whenever it opens. An empty type turns this off. The
  // application never calls it, and an engine without test.allow_panic refuses diag.panic.
  void setPanicOnRequestTypeForTesting(const QString &type) { panicOnRequestType_ = type; }
  // For tests that check what the shell sends, such as hello's platform: called with every envelope that the engine accepted, on the thread
  // that sent it. The application never calls it.
  void setSentObserverForTesting(std::function<void(const QJsonObject &envelope)> observer) { sentObserver_ = std::move(observer); }

signals:
  // An answer to request(). On success, ok is true and payload holds the answer; otherwise error holds { code, message_id, args }, with a
  // string code. Each request is answered at most once; a reply to a request that was never posted, or that was answered already, is a
  // protocolError instead.
  void replyReceived(qint64 requestId, bool ok, const QJsonObject &payload, const QJsonObject &error);
  // An event pushed by the engine, such as view.pages or engine.error.
  void eventReceived(const QString &type, const QJsonObject &payload);
  // The engine broke the protocol (engine protocol §14.1 item 5): it sent something that is not a valid protocol message, or a tile with a
  // revision out of range. The reason is a fixed description, never message content.
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
  std::atomic<qint64> lastPostedRequest_{0};
  // The requests posted and not answered yet, and whether each is a hello, whose reply may carry another protocol version.
  std::map<qint64, bool> pending_;
  // Messages that could not be copied on the engine thread (out of memory); reported with the next message that arrives.
  std::atomic<int> lostMessages_{0};
  // See setPanicOnRequestTypeForTesting and setSentObserverForTesting.
  QString panicOnRequestType_;
  std::function<void(const QJsonObject &)> sentObserver_;
};

} // namespace bayan
