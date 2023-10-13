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

// unordered map to count fens
using map_t = phmap::parallel_flat_hash_map<
    std::string, std::uint64_t, std::hash<std::string>,
    std::equal_to<std::string>,
    std::allocator<std::pair<const std::string, std::uint64_t>>, 8, std::mutex>;

map_t pos_map;

// map to collect metadata for tests
using map_meta = std::unordered_map<std::string, TestMetaData>;

std::atomic<std::size_t> total_chunks = 0;

namespace analysis {

/// @brief Magic value for fishtest pgns, ~1.2 million keys
static constexpr int map_size = 1200000;

/// @brief Analyze a file with pgn games and update the position map, apply filter if present
class Analyze : public pgn::Visitor {
   public:
    Analyze(const std::string &regex_engine, const std::string &move_counter, const bool stop_early, const int max_plies)
        : regex_engine(regex_engine), move_counter(move_counter), stop_early(stop_early), max_plies(max_plies) {}

    virtual ~Analyze() {}

    void startPgn() override {}

    void startMoves() override {
        do_filter = !regex_engine.empty();

        if (do_filter) {
            if (white.empty() || black.empty()) {
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

    void header(std::string_view key, std::string_view value) override {
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
            hasResult  = true;
            goodResult = true;

            if (value == "1-0") {
                resultkey.white = Result::WIN;
                resultkey.black = Result::LOSS;
            } else if (value == "0-1") {
                resultkey.white = Result::LOSS;
                resultkey.black = Result::WIN;
            } else if (value == "1/2-1/2") {
                resultkey.white = Result::DRAW;
                resultkey.black = Result::DRAW;
            } else {
                goodResult = false;
            }
        }

        if (key == "Termination") {
            if (value == "time forfeit" || value == "abandoned") {
                goodTermination = false;
            }
        }

        if (key == "White") {
            white = value;
        }

        if (key == "Black") {
            black = value;
        }

        skip = !(hasResult && goodTermination && goodResult);
        skip = !hasResult; /* TODO */
    }

    void move(std::string_view move, std::string_view comment) override {
        if (skip) {
            return;
        }

        if (retained_plies >= max_plies)
            return;

        Move m;

        m = uci::parseSanInternal(board, move.data(), moves);

        board.makeMove(m);

        if (!do_filter || filter_side == board.sideToMove())
          if (comment != "book") {
            std::string fen = board.getFen();
            bool is_new_entry = pos_map.lazy_emplace_l(std::move(fen),
                                     [&](map_t::value_type& p) { ++p.second; },
                                     [&](const map_t::constructor& ctor) { ctor(std::move(fen), 1); });
            if (stop_early && is_new_entry) {
                skip = true;
                return;
            }
            retained_plies++;
          }
    }

    void endPgn() override {
        board.set960(false);
        board.setFen(STARTPOS);

        goodTermination = true;
        hasResult       = false;
        goodResult      = false;

        retained_plies  = 0;

        filter_side = Color::NONE;

        white.clear();
        black.clear();
    }

   private:
    const std::string &regex_engine;
    const std::string &move_counter;
    const bool stop_early;
    const int max_plies;

    Board board;
    Movelist moves;

    bool skip = false;

    bool goodTermination = true;
    bool hasResult       = false;
    bool goodResult      = false;

    bool do_filter    = false;
    Color filter_side = Color::NONE;

    std::string white;
    std::string black;

    ResultKey resultkey;

    int retained_plies = 0;
};

void ana_files(const std::vector<std::string> &files,
               const std::string &regex_engine, const map_meta &meta_map,
               bool fix_fens, const int max_plies, const bool stop_early) {

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
        auto vis = std::make_unique<Analyze>(regex_engine, move_counter, stop_early, max_plies);

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
             int concurrency) {
  // Create more chunks than threads to prevent threads from idling.
  int target_chunks = 4 * concurrency;

  auto files_chunked = split_chunks(files_pgn, target_chunks);

  std::cout << "Found " << files_pgn.size() << " .pgn(.gz) files, creating "
            << files_chunked.size() << " chunks for processing." << std::endl;

  // Mutex for progress output
  std::mutex progress_output;

  // Create a thread pool
  ThreadPool pool(concurrency);

  // Print progress
  std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size()
            << std::flush;

  for (const auto &files : files_chunked) {

    pool.enqueue([&files, &regex_engine, &meta_map, &fix_fens, &progress_output,
                  &files_chunked, &max_plies, &stop_early]() {
      analysis::ana_files(files, regex_engine, meta_map, fix_fens,
                          max_plies, stop_early);

      total_chunks++;

      // Limit the scope of the lock
      {
        const std::lock_guard<std::mutex> lock(progress_output);

        // Print progress
        std::cout << "\rProgress: " << total_chunks << "/"
                  << files_chunked.size() << std::flush;
      }
    });
  }

  // Wait for all threads to finish
  pool.wait();
}

/// @brief Save the position map to a json file.
/// @param pos_map
/// @param json_filename
void save(const std::string &filename,
          const unsigned int min_count) {

  const auto t0 = std::chrono::high_resolution_clock::now();

  std::uint64_t total = 0;

  std::ofstream out_file(filename);
  for (const auto &pair : pos_map) {
    if (pair.second >= min_count) {
      out_file << pair.first << " ; c0 " << pair.second << std::endl;
      total++;
    }
  }
  out_file.close();

  const auto t1 = std::chrono::high_resolution_clock::now();

  std::cout << "Wrote " << total << " scored positions to " << filename
            << " for analysis in " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0 << "s" << std::endl;
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

  const auto t0 = std::chrono::high_resolution_clock::now();

  process(files_pgn, regex_engine, meta_map, fix_fens, max_plies,
          stop_early, concurrency);

  const auto t1 = std::chrono::high_resolution_clock::now();

  std::cout << "\nTime taken for processing: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0
            << "s" << std::endl;

  save(filename, min_count);

  return 0;
}
