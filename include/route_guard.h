#ifndef ROUTE_GUARD_H
#define ROUTE_GUARD_H

// Minimal routing guard that temporarily sets the command source to SERIAL
// for the duration of a scope, then restores it.
//
// Your project must provide these two functions and the enum (or identical types):
//   enum class CommandSource { CONSOLE = 0, SERIAL = 1 };
//   CommandSource getCurrentCommandSource();
//   void setCurrentCommandSource(CommandSource);
//
// If your enum uses different names/values, adjust below accordingly.

enum class CommandSource { CONSOLE = 0, SERIAL = 1 };

// These must be implemented in your project.
CommandSource getCurrentCommandSource();
void setCurrentCommandSource(CommandSource);

struct RouteGuard {
    CommandSource prev;
    explicit RouteGuard(CommandSource now) : prev(getCurrentCommandSource()) {
        setCurrentCommandSource(now);
    }
    ~RouteGuard() { setCurrentCommandSource(prev); }
};

#endif // ROUTE_GUARD_H
