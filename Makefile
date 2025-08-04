# Compiler
CXX = g++
CXXFLAGS = -Wall -std=c++17

# Libraries
LIBS = -lserialport -ljsoncpp

# Project files
SRC = main.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = ollama_cli

# Build target
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build files
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean

