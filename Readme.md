# Find popular chess positions

Given a set of .pgn files (or .pgn.gz), quickly find the most commonly played positions.

Example usage:

```
$ ./fastpopular --dir pgns -r --minCount 8 --stopEarly --countStopEarly 6 --maxPlies 60
Looking (recursively) for pgn files in pgns
Found 830 .pgn([.gz|.zst]) files, creating 93 chunks for processing.
Processed 830 files
Retained 54671041 positions from 554007242 unique visited in 1981339655 visits in 92445853 games.
Positions written to popular.epd.
Total time for processing: 728.859 s
```

This analyzes all games in the given directory (and all its subdirectories), analyzing games at most 60 plies deep out of the book,
skipping the remainder of the game if 6 new positions were found, and writing eventually all positions that have been seen 8 times or more.

The analysis runs multi-threaded, typically limited by the speed of the storage.

```
Usage: ./fastpopular [options]
Options:
  --file <path>         Path to .pgn([.gz|.zst]) file
  --dir <path>          Path to directory containing .pgn([.gz|.zst]) files (default: pgns)
  -r                    Search for .pgn([.gz|.zst]) files recursively in subdirectories
  --noFRC               Exclude (D)FRC games (included by default)
  --allowDuplicates     Allow duplicate directories for test pgns
  --concurrency <N>     Number of concurrent threads to use (default: maximum)
  --matchEngine <regex> Filter data based on engine name
  --matchBook <regex>   Filter data based on book name
  --matchBookInvert     Invert the filter
  --SPRTonly            Analyse only pgns from SPRT tests
  --fixFEN              Patch move counters lost by cutechess-cli
  --maxPlies <N>        Maximum number of plies to consider from the game, excluding book moves (default 20)
  --stopEarly           Stop analysing the game as soon as countStopEarly new positions are reached (default false) for the analysing thread.
  --countStopEarly <N>  Number of new positions encountered before stopping with stopEarly (default 1)
  --minCount <N>        Minimum count of the position before being written to file (default 1). Use N=0 to simply parse the games, without writing positions to file.
  -o <path>             Path to output epd file (default: popular.epd)
  --omitMoveCounter     Omit movecounter when storing the FEN (the same position with different movecounters is in any case only stored once)
  --saveCount           Output the count of each stored position without movecounter in the file popular_sorted.epd.
  --cdfCutoff <P>       Output the counts only of the P% most popular positions, weighted by their counts. (default: 100.0)
  --TBlimit <N>         Omit positions with N pieces, or fewer (default: 1)
  --omitMates           Omit positions without a legal move (check/stale mates)
  --minElo <N>          Omit games where WhiteElo or BlackElo < minElo (default: 0)
  --cdb                 Shorthand for --TBlimit 7 --omitMates
  --help                Print this help message
```

The code is based on a [related project](https://github.com/official-stockfish/WDL_model) 
