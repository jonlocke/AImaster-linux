#ifndef IO_SINK_H
#define IO_SINK_H

#include <string>
#include <iostream>

// Project-provided symbols:
extern bool serial_available;
void serialSend(const std::string& data);

// Simple unified sink: serial if available, else console.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void write(const std::string& s) = 0;
    virtual void flush() {}
};

class SerialSink : public OutputSink {
public:
    void write(const std::string& s) override { serialSend(s); }
};

class ConsoleSink : public OutputSink {
public:
    void write(const std::string& s) override { std::cout << s; }
    void flush() override { std::cout.flush(); }
};

inline OutputSink& sink() {
    static SerialSink ser;
    static ConsoleSink con;
    return serial_available ? static_cast<OutputSink&>(ser)
                            : static_cast<OutputSink&>(con);
}

inline void out(const std::string& s) { sink().write(s); }
inline void outln(const std::string& s) { sink().write(s + "\r\n"); }
inline void emit_prompt() { sink().write("-> "); sink().flush(); }

#endif // IO_SINK_H
