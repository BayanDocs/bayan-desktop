// Checks that a window shows exactly the engine's pixels where the document canvas draws a page (DESK-002): for points spread over the
// visible part of a page, it renders the single device pixel there with the engine and compares it with the window's pixel. It needs no
// knowledge of what the page looks like, so the smoke test and the tests use it with the stub engine and with the real one.

#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

#include <optional>

namespace bayan {

class DocumentCanvas;

// Returns nothing if every probe matches, otherwise what went wrong. frame is a grab of the canvas's window, in device pixels.
[[nodiscard]] std::optional<QString> compareWithEnginePixels(const DocumentCanvas &canvas, const QImage &frame, int page = 0);

} // namespace bayan
