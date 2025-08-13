
How to enable INT over serial
=============================

1) Replace your files:
   - include/command_exec.h
   - src/command_exec.cpp
   - include/ollama_client.h
   - src/ollama_client.cpp   (patched)

2) Update your serial listener in src/main.cpp:
   startSerialListener([&](const std::string& line) {
       try {
           if (SerialINT_IsActive()) {
               SerialINT_HandleLine(line, config);
           } else {
               (void)execute_command(line, config, CommandSource::SERIAL);
           }
       } catch (...) {
           std::cerr << "[Warning] exception in serial command handler\n";
       }
   }, 50);

3) Rebuild:
   make clean && make
