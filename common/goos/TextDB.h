#pragma once

/*!
 * @file TextDB.h
 * The Text Database for storing source code text.
 * This allows us to ask for things like "where did this form come from?"
 * and be able to print the file name and offset into the file, as well as the line.
 *
 * The purpose is to be able to have an error message like:
 *
 * Error on (+ a b): a has invalid type (string)
 * From my-file.gc, line 25:
 *   (+ 1 (+ a b)) ; compute the sum
 */

#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <memory>
#include <optional>

#include "common/goos/Object.h"

namespace goos {
/*!
 * Parent class for source-code text organized in lines
 */
class SourceText {
 public:
  explicit SourceText(std::string r);
  SourceText() = default;
  const char* get_text() { return m_text.c_str(); }
  int get_size() { return m_text.size(); }
  virtual std::string get_description() = 0;
  std::string get_line_containing_offset(int offset);
  int get_line_idx(int offset);
  int get_offset_of_line(int line_idx);
  // should the compiler keep looking up the stack when printing errors on this, or not?
  // this should return true if the text source is specific enough so that they can find what they
  // want
  virtual bool terminate_compiler_error() { return true; }

  virtual ~SourceText(){};

 protected:
  void build_offsets();
  std::string m_text;
  std::vector<int> m_offset_by_line;
  std::pair<int, int> get_containing_line(int offset);
};

/*!
 * Text from the REPL prompt
 */
class ReplText : public SourceText {
 public:
  explicit ReplText(const std::string& text_) : SourceText(text_) {}
  std::string get_description() override { return "REPL"; }
  ~ReplText() = default;
};

/*!
 * Text generated by the program itself (like for example, with (read "hello"))
 */
class ProgramString : public SourceText {
 public:
  explicit ProgramString(const std::string& text_,
                         const std::string& string_name = "Program string")
      : SourceText(text_), m_string_name(string_name) {}
  std::string get_description() override { return m_string_name; }
  ~ProgramString() = default;

 private:
  std::string m_string_name;
};

/*!
 * Text from a file.
 */
class FileText : public SourceText {
 public:
  FileText(const std::string& filename, const std::string& description_name);

  std::string get_description() { return m_desc_name; }
  ~FileText() = default;

 private:
  std::string m_filename;
  std::string m_desc_name;
};

struct TextRef {
  int offset;
  std::shared_ptr<SourceText> frag;
};

class TextDb {
 public:
  struct ShortInfo {
    std::string filename;
    int line_idx_to_display = -1;
    int pos_in_line = -1;
    std::string line_text;
  };

  void insert(const std::shared_ptr<SourceText>& frag);
  void link(const Object& o, std::shared_ptr<SourceText> frag, int offset);
  std::string get_info_for(const Object& o, bool* terminate_compiler_error = nullptr) const;
  std::string get_info_for(const std::shared_ptr<SourceText>& frag, int offset) const;
  std::optional<ShortInfo> try_get_short_info(const Object& o) const;
  bool has_info(const Object& o) const;
  void inherit_info(const Object& parent, const Object& child);

 private:
  std::vector<std::shared_ptr<SourceText>> m_fragments;
  std::unordered_map<std::shared_ptr<goos::HeapObject>, TextRef> m_map;
};
}  // namespace goos
