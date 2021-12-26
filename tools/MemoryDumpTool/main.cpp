#include <string>
#include <cassert>
#include <fstream>
#include <iomanip>
#include "third-party/fmt/core.h"
#include "third-party/11zip/include/elzip/elzip.hpp"
#include "third-party/json.hpp"

#include "common/util/FileUtil.h"
#include "common/goal_constants.h"
#include "common/symbols.h"
#include "common/type_system/TypeSystem.h"

#include "decompiler/util/DecompilerTypeSystem.h"

struct Ram {
  const u8* data = nullptr;
  u32 size = 0;

  Ram(const u8* _data, u32 _size) : data(_data), size(_size) {}

  template <typename T>
  T read(u32 addr) const {
    assert(in_memory<T>(addr));
    T result;
    memcpy(&result, data + addr, sizeof(T));
    return result;
  }

  template <typename T>
  bool in_memory(u32 addr) const {
    return addr > (1 << 19) && addr <= (size - sizeof(T));
  }

  u32 word(u32 addr) const { return read<u32>(addr); }

  u8 byte(int addr) const { return read<u8>(addr); }

  std::string string(u32 addr) const {
    std::string result;
    while (true) {
      assert(in_memory<u8>(addr));
      auto next = read<u8>(addr++);
      if (next) {
        result.push_back(next);
      } else {
        return result;
      }
    }
  }

  std::optional<std::string> try_string(u32 addr, int max_len = 128) const {
    std::string result;
    for (int i = 0; i < max_len; i++) {
      if (!in_memory<u8>(addr)) {
        return {};
      }
      auto next = read<u8>(addr++);
      if (next) {
        result.push_back(next);
      } else {
        return result;
      }
    }
    return {};
  }

  /*!
   * addr, including basic offset.
   */
  std::string goal_string(u32 addr) { return string(addr + 4); }

  bool word_in_memory(u32 addr) const { return in_memory<u32>(addr); }
};

u32 scan_for_symbol_table(const Ram& ram, u32 start_addr, u32 end_addr) {
  fmt::print("scanning for symbol table in 0x{:x} - 0x{:x}\n", start_addr, end_addr);
  std::vector<u32> candidates;

  // look for the false symbol.
  for (u32 addr = (start_addr & 0xfffffff0); addr < end_addr; addr += 8) {
    if (ram.word(addr + 4) == addr + 4) {
      candidates.push_back(addr);
    }
  }

  fmt::print("got {} candidates for #f:\n", candidates.size());

  for (auto addr : candidates) {
    auto str = addr + BASIC_OFFSET + SYM_INFO_OFFSET;
    fmt::print(" trying 0x{:x}:\n", addr);
    if (ram.word_in_memory(str)) {
      auto mem = ram.word(str + 4);         // offset of str in SymInfo
      auto name = ram.try_string(mem + 4);  // offset of data in GOAL string
      if (name) {
        fmt::print("   name: {}\n", *name);
      }
      if (name == "#f") {
        fmt::print("Got #f = 0x{:x}!\n", addr + 4);
        return addr + 4;
      }
    }
  }

  return 0;
}

struct SymbolMap {
  std::unordered_map<std::string, u32> name_to_addr;
  std::unordered_map<std::string, u32> name_to_value;
  std::unordered_map<u32, std::string> addr_to_name;
};

SymbolMap build_symbol_map(const Ram& ram, u32 s7) {
  fmt::print("finding symbols...\n");
  SymbolMap map;
  /*
   s7 = symbol_table + (GOAL_MAX_SYMBOLS / 2) * 8 + BASIC_OFFSET;
  // pointer to the first symbol (SymbolTable2 is the "lower" symbol table)
  SymbolTable2 = symbol_table + BASIC_OFFSET;
  // the last symbol we will ever access.
  LastSymbol = symbol_table + 0xff00;
   */

  auto symbol_table = s7 - ((GOAL_MAX_SYMBOLS / 2) * 8 + BASIC_OFFSET);
  auto SymbolTable2 = symbol_table + BASIC_OFFSET;
  auto LastSymbol = symbol_table + 0xff00;

  for (u32 sym = SymbolTable2; sym < LastSymbol; sym += 8) {
    auto info = sym + SYM_INFO_OFFSET;  // already has basic offset
    auto str = ram.word(info + 4);
    if (str) {
      auto name = ram.string(str + 4);
      if (name != "asize-of-basic-func") {
        assert(map.name_to_addr.find(name) == map.name_to_addr.end());
        map.name_to_addr[name] = sym;
        map.addr_to_name[sym] = name;
        map.name_to_value[name] = ram.word(sym);
      }
    }
  }

  assert(map.name_to_addr.size() == map.addr_to_name.size());
  fmt::print("found {} symbols.\n", map.name_to_addr.size());
  return map;
}

std::unordered_map<u32, std::string> build_type_map(const Ram& ram,
                                                    const SymbolMap& symbols,
                                                    u32 s7) {
  std::unordered_map<u32, std::string> result;
  fmt::print("finding types...\n");
  u32 type_of_type = ram.word(s7 + FIX_SYM_TYPE_TYPE);
  assert(type_of_type == ram.word(symbols.name_to_addr.at("type")));

  for (const auto& [name, addr] : symbols.name_to_addr) {
    u32 value = ram.word(addr);
    if (ram.word_in_memory(value - 4) && ((value & 0x7) == BASIC_OFFSET)) {
      if (ram.word(value - 4) == type_of_type) {
        result[value] = name;
      }
    }
  }

  fmt::print("found {} types\n", result.size());
  return result;
}

std::unordered_map<std::string, std::vector<u32>> find_basics(
    const Ram& ram,
    const std::unordered_map<u32, std::string>& type_map) {
  fmt::print("Scanning memory for objects. This may take a while...\n");

  std::unordered_map<std::string, std::vector<u32>> result;
  int total_objects = 0;

  for (u32 addr = (1 << 20); addr < ram.size; addr += 16) {
    u32 tag = ram.word(addr);
    auto iter = type_map.find(tag);
    // ignore the stupid types.
    if (iter != type_map.end() && iter->second != "symbol" && iter->second != "string" &&
        iter->second != "function" && iter->second != "object" && iter->second != "integer") {
      result[iter->second].push_back(addr);
      total_objects++;
    }
  }

  fmt::print("Got {} objects of {} unique types\n", total_objects, result.size());
  return result;
}

void inspect_process_self(const Ram& ram,
                          const std::unordered_map<std::string, std::vector<u32>>& basics,
                          const std::unordered_map<u32, std::string>& types,
                          const TypeSystem& type_system) {
  std::vector<std::string> sorted_type_names;
  for (auto& x : basics) {
    sorted_type_names.emplace_back(x.first);
  }
  std::sort(sorted_type_names.begin(), sorted_type_names.end(), [&](const auto& a, const auto& b) {
    return basics.at(a).size() < basics.at(b).size();
  });

  for (const auto& name : sorted_type_names) {
    // first, try looking up the type.
    if (!type_system.fully_defined_type_exists(name)) {
      continue;
    }

    auto type = dynamic_cast<BasicType*>(type_system.lookup_type(name));
    if (!type) {
      continue;
    }

    for (auto& field : type->fields()) {
      if (field.name() == "self") {
        for (auto base_addr : basics.at(name)) {
          int field_addr = base_addr + field.offset();
          if (ram.word_in_memory(field_addr)) {
            auto field_val = ram.word(field_addr);
            if (base_addr + 4 != field_val) {
              fmt::print("Process type {} had mismatched self #x{:x} #x{:x}\n", name, field_val,
                         base_addr);
              if (ram.word_in_memory(field_val - 4)) {
                auto type_lookup = types.find(ram.word(field_val - 4));
                if (type_lookup != types.end()) {
                  fmt::print("  The actual thing had type {}\n", type_lookup->second);
                }
              }
            }
          }
        }
      }
    }
  }
}

void follow_references_to_find_pointers(
    const Ram& ram,
    const TypeSystem& type_system,
    std::unordered_map<std::string, std::vector<u32>>& basics_in,
    u32 st_addr) {
  // all the objects.
  std::unordered_map<std::string, std::unordered_set<u32>> found;
  // things to check.
  std::vector<std::pair<std::string, u32>> stack;

  // insert the basics
  for (auto& kv : basics_in) {
    for (auto addr : kv.second) {
      found[kv.first].insert(addr);
      stack.push_back({kv.first, addr});
    }
  }

  // dfs
  while (!stack.empty()) {
    auto to_check = stack.back();
    stack.pop_back();

    if (type_system.fully_defined_type_exists(to_check.first)) {
      auto type_info = dynamic_cast<StructureType*>(type_system.lookup_type(to_check.first));
      if (type_info == NULL) {
        continue;
      }
      for (auto& field : type_info->fields()) {
        if (type_system.fully_defined_type_exists(field.type())) {
          auto field_info = type_system.lookup_type(field.type());
          auto field_as_structure = dynamic_cast<StructureType*>(field_info);
          auto field_as_basic = dynamic_cast<BasicType*>(field_info);
          if (field_as_structure && !field_as_basic) {
            u32 field_address = to_check.second + field.offset();
            if (field.is_inline()) {
              if (ram.word_in_memory(field_address) && field_address > st_addr) {
                if (found[field.type().base_type()].insert(field_address).second) {
                  // fmt::print("In type {} field {} (inline), found an {} at {} {}\n",
                  // to_check.first,
                  //            field.name(), field.type().print(), field_address,
                  //            field_address & 0xf);
                  stack.push_back({field.type().base_type(), field_address});
                }
              }

            } else {
              if (ram.word_in_memory(field_address)) {
                u32 field_value = ram.word(field_address);
                if (ram.word_in_memory(field_value) && field_value > st_addr) {
                  if (found[field.type().base_type()].insert(field_value).second) {
                    // fmt::print("In type {} field {}, found an {} at {} {}\n", to_check.first,
                    //            field.name(), field.type().print(), field_value, field_value &
                    //            0xf);
                    stack.push_back({field.type().base_type(), field_value});
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  int total_found = 0;
  for (const auto& kv : found) {
    for (auto addr : kv.second) {
      basics_in[kv.first].push_back(addr);
      total_found++;
    }
  }

  fmt::print("Following points found {} objects.\n", total_found++);
}

void inspect_basics(const Ram& ram,
                    const std::unordered_map<std::string, std::vector<u32>>& basics,
                    const std::unordered_map<u32, std::string>& types,
                    const SymbolMap& symbols,
                    const TypeSystem& type_system,
                    nlohmann::json& results) {
  std::vector<std::string> sorted_type_names;
  for (auto& x : basics) {
    sorted_type_names.emplace_back(x.first);
  }
  std::sort(sorted_type_names.begin(), sorted_type_names.end(), [&](const auto& a, const auto& b) {
    return basics.at(a).size() < basics.at(b).size();
  });

  for (const auto& name : sorted_type_names) {
    fmt::print("TYPE {} (count {})\n", name, basics.at(name).size());

    nlohmann::json type_results;
    if (results.contains(name)) {
      type_results = results.at(name);
      type_results["__metadata"]["occurences"] =
          type_results["__metadata"]["occurences"].get<int>() + basics.at(name).size();
    } else {
      type_results["__metadata"]["unknown?"] = false;
      type_results["__metadata"]["failedToCast?"] = false;
      type_results["__metadata"]["occurences"] = basics.at(name).size();
    }

    // first, try looking up the type.
    if (!type_system.fully_defined_type_exists(name)) {
      fmt::print("-----Type is unknown!\n\n");
      std::string wat = type_results.dump();
      type_results["__metadata"]["unknown?"] = true;
      results[name] = type_results;
      continue;
    }

    auto type = dynamic_cast<StructureType*>(type_system.lookup_type(name));
    if (!type) {
      fmt::print("Could not cast Type! Skipping!!\n");
      type_results["__metadata"]["failedToCast?"] = true;
      results[name] = type_results;
      continue;
    }

    if (!dynamic_cast<BasicType*>(type)) {
      fmt::print("NOTE: Not a basic.\n");
    }

    for (auto& field : type->fields()) {
      if (!field.is_inline() && !field.is_dynamic() &&
          (field.type() == TypeSpec("basic") || field.type() == TypeSpec("object") ||
           field.type() == TypeSpec("uint32") ||
           field.type() == TypeSpec("array", {TypeSpec("basic")}))) {
        int array_size = field.is_array() ? field.array_size() : 1;
        fmt::print("  field {}\n", field.name());

        nlohmann::json field_results;
        if (type_results.contains(field.name())) {
          field_results = type_results.at(field.name());
        } else {
          field_results = {};
        }

        bool goal_array = field.type() == TypeSpec("array", {TypeSpec("basic")});

        std::unordered_map<std::string, int> type_frequency;

        for (auto base_addr : basics.at(name)) {
          for (int elt_idx = 0; elt_idx < array_size; elt_idx++) {
            int field_addr = base_addr + field.offset() + 4 * elt_idx;
            if (ram.word_in_memory(field_addr)) {
              auto field_val = ram.word(field_addr);
              auto array_addr = field_val;
              int goal_array_length = 1;
              if (goal_array) {
                if (ram.word_in_memory(field_val)) {
                  goal_array_length = ram.word(field_val);
                } else {
                  array_addr = 0xBAADBEEF;
                }
              }
              for (int arr_idx = 0; arr_idx < goal_array_length; ++arr_idx) {
                if (goal_array) {
                  field_val = array_addr + 12 + arr_idx * 4;
                  if (ram.word_in_memory(field_val)) {
                    field_val = ram.word(field_val);
                  } else {
                    field_val = 0xBAADBEEF;
                  }
                }

                if ((field_val & 0x7) == 4 && ram.word_in_memory(field_val - 4)) {
                  auto type_tag = ram.word(field_val - 4);
                  auto iter = types.find(type_tag);
                  if (iter != types.end()) {
                    if (iter->second == "symbol") {
                      auto sym_iter = symbols.addr_to_name.find(field_val);
                      if (sym_iter != symbols.addr_to_name.end()) {
                        type_frequency[fmt::format("(symbol {})", sym_iter->second)]++;
                      } else {
                        type_frequency[iter->second]++;
                      }
                    } else {
                      type_frequency[iter->second]++;
                    }
                  } else {
                    type_frequency["_bad-type"]++;
                  }
                } else if (field_val == 0) {
                  type_frequency["0"]++;
                } else {
                  type_frequency["_not-basic-ptr"]++;
                }

                if (!goal_array)
                  break;
              }
            } else {
              type_frequency["_bad-field-memory"]++;
            }
          }
        }

        std::vector<std::string> sorted_field_types;
        for (const auto& x : type_frequency) {
          sorted_field_types.push_back(x.first);
        }
        std::sort(sorted_field_types.begin(), sorted_field_types.end(),
                  [&](const auto& a, const auto& b) {
                    return type_frequency.at(a) > type_frequency.at(b);
                  });

        for (const auto& field_type : sorted_field_types) {
          int freq = type_frequency.at(field_type);
          if (field_results.contains(field_type)) {
            field_results[field_type] = field_results[field_type].get<int>() + freq;
          } else {
            field_results[field_type] = freq;
          }
          fmt::print("     [{}] {}\n", type_frequency.at(field_type), field_type);
        }

        type_results[field.name()] = field_results;
      } else if (field.type().base_type() == "handle" && !field.is_array()) {
        // check the types of handles.
        // auto proc_type = type_system.lookup_type("process");
        std::unordered_map<std::string, int> type_frequency;
        fmt::print("  field {}\n", field.name());

        for (auto base_addr : basics.at(name)) {
          int field_addr = base_addr + field.offset();
          if (ram.word_in_memory(field_addr)) {
            auto proc_pointer = ram.word(field_addr);  // pointer process
            // auto pid = ram.word(field_addr + 4);
            if (ram.word_in_memory(proc_pointer)) {
              auto proc = ram.word(proc_pointer);
              auto proc_type_tag_addr = proc - 4;
              if (ram.word_in_memory(proc_type_tag_addr)) {
                auto type_tag_value = ram.word(proc_type_tag_addr);
                auto type_it = types.find(type_tag_value);
                if (type_it != types.end()) {
                  if (type_it->second == "symbol") {
                    auto sym_iter = symbols.addr_to_name.find(proc);
                    if (sym_iter != symbols.addr_to_name.end()) {
                      type_frequency[fmt::format("(symbol {})", sym_iter->second)]++;
                    }
                  } else {
                    type_frequency[type_it->second]++;
                  }
                }
              }
            }
          }
        }

        std::vector<std::string> sorted_field_types;
        for (const auto& x : type_frequency) {
          sorted_field_types.push_back(x.first);
        }
        std::sort(sorted_field_types.begin(), sorted_field_types.end(),
                  [&](const auto& a, const auto& b) {
                    return type_frequency.at(a) > type_frequency.at(b);
                  });
        nlohmann::json field_results;
        if (type_results.contains(field.name())) {
          field_results = type_results.at(field.name());
        } else {
          field_results = {};
        }
        for (const auto& field_type : sorted_field_types) {
          int freq = type_frequency.at(field_type);
          if (field_results.contains(field_type)) {
            field_results[field_type] = field_results[field_type].get<int>() + freq;
          } else {
            field_results[field_type] = freq;
          }
          fmt::print("     [{}] {} (handle)\n", type_frequency.at(field_type), field_type);
        }
        type_results[field.name()] = field_results;
      }
    }

    results[name] = type_results;
  }
}

static bool ends_with(const std::string& str, const std::string& suffix) {
  return str.size() >= suffix.size() &&
         0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

void inspect_symbols(const Ram& ram,
                     const std::unordered_map<u32, std::string>& types,
                     const SymbolMap& symbols) {
  fmt::print("Symbols:\n");
  for (const auto& [name, addr] : symbols.name_to_addr) {
    std::string found_type;
    if (ram.word_in_memory(addr)) {
      u32 symbol_value = ram.read<u32>(addr);
      if ((symbol_value & 0xf) == 4) {
        if (ram.word_in_memory(symbol_value)) {
          u32 type = ram.read<u32>(symbol_value - 4);
          auto type_it = types.find(type);
          if (type_it != types.end()) {
            found_type = type_it->second;
          }
        }
      }
    }
    if (!found_type.empty()) {
      fmt::print("  [{:08x}] {:30s} : {}\n", symbols.name_to_value.at(name), name, found_type);
    }
  }
}

int main(int argc, char** argv) {
  fmt::print("MemoryDumpTool\n");

  if (argc != 2 && argc != 3) {
    fmt::print("usage: memory_dump_tool <ee_ram.bin|savestate.p2s> [output folder]\n");
    return 1;
  }

  fmt::print("Loading type definitions from all-types.gc...\n");
  decompiler::DecompilerTypeSystem dts;
  dts.parse_type_defs({"decompiler", "config", "all-types.gc"});

  std::string file_name = argv[1];
  fs::path output_folder;

  if (!fs::exists(output_folder) || argc < 3) {
    fmt::print("Output folder not found, defaulting to current directory");
    output_folder = ".";
  } else {
    output_folder = argv[2];
  }

  // If it's a PCSX2 savestate, lets extract the ee memory automatically
  if (ends_with(file_name, "p2s")) {
    fmt::print("Detected PCSX2 Save-state '{}', extracting memory...\n", file_name);
    elz::extractZip(file_name, "./savestate-out");
    // Then, check for and use the eeMemory.bin file
    if (fs::exists("./savestate-out/eeMemory.bin")) {
      file_name = "./savestate-out/eeMemory.bin";
      fmt::print("EE Memory extracted\n");
    } else {
      fmt::print("Couldn't locate EE Memory, aborting!\n");
      return 1;
    }
  }

  fmt::print("Loading memory from '{}'\n", file_name);
  auto data = file_util::read_binary_file(file_name);

  u32 one_mb = (1 << 20);

  if (data.size() == 32 * one_mb) {
    fmt::print("Got 32MB file\n");
  } else if (data.size() == 128 * one_mb) {
    fmt::print("Got 128MB file\n");
  } else if (data.size() == 127 * one_mb) {
    fmt::print("Got a 127MB file. Assuming this is a dump with the first 1 MB missing.\n");
    data.insert(data.begin(), one_mb, 0);
    assert(data.size() == 128 * one_mb);
  } else {
    fmt::print("Invalid size: {} bytes\n", data.size());
  }

  Ram ram(data.data(), data.size());

  u32 s7 = scan_for_symbol_table(ram, one_mb, 2 * one_mb);
  if (!s7) {
    fmt::print("Failed to find symbol table\n");
    return 1;
  }

  nlohmann::json results;
  if (fs::exists(output_folder / "ee-results.json")) {
    fmt::print("Found existing result file, appending results to it!\n");
    std::ifstream i(output_folder / "ee-results.json");
    i >> results;
  }

  auto symbol_map = build_symbol_map(ram, s7);
  auto types = build_type_map(ram, symbol_map, s7);
  auto basics = find_basics(ram, types);

  follow_references_to_find_pointers(ram, dts.ts, basics, s7 + 0x100);

  inspect_basics(ram, basics, types, symbol_map, dts.ts, results);
  inspect_symbols(ram, types, symbol_map);
  inspect_process_self(ram, basics, types, dts.ts);

  if (fs::exists(output_folder / "ee-results.json")) {
    fs::remove(output_folder / "ee-results.json");
  }
  std::ofstream o(output_folder / "ee-results.json");
  o << std::setw(2) << results << std::endl;

  return 0;
}
