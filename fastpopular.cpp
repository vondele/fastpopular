#include "fastpopular.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "external/chess.hpp"
#include "external/gzip/gzstream.h"
#include "external/parallel_hashmap/phmap.h"
#include "external/threadpool.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

using namespace chess;

// unordered map to count zobrist keys
using zobrist_map_t = phmap::parallel_flat_hash_map<
    std::uint64_t, std::uint64_t, std::hash<std::uint64_t>,
    std::equal_to<std::uint64_t>,
    std::allocator<std::pair<const std::uint64_t, std::uint64_t>>, 8,
    std::mutex>;

zobrist_map_t zobrist_map;

// unordered map from zobrist keys to fen strings
using fen_map_t = phmap::parallel_flat_hash_map<
    std::uint64_t, std::string, std::hash<std::uint64_t>,
    std::equal_to<std::uint64_t>,
    std::allocator<std::pair<const std::uint64_t, std::string>>, 8, std::mutex>;

fen_map_t fen_map;

// map to collect metadata for tests
using map_meta = std::unordered_map<std::string, TestMetaData>;

std::atomic<std::size_t> total_files = 0;
std::atomic<std::size_t> total_games = 0;
std::atomic<std::size_t> total_pos = 0;

namespace analysis {

/// @brief Magic value for fishtest pgns, ~1.2 million keys
static constexpr int map_size = 1200000;

/// @brief Analyze a file with pgn games and update the position map, apply
/// filter if present
class Analyze : public pgn::Visitor {
public:
  Analyze(const std::string &regex_engine, const std::string &move_counter,
          const bool stop_early, const int max_plies, std::ofstream &out_file,
          const int min_count, const bool save_count,
          std::mutex &progress_output)
      : regex_engine(regex_engine), move_counter(move_counter),
        stop_early(stop_early), max_plies(max_plies), out_file(out_file),
        min_count(min_count), save_count(save_count),
        progress_output(progress_output) {}

  virtual ~Analyze() {}

  void startPgn() override {}

  void header(std::string_view key, std::string_view value) override {

    total_games++;

    if (key == "FEN") {
      std::regex p("0 1$");

      // revert change by cutechess-cli of move counters in .epd books to "0 1"
      if (!move_counter.empty() && std::regex_search(value.data(), p)) {
        board.setFen(std::regex_replace(value.data(), p, "0 " + move_counter));
      } else {
        board.setFen(value);
      }
    }

    if (key == "Variant" && value == "fischerandom") {
      board.set960(true);
    }

    if (key == "Result") {
      hasResult = true;
    }

    if (key == "White") {
      white = value;
    }

    if (key == "Black") {
      black = value;
    }
  }

  void startMoves() override {
    if (!hasResult) {
      this->skipPgn(true);
      return;
    }

    do_filter = !regex_engine.empty();

    if (do_filter) {
      if (white.empty() || black.empty()) {
        this->skipPgn(true);
        return;
      }

      std::regex regex(regex_engine);

      if (std::regex_match(white, regex)) {
        filter_side = Color::WHITE;
      }

      if (std::regex_match(black, regex)) {
        if (filter_side == Color::NONE) {
          filter_side = Color::BLACK;
        } else {
          do_filter = false;
        }
      }
    }
  }

  void move(std::string_view move, std::string_view comment) override {
    if (retained_plies >= max_plies) {
      this->skipPgn(true);
      return;
    }

    Move m;

    m = uci::parseSanInternal(board, move.data(), moves);

    board.makeMove(m);

    if (!do_filter || filter_side == board.sideToMove())
      if (comment != "book") {
        // std::string fen = board.getFen(false);
        std::uint64_t key = board.hash();
        std::uint64_t value;

        bool is_new_entry = zobrist_map.lazy_emplace_l(
            std::move(key),
            [&](zobrist_map_t::value_type &p) { value = ++p.second; },
            [&](const zobrist_map_t::constructor &ctor) {
              ctor(std::move(key), 1);
              value = 1;
            });

        if (value == std::uint64_t(min_count)) {
          total_pos++;
          std::string fen = board.getFen(false);
          if (save_count) {
            fen_map.insert(std::pair(key, fen));
          } else {
            const std::lock_guard<std::mutex> lock(progress_output);
            out_file << fen << "\n";
          }
        }

        if (stop_early && is_new_entry) {
          this->skipPgn(true);
          return;
        }
        retained_plies++;
      }
  }

  void endPgn() override {
    board.set960(false);
    board.setFen(STARTPOS);

    hasResult = false;

    retained_plies = 0;

    filter_side = Color::NONE;

    white.clear();
    black.clear();
  }

private:
  const std::string &regex_engine;
  const std::string &move_counter;
  const bool stop_early;
  const int max_plies;
  std::ofstream &out_file;
  const int min_count;
  const bool save_count;
  std::mutex &progress_output;

  Board board;
  Movelist moves;

  bool skip = false;

  bool hasResult = false;

  bool do_filter = false;
  Color filter_side = Color::NONE;

  std::string white;
  std::string black;

  int retained_plies = 0;
};

void ana_files(const std::vector<std::string> &files,
               const std::string &regex_engine, const map_meta &meta_map,
               bool fix_fens, const int max_plies, const bool stop_early,
               std::ofstream &out_file, const int min_count,
               const bool save_count, std::mutex &progress_output) {

  for (const auto &file : files) {
    std::string move_counter;
    if (fix_fens) {
      std::string test_filename = file.substr(0, file.find_last_of('-'));

      if (meta_map.find(test_filename) == meta_map.end()) {
        std::cout << "Error: No metadata for test " << test_filename
                  << std::endl;
        std::exit(1);
      }

      if (meta_map.at(test_filename).book_depth.has_value()) {
        move_counter =
            std::to_string(meta_map.at(test_filename).book_depth.value() + 1);
      } else {
        if (!meta_map.at(test_filename).book.has_value()) {
          std::cout << "Error: Missing \"book\" key in metadata for test "
                    << test_filename << std::endl;
          std::exit(1);
        }

        std::regex p(".epd");

        if (std::regex_search(meta_map.at(test_filename).book.value(), p)) {
          std::cout << "Error: Missing \"book_depth\" key in metadata for .epd "
                       "book for test "
                    << test_filename << std::endl;
          std::exit(1);
        }
      }
    }

    const auto pgn_iterator = [&](std::istream &iss) {
      auto vis = std::make_unique<Analyze>(
          regex_engine, move_counter, stop_early, max_plies, out_file,
          min_count, save_count, progress_output);

      pgn::StreamParser parser(iss);

      try {
        parser.readGames(*vis);
      } catch (const std::exception &e) {
        std::cout << "Error when parsing: " << file << std::endl;
        std::cerr << e.what() << '\n';
      }
    };

    if (file.size() >= 3 && file.substr(file.size() - 3) == ".gz") {
      igzstream input(file.c_str());
      pgn_iterator(input);
    } else {
      std::ifstream pgn_stream(file);
      pgn_iterator(pgn_stream);
      pgn_stream.close();
    }

    ++total_files;

    // Limit the scope of the lock
    {
      const std::lock_guard<std::mutex> lock(progress_output);

      // Print progress
      std::cout << "\rProcessed " << total_files << " files" << std::flush;
    }
  }
}

} // namespace analysis

[[nodiscard]] map_meta get_metadata(const std::vector<std::string> &file_list,
                                    bool allow_duplicates) {
  map_meta meta_map;
  std::unordered_map<std::string, std::string>
      test_map; // map to check for duplicate tests
  std::set<std::string> test_warned;
  for (const auto &pathname : file_list) {
    fs::path path(pathname);
    std::string directory = path.parent_path().string();
    std::string filename = path.filename().string();
    std::string test_id = filename.substr(0, filename.find_last_of('-'));
    std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

    if (test_map.find(test_id) == test_map.end()) {
      test_map[test_id] = test_filename;
    } else if (test_map[test_id] != test_filename) {
      if (test_warned.find(test_filename) == test_warned.end()) {
        std::cout << (allow_duplicates ? "Warning" : "Error")
                  << ": Detected a duplicate of test " << test_id
                  << " in directory " << directory << std::endl;
        test_warned.insert(test_filename);

        if (!allow_duplicates) {
          std::cout << "Use --allowDuplicates to continue nonetheless."
                    << std::endl;
          std::exit(1);
        }
      }
    }

    // load the JSON data from disk, only once for each test
    if (meta_map.find(test_filename) == meta_map.end()) {
      std::ifstream json_file(test_filename + ".json");

      if (!json_file.is_open())
        continue;

      json metadata = json::parse(json_file);

      meta_map[test_filename] = metadata.get<TestMetaData>();
    }
  }
  return meta_map;
}

void filter_files_book(std::vector<std::string> &file_list,
                       const map_meta &meta_map, const std::regex &regex_book,
                       bool invert) {
  const auto pred = [&regex_book, invert,
                     &meta_map](const std::string &pathname) {
    std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

    // check if metadata and "book" entry exist
    if (meta_map.find(test_filename) != meta_map.end() &&
        meta_map.at(test_filename).book.has_value()) {
      bool match =
          std::regex_match(meta_map.at(test_filename).book.value(), regex_book);
      return invert ? match : !match;
    }

    // missing metadata or "book" entry can never match
    return true;
  };

  file_list.erase(std::remove_if(file_list.begin(), file_list.end(), pred),
                  file_list.end());
}

void filter_files_sprt(std::vector<std::string> &file_list,
                       const map_meta &meta_map) {
  const auto pred = [&meta_map](const std::string &pathname) {
    std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

    // check if metadata and "sprt" entry exist
    if (meta_map.find(test_filename) != meta_map.end() &&
        meta_map.at(test_filename).sprt.has_value()) {
      return false;
    }

    return true;
  };

  file_list.erase(std::remove_if(file_list.begin(), file_list.end(), pred),
                  file_list.end());
}

void process(const std::vector<std::string> &files_pgn,
             const std::string &regex_engine, const map_meta &meta_map,
             bool fix_fens, const int max_plies, const bool stop_early,
             std::ofstream &out_file, const int min_count,
             const bool save_count, int concurrency) {
  // Create more chunks than threads to prevent threads from idling.
  int target_chunks = 4 * concurrency;

  auto files_chunked = split_chunks(files_pgn, target_chunks);

  std::cout << "Found " << files_pgn.size() << " .pgn(.gz) files, creating "
            << files_chunked.size() << " chunks for processing." << std::endl;

  // Mutex for progress output
  std::mutex progress_output;

  // Create a thread pool
  ThreadPool pool(concurrency);

  for (const auto &files : files_chunked) {

    pool.enqueue([&files, &regex_engine, &meta_map, &fix_fens, &progress_output,
                  &files_chunked, &max_plies, &stop_early, &out_file,
                  &min_count, &save_count]() {
      analysis::ana_files(files, regex_engine, meta_map, fix_fens, max_plies,
                          stop_early, out_file, min_count, save_count,
                          progress_output);
    });
  }

  // Wait for all threads to finish
  pool.wait();
}

void print_usage(char const *program_name) {
  std::stringstream ss;

  // clang-format off
    ss << "Usage: " << program_name << " [options]" << "\n";
    ss << "Options:" << "\n";
    ss << "  --file <path>         Path to .pgn(.gz) file" << "\n";
    ss << "  --dir <path>          Path to directory containing .pgn(.gz) files (default: pgns)" << "\n";
    ss << "  -r                    Search for .pgn(.gz) files recursively in subdirectories" << "\n";
    ss << "  --allowDuplicates     Allow duplicate directories for test pgns" << "\n";
    ss << "  --concurrency <N>     Number of concurrent threads to use (default: maximum)" << "\n";
    ss << "  --matchEngine <regex> Filter data based on engine name" << "\n";
    ss << "  --matchBook <regex>   Filter data based on book name" << "\n";
    ss << "  --matchBookInvert     Invert the filter" << "\n";
    ss << "  --SPRTonly            Analyse only pgns from SPRT tests" << "\n";
    ss << "  --fixFEN              Patch move counters lost by cutechess-cli" << "\n";
    ss << "  --maxPlies <N>        Maximum number of plies to consider from the game, excluding book moves (default 20)" << "\n";
    ss << "  --stopEarly           Stop analysing the game as soon as a new position is reached (default false) for the analysing thread." << "\n";
    ss << "  --minCount <N>        Minimum count of the positin before being written to file (default 1)" << "\n";
    ss << "  --saveCount           Add to the output file the count of each position. This adds significant memory overhead." << "\n";
    ss << "  -o <path>             Path to output epd file (default: popular.epd)" << "\n";
    ss << "  --help                Print this help message" << "\n";
  // clang-format on

  std::cout << ss.str();
}

/// @brief
/// @param argc
/// @param argv See print_usage() for possible arguments
/// @return
int main(int argc, char const *argv[]) {
  const std::vector<std::string> args(argv + 1, argv + argc);

  std::vector<std::string> files_pgn;
  std::string regex_engine, regex_book, filename = "popular.epd";
  int max_plies = 20;
  unsigned int min_count = 1;
  int concurrency = std::max(1, int(std::thread::hardware_concurrency()));

  std::vector<std::string>::const_iterator pos;

  if (std::find(args.begin(), args.end(), "--help") != args.end()) {
    print_usage(argv[0]);
    return 0;
  }

  if (find_argument(args, pos, "--concurrency")) {
    concurrency = std::stoi(*std::next(pos));
  }

  if (find_argument(args, pos, "--file")) {
    files_pgn = {*std::next(pos)};
  } else {
    std::string path = "./pgns";

    if (find_argument(args, pos, "--dir")) {
      path = *std::next(pos);
    }

    bool recursive = find_argument(args, pos, "-r", true);
    std::cout << "Looking " << (recursive ? "(recursively) " : "")
              << "for pgn files in " << path << std::endl;

    files_pgn = get_files(path, recursive);
  }

  // sort to easily check for "duplicate" files, i.e. "foo.pgn.gz" and "foo.pgn"
  std::sort(files_pgn.begin(), files_pgn.end());

  for (size_t i = 1; i < files_pgn.size(); ++i) {
    if (files_pgn[i].find(files_pgn[i - 1]) == 0) {
      std::cout << "Error: \"Duplicate\" files: " << files_pgn[i - 1] << " and "
                << files_pgn[i] << std::endl;
      std::exit(1);
    }
  }

  bool allow_duplicates = find_argument(args, pos, "--allowDuplicates", true);
  auto meta_map = get_metadata(files_pgn, allow_duplicates);

  if (find_argument(args, pos, "--SPRTonly", true)) {
    filter_files_sprt(files_pgn, meta_map);
  }

  if (find_argument(args, pos, "--matchBook")) {
    regex_book = *std::next(pos);

    if (!regex_book.empty()) {
      bool invert = find_argument(args, pos, "--matchBookInvert", true);
      std::cout << "Filtering pgn files " << (invert ? "not " : "")
                << "matching the book name " << regex_book << std::endl;
      std::regex regex(regex_book);
      filter_files_book(files_pgn, meta_map, regex, invert);
    }
  }

  if (find_argument(args, pos, "--maxPlies")) {
    max_plies = std::stoi(*std::next(pos));
  }

  bool stop_early = find_argument(args, pos, "--stopEarly", true);
  bool save_count = find_argument(args, pos, "--saveCount", true);

  if (find_argument(args, pos, "--minCount")) {
    min_count = std::stoi(*std::next(pos));
  }

  bool fix_fens = find_argument(args, pos, "--fixFEN", true);

  if (find_argument(args, pos, "--matchEngine")) {
    regex_engine = *std::next(pos);
  }

  if (find_argument(args, pos, "-o")) {
    filename = *std::next(pos);
  }

  std::ofstream out_file(filename);

  const auto t0 = std::chrono::high_resolution_clock::now();

  process(files_pgn, regex_engine, meta_map, fix_fens, max_plies, stop_early,
          out_file, min_count, save_count, concurrency);

  if (save_count) {
    for (const auto &pair : fen_map) {
      out_file << pair.second << " ; c0 " << zobrist_map[pair.first] << "\n";
    }
  }

  out_file.close();

  const auto t1 = std::chrono::high_resolution_clock::now();

  std::cout << "\nFound " << total_pos << " positions in " << total_games << " games."
            << "\nTotal time for processing: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                       .count() /
                   1000.0
            << "s" << std::endl;

  return 0;
}
