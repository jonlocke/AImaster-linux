CXX = g++
CXXFLAGS = -Wall -std=c++17 -Iinclude -I/usr/include/poppler/cpp -I/usr/include/jsoncpp
LDFLAGS = -lserialport -ljsoncpp -lcurl -lreadline -lpoppler-cpp -ltesseract

TARGET = AImaster

OBJS = \
  src/utils.o \
  src/main.o \
  src/config_loader.o \
  src/serial_handler.o \
  src/chat_provider.o \
  src/ollama_client.o \
  src/linux_integrations.o \
  src/tts.o \
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

TEST_TARGET = chat_provider_tests
TEST_OBJS = src/chat_provider.o src/config_loader.o src/tts.o tests/chat_provider_tests.o tests/tts_tests.o

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) -ljsoncpp -lcurl

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)
