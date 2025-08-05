CXX = g++
CXXFLAGS = -Wall -std=c++17 -Iinclude
LDFLAGS = -lserialport -ljsoncpp -lcurl -lreadline

OBJS = src/utils.o \
       src/main.o \
       src/config_loader.o \
       src/serial_handler.o \
       src/ollama_client.o \
       src/chat_session.o

TARGET = ollama_cli

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/utils.o: src/utils.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/config_loader.o: src/config_loader.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/serial_handler.o: src/serial_handler.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/ollama_client.o: src/ollama_client.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/chat_session.o: src/chat_session.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
