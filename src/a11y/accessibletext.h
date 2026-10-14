// The document's text as the desktop's screen readers read it (DESK-002, ADR-0020 §3).
//
// The engine describes the document as an accessibility tree (engine protocol §6.6): headings, paragraphs, lists, tables and the text of
// each, with the box of every character. The desktop's first bridge exposes the whole document as one multi-line text, the way a text
// editor does: the text of every node that has text, in the tree's reading order, one node per line. Qt's platform bridges then give screen
// readers the usual text navigation (by character, word, line and paragraph) on Windows (UI Automation's text pattern), macOS
// (NSAccessibility's text attributes) and Linux (AT-SPI's text interface). This class builds that text and maps between its offsets and the
// engine's nodes; it adds nothing that is not in the tree. The report (DESK-002) compares this with exposing each node as its own object.

#pragma once

#include "protocol.h"

#include <QtCore/QString>

#include <optional>
#include <vector>

namespace bayan {

class AccessibleText {
public:
  // One node's text inside the whole text.
  struct Segment {
    quint64 node = 0;
    protocol::Role role = protocol::Role::Other;
    // Where the node's text starts in the whole text, and its length, in UTF-16 code units.
    int start = 0;
    int length = 0;
    bool editable = false;
    // The node's box, and the box of each of its characters, on the page of the box.
    std::optional<protocol::PageRect> bounds;
    std::vector<protocol::CharBox> chars;

    [[nodiscard]] int end() const { return start + length; }
  };

  // The change between two versions of the text, for the screen readers' text-changed events.
  struct Change {
    int position = 0;
    QString removed{};
    QString inserted{};
  };

  AccessibleText() = default;
  explicit AccessibleText(const protocol::AccessibleTree &tree);

  [[nodiscard]] const QString &text() const { return text_; }
  // The root node's accessible name ("BayanDocs mock document" for the mock document), or an empty string.
  [[nodiscard]] const QString &name() const { return name_; }
  [[nodiscard]] const std::vector<Segment> &segments() const { return segments_; }
  [[nodiscard]] bool isEmpty() const { return segments_.empty(); }

  // The segment that holds an offset of the whole text. The offset just after a segment's last character (where its line ends) belongs to
  // that segment. nullptr if there is none.
  [[nodiscard]] const Segment *segmentAt(int offset) const;
  [[nodiscard]] const Segment *segmentOf(quint64 node) const;
  // The offset in the whole text of an offset in a node's text, or nothing if the node has no text here.
  [[nodiscard]] std::optional<int> offsetOf(quint64 node, int nodeOffset) const;
  // The page and box of the character at an offset. At the end of a segment, a zero-width box after its last character.
  [[nodiscard]] std::optional<protocol::PageRect> characterBox(int offset) const;

  // The smallest single change that turns before into after.
  [[nodiscard]] static Change difference(const QString &before, const QString &after);

private:
  QString text_;
  QString name_;
  std::vector<Segment> segments_;
};

} // namespace bayan
