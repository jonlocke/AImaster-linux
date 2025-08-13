CXX = g++
CXXFLAGS = -Wall -std=c++17 -Iinclude -I/usr/include/poppler/cpp
LDFLAGS = -lserialport -ljsoncpp -lcurl -lreadline -lpoppler-cpp -ltesseract

TARGET = AImaster

OBJS = \
  src/utils.o \
  src/main.o \
  src/config_loader.o \
  src/serial_handler.o \
  src/ollama_client.o \
  src/rag_session.o \
  src/rag_adapter.o \
  src/rag_int_bridge.o \
  src/command_exec.o \
  src/rag_state.o 

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Generic compile rule for any src/*.cpp -> src/*.o
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
