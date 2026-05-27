CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

UNAME_S := $(shell uname -s 2>/dev/null)

# ─── macOS ────────────────────────────────────────────────────────────────
ifeq ($(UNAME_S),Darwin)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ifneq ($(BREW_PREFIX),)
    NLOHMANN := $(shell brew --prefix nlohmann-json 2>/dev/null)
    CURLPFX  := $(shell brew --prefix curl 2>/dev/null)
    # `brew --prefix X` returns a path even if X isn't installed; verify the
    # header/lib actually exist before adding -I/-L (avoids ld warnings).
    ifneq ($(wildcard $(NLOHMANN)/include/nlohmann/json.hpp),)
      CXXFLAGS += -I$(NLOHMANN)/include
    endif
    ifneq ($(wildcard $(CURLPFX)/include/curl/curl.h),)
      CXXFLAGS += -I$(CURLPFX)/include
      LDFLAGS  += -L$(CURLPFX)/lib
    endif
  endif
  LDFLAGS += -lcurl -lpthread
endif

# ─── Linux ────────────────────────────────────────────────────────────────
ifeq ($(UNAME_S),Linux)
  CXX := g++
  # pkg-config is the canonical way to find libcurl on Linux distros.
  CXXFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
  LDFLAGS  += $(shell pkg-config --libs   libcurl 2>/dev/null)
  # If pkg-config didn't find anything, fall back to -lcurl from default lib paths.
  ifeq ($(strip $(LDFLAGS)),)
    LDFLAGS += -lcurl
  endif
  LDFLAGS += -lpthread
  # nlohmann-json is usually at /usr/include/nlohmann/json.hpp (apt/dnf default).
endif

# ─── Windows (MSYS2 / mingw-w64) ──────────────────────────────────────────
# Detect MSYS/MINGW build environment. Compile with g++ from mingw-w64;
# clipboard uses Win32 API so we link user32. libcurl + nlohmann-json come
# from MSYS2 packages: mingw-w64-ucrt-x86_64-{curl,nlohmann-json}.
ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
  CXX := g++
  CXXFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
  LDFLAGS  += $(shell pkg-config --libs   libcurl 2>/dev/null)
  LDFLAGS  += -luser32 -lpthread
endif

BIN := ytmerge
SRC := src/ytmerge.cpp

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN) $(BIN).exe

install: $(BIN)
	mkdir -p $(HOME)/.local/bin
	cp $(BIN) $(HOME)/.local/bin/$(BIN)
	chmod +x $(HOME)/.local/bin/$(BIN)

.PHONY: all clean install
