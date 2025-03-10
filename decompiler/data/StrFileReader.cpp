/*!
 * @file StrFileReader.cpp
 * Utility class to read a .STR file and extract the full file name.
 */

#include <cstring>
#include "common/util/FileUtil.h"
#include "game/common/overlord_common.h"
#include "game/common/str_rpc_types.h"
#include "StrFileReader.h"
#include "common/util/assert.h"

namespace decompiler {
StrFileReader::StrFileReader(const std::string& file_path) {
  auto data = file_util::read_binary_file(file_path);
  assert(data.size() >= SECTOR_SIZE);      // must have at least the header sector
  assert(data.size() % SECTOR_SIZE == 0);  // should be multiple of the sector size.
  int end_sector = int(data.size()) / SECTOR_SIZE;

  auto* header = (StrFileHeaderSector*)data.data();

  bool got_zero = false;
  for (int i = 0; i < SECTOR_TABLE_SIZE; i++) {
    // the chunk is from sector to next_sector
    int sector = header->sectors[i];
    // assume this chunk continues to the end...
    int next_sector = end_sector;
    // unless there's another chunk.
    if (i + 1 < SECTOR_TABLE_SIZE && header->sectors[i + 1]) {
      next_sector = header->sectors[i + 1];
    }
    if (sector) {
      assert(!got_zero);             // shouldn't have a non-zero after a zero!
      assert(next_sector > sector);  // should have a positive size.
      assert(next_sector * SECTOR_SIZE <= int(data.size()));  // check for overflowing the file
      // get chunk data.
      std::vector<u8> chunk;
      chunk.insert(chunk.end(), data.begin() + sector * SECTOR_SIZE,
                   data.begin() + next_sector * SECTOR_SIZE);
      m_chunks.emplace_back(std::move(chunk));
    } else {
      got_zero = true;
    }
  }

  // check our sizes are accurate. Will make sure that we include all data, as our m_chunks
  // are sized assuming they are packed in order and dense (sectors);
  for (int i = 0; i < SECTOR_TABLE_SIZE; i++) {
    if (header->sectors[i]) {
      assert(header->sizes[i] == m_chunks.at(i).size());
    } else {
      assert(header->sizes[i] == 0);
    }
  }

  // check nothing stored in the padding.
  for (auto x : header->pad) {
    assert(x == 0);
  }
}

int StrFileReader::chunk_count() const {
  return m_chunks.size();
}

const std::vector<u8>& StrFileReader::get_chunk(int idx) const {
  return m_chunks.at(idx);
}

namespace {
bool find_string_in_data(const u8* data, int data_size, const std::string& str, int* result) {
  for (int i = 0; i < data_size - int(str.length()); i++) {
    if (std::memcmp(data + i, str.c_str(), str.length()) == 0) {
      *result = i;
      return true;
    }
  }
  return false;
}

std::string get_string_of_max_length(const char* data, int max_length) {
  std::string result;
  for (int i = 0; i < max_length; i++) {
    if (data[i]) {
      result.push_back(data[i]);
    } else {
      return result;
    }
  }
  assert(false);
  return "";
}

struct FullName {
  std::string name;
  int chunk_idx = -1;
};

FullName extract_name(const std::string& file_info_name) {
  FullName name;
  name.name = file_info_name;
  assert(name.name.length() > 10);
  assert(name.name.substr(name.name.length() - 6, 6) == "-ag.go");
  name.name = name.name.substr(0, name.name.length() - 6);
  int chunk_id = 0;
  int place = 0;
  for (int i = 2; i-- > 0;) {
    char c = name.name.back();
    if (c >= '0' && c <= '9') {
      int val = (c - '0');
      for (int j = 0; j < place; j++) {
        val *= 10;
      }
      chunk_id += val;
      name.name.pop_back();
      place++;
    } else {
      break;
    }
  }
  assert(name.name.back() == '+');
  name.name.pop_back();
  name.chunk_idx = chunk_id;
  return name;
}

}  // namespace

/*!
 * Look inside the chunks to determine the source file name.
 * Does a lot of checking, might not work in future versions without some updating.
 */
std::string StrFileReader::get_full_name(const std::string& short_name) const {
  std::string result;
  bool done_first = false;

  // this string is part of the file info struct and the stuff after it is the file name.
  const std::string file_info_string = "/src/next/data/art-group6/";

  // it should occur in each chunk.
  int chunk_id = 0;
  for (const auto& chunk : m_chunks) {
    std::string chunk_long_name;

    // find the file info string in the chunk.
    int offset;
    if (find_string_in_data(chunk.data(), int(chunk.size()), file_info_string, &offset)) {
      offset += file_info_string.length();
    } else {
      assert(false);
    }

    // extract the name info as a "name" + "chunk id" + "-ag.go" format.
    auto full_name =
        extract_name(get_string_of_max_length((const char*)(chunk.data() + offset), 128));

    // make sure it matches previous chunks for the name
    if (!done_first) {
      result = full_name.name;
    } else {
      assert(result == full_name.name);
    }

    // make sure the index is right.
    assert(full_name.chunk_idx == chunk_id);

    done_first = true;
    chunk_id++;
  }

  // convert to ISO names in two ways.
  char iso_name_2[256];
  char iso_name_1[256];

  // first, using the file name to ISO name
  file_util::MakeISOName(iso_name_1, short_name.c_str());
  // second, using the full name.
  file_util::ISONameFromAnimationName(iso_name_2, result.c_str());
  assert(strcmp(iso_name_1, iso_name_2) == 0);

  return result;
}
}  // namespace decompiler
