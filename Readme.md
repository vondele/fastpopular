# Find popular chess positions

Given a set of .pgn files (or .pgn.gz), quickly find the most commonly played positions.

Example usage:

```
$ ./fastpopular -r --dir chess/fishtestgames/download33 --stopEarly --minCount 3 --maxPlies 20
Looking (recursively) for pgn files in chess/fishtestgames/download33
Found 778329 .pgn(.gz) files, creating 128 chunks for processing.
Progress: 128/128
Time taken: 5797s
Wrote 7423674 scored positions to popular.epd for analysis.
```

This analyzes all games in the given directory (and all its subdirectories), analyzing games at most 20 plies deep out of the book,
skipping the remainder of the game if a new position was found, and writing eventually all positions that have been seen 3 times or more.

The analysis runs multi-threaded, typically limited by the speed of the storage.

```
Options:
  --file <path>         Path to .pgn(.gz) file
  --dir <path>          Path to directory containing .pgn(.gz) files (default: pgns)
  -r                    Search for .pgn(.gz) files recursively in subdirectories
  --allowDuplicates     Allow duplicate directories for test pgns
  --matchEngine <regex> Filter data based on engine name
  --matchBook <regex>   Filter data based on book name
  --matchBookInvert     Invert the filter
  --SPRTonly            Analyse only pgns from SPRT tests
  --fixFEN              Patch move counters lost by cutechess-cli
  --maxPlies <N>        Maximum number of plies to consider from the game, excluding book moves (default 20)
  --stopEarly           Stop analysing the game as soon as a new position is reached (default false) for the analysing thread.
  --minCount <N>        Minimum count of the positin before being written to file (default 1)
  -o <path>             Path to output epd file (default: popular.epd)
  --help                Print this help message
```

The code is based on a [related project](https://github.com/official-stockfish/WDL_model) 
