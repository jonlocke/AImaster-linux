CXX = g++
CXXFLAGS = -Wall -std=c++17 -Iinclude -I/usr/include/poppler/cpp
LDFLAGS = -lserialport -ljsoncpp -lcurl -lreadline -lpoppler-cpp -ltesseract

OBJS = src/utils.o \
 src/main.o \
 src/config_loader.o \
 src/serial_handler.o \
 src/ollama_client.o \
 \
 src/rag_session.o \
 src/rag_adapter.o \
 src/rag_state.o

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

 \
 src/rag_session.o \
 src/rag_adapter.o \
 src/rag_state.o: 
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
