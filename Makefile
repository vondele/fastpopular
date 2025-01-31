CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -g

NATIVE = -march=native

# Detect Windows
ifeq ($(OS), Windows_NT)
	uname_S  := Windows
else
ifeq ($(COMP), MINGW)
	uname_S  := Windows
else
	uname_S := $(shell uname -s)
endif
endif

ifeq ($(uname_S), Darwin)
	NATIVE =	
endif

SRC_FILE = fastpopular.cpp
EXE_FILE = fastpopular
HEADERS = fastpopular.hpp
EXT_HEADERS = external/chess.hpp external/json.hpp external/threadpool.hpp external/parallel_hashmap/phmap.h fastpopular.hpp
EXT_BXZSTR_HEADERS = external/bxzstr/bxzstr.hpp external/bxzstr/config.hpp external/bxzstr/strict_fstream.hpp external/bxzstr/bz_stream_wrapper.hpp external/bxzstr/lzma_stream_wrapper.hpp external/bxzstr/zstd_stream_wrapper.hpp external/bxzstr/compression_types.hpp external/bxzstr/stream_wrapper.hpp external/bxzstr/z_stream_wrapper.hpp
EXT_HEADERS += $(EXT_BXZSTR_HEADERS)

all: $(EXE_FILE)

$(EXE_FILE): $(SRC_FILE) $(HEADERS) $(EXT_HEADERS)
	$(CXX) $(CXXFLAGS) $(NATIVE) -o $(EXE_FILE) $(SRC_FILE) -lz -lzstd

format:
	clang-format -i $(SRC_FILE) $(HEADERS)

clean:
	rm -f $(EXE_FILE) $(EXE_FILE).exe
