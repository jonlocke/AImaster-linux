CXX = g++
CXXFLAGS = -Wall -std=c++17
LIBS = -lserialport -ljsoncpp -lcurl -lreadline

SRC = src/utils.cpp src/main.cpp src/config_loader.cpp src/serial_handler.cpp src/ollama_client.cpp
OBJ = $(SRC:.cpp=.o)
INCLUDE = -Iinclude

TARGET = ollama_cli

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
