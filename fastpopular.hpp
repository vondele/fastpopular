#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "external/chess.hpp"
#include "external/json.hpp"

struct TestMetaData {
  std::optional<std::string> book;
  std::optional<bool> sprt;
  std::optional<int> book_depth;
};

template <typename T = std::string>
std::optional<T> get_optional(const nlohmann::json &j, const char *name) {
  const auto it = j.find(name);
  if (it != j.end()) {
    return std::optional<T>(j[name]);
  } else {
    return std::nullopt;
  }
}

void from_json(const nlohmann::json &nlohmann_json_j,
               TestMetaData &nlohmann_json_t) {
  auto &j = nlohmann_json_j["args"];

  nlohmann_json_t.book_depth =
      get_optional(j, "book_depth").has_value()
          ? std::optional<int>(std::stoi(get_optional(j, "book_depth").value()))
          : std::nullopt;

  nlohmann_json_t.sprt =
      j.contains("sprt") ? std::optional<bool>(true) : std::nullopt;

  nlohmann_json_t.book = get_optional(j, "book");
}

/// @brief Get all files from a directory.
/// @param path
/// @param recursive
/// @return
[[nodiscard]] inline std::vector<std::string>
get_files(const std::string &path, bool recursive = false) {
  std::vector<std::string> files;

  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    if (std::filesystem::is_regular_file(entry)) {
      std::string stem = entry.path().stem().string();
      std::string extension = entry.path().extension().string();
      if (extension == ".gz" || extension == ".zst") {
        if (stem.size() >= 4 && stem.substr(stem.size() - 4) == ".pgn") {
          files.push_back(entry.path().string());
        }
      } else if (extension == ".pgn") {
        files.push_back(entry.path().string());
      }
    } else if (recursive && std::filesystem::is_directory(entry)) {
      auto subdir_files = get_files(entry.path().string(), true);
      files.insert(files.end(), subdir_files.begin(), subdir_files.end());
    }
  }

  return files;
}

/// @brief Split into successive n-sized chunks from pgns.
/// @param pgns
/// @param target_chunks
/// @return
[[nodiscard]] inline std::vector<std::vector<std::string>>
split_chunks(const std::vector<std::string> &pgns, int target_chunks) {
  const int chunks_size = (pgns.size() + target_chunks - 1) / target_chunks;

  auto begin = pgns.begin();
  auto end = pgns.end();

  std::vector<std::vector<std::string>> chunks;

  while (begin != end) {
    auto next =
        std::next(begin, std::min(chunks_size,
                                  static_cast<int>(std::distance(begin, end))));
    chunks.push_back(std::vector<std::string>(begin, next));
    begin = next;
  }

  return chunks;
}

inline bool find_argument(const std::vector<std::string> &args,
                          std::vector<std::string>::const_iterator &pos,
                          std::string_view arg,
                          bool without_parameter = false) {
  pos = std::find(args.begin(), args.end(), arg);

  return pos != args.end() &&
         (without_parameter || std::next(pos) != args.end());
}

inline std::string to_lower(std::string_view s) {
  std::string result(s);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

/// @brief Return the material key for a board as "<White>v<Black>" (e.g. KBNvKP).
/// Piece order is KQRBNP, kings are always present.
[[nodiscard]] inline std::string material_key(const chess::Board &board) {
  static constexpr std::array<chess::PieceType, 5> types = {
      chess::PieceType::QUEEN, chess::PieceType::ROOK, chess::PieceType::BISHOP,
      chess::PieceType::KNIGHT, chess::PieceType::PAWN};
  static constexpr std::array<char, 5> letters = {'Q', 'R', 'B', 'N', 'P'};

  auto count = [&](chess::PieceType pt, chess::Color c) {
    return board.pieces(pt, c).count();
  };

  std::string key;
  key += 'K';
  for (std::size_t i = 0; i < types.size(); ++i) {
    key += std::string(count(types[i], chess::Color::WHITE), letters[i]);
  }
  key += "vK";
  for (std::size_t i = 0; i < types.size(); ++i) {
    key += std::string(count(types[i], chess::Color::BLACK), letters[i]);
  }
  return key;
}

namespace detail {

inline void generate_compositions(int sum, int parts, std::array<int, 5> &current,
                                  int pos,
                                  std::vector<std::array<int, 5>> &out) {
  if (parts == 0) {
    if (sum == 0) {
      out.push_back(current);
    }
    return;
  }
  for (int v = 0; v <= sum; ++v) {
    current[pos] = v;
    generate_compositions(sum - v, parts - 1, current, pos + 1, out);
  }
}

} // namespace detail

/// @brief Generate all exact White-vs-Black material keys with 2..max_pieces
/// pieces on the board. Sorted by total piece count, then key.
[[nodiscard]] inline std::vector<std::string>
generate_material_keys(int max_pieces) {
  std::vector<std::pair<std::string, int>> keys;

  for (int non_king = 0; non_king <= max_pieces - 2; ++non_king) {
    for (int white_extra = 0; white_extra <= non_king; ++white_extra) {
      int black_extra = non_king - white_extra;

      std::vector<std::array<int, 5>> white_combos;
      std::array<int, 5> current{};
      detail::generate_compositions(white_extra, 5, current, 0, white_combos);

      std::vector<std::array<int, 5>> black_combos;
      detail::generate_compositions(black_extra, 5, current, 0, black_combos);

      for (const auto &wc : white_combos) {
        for (const auto &bc : black_combos) {
          std::string key;
          key += 'K';
          key += std::string(wc[0], 'Q');
          key += std::string(wc[1], 'R');
          key += std::string(wc[2], 'B');
          key += std::string(wc[3], 'N');
          key += std::string(wc[4], 'P');
          key += "vK";
          key += std::string(bc[0], 'Q');
          key += std::string(bc[1], 'R');
          key += std::string(bc[2], 'B');
          key += std::string(bc[3], 'N');
          key += std::string(bc[4], 'P');
          keys.emplace_back(key, non_king + 2);
        }
      }
    }
  }

  std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
    if (a.second != b.second)
      return a.second < b.second;
    return a.first < b.first;
  });

  std::vector<std::string> result;
  result.reserve(keys.size());
  for (const auto &k : keys) {
    result.push_back(k.first);
  }
  return result;
}
