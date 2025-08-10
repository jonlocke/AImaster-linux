# ---- toolchain ----
CXX       ?= g++
CXXFLAGS  ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS  += -Iinclude -Isrc

# pkg-config cflags
CXXFLAGS  += $(shell pkg-config --cflags poppler-cpp 2>/dev/null)
CXXFLAGS  += $(shell pkg-config --cflags jsoncpp 2>/dev/null)
CXXFLAGS  += $(shell pkg-config --cflags libserialport 2>/dev/null)

# ---- link libs ----
LDLIBS    += -lcurl -ltesseract
LDLIBS    += $(shell pkg-config --libs poppler-cpp 2>/dev/null)
LDLIBS    += $(shell pkg-config --libs jsoncpp 2>/dev/null)
LDLIBS    += $(shell pkg-config --libs libserialport 2>/dev/null)

# Fallbacks if pkg-config entries aren’t found
LDLIBS    += -ljsoncpp -lserialport
# readline often lacks pkg-config; add explicitly (and terminfo)
LDLIBS    += -lreadline -lhistory -lncurses -ltinfo

# ---- targets ----
AIMASTER_TARGET ?= aimaster
RAG_DEMO_TARGET ?= rag_demo

# RAG module sources
RAG_SRC := \
    src/rag_session.cpp \
    src/rag_adapter.cpp

# Standalone demo
RAG_DEMO_SRC := examples/rag_demo.cpp

# AImaster sources = all src/*.cpp except RAG files
AIMASTER_SRC := $(filter-out $(RAG_SRC), $(wildcard src/*.cpp))

# Objects
RAG_OBJ        := $(RAG_SRC:.cpp=.o)
RAG_DEMO_OBJ   := $(RAG_DEMO_SRC:.cpp=.o)
AIMASTER_OBJ   := $(AIMASTER_SRC:.cpp=.o)

.PHONY: all clean print
all: $(AIMASTER_TARGET) $(RAG_DEMO_TARGET)

$(AIMASTER_TARGET): $(AIMASTER_OBJ) $(RAG_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(RAG_DEMO_TARGET): $(RAG_DEMO_OBJ) $(RAG_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(AIMASTER_TARGET) $(RAG_DEMO_TARGET) \
	      $(AIMASTER_OBJ) $(RAG_OBJ) $(RAG_DEMO_OBJ)

print:
	@echo "AIMASTER_SRC = $(AIMASTER_SRC)"
	@echo "RAG_SRC      = $(RAG_SRC)"
	@echo "LDLIBS       = $(LDLIBS)"
