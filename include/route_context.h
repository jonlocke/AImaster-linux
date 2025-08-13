#pragma once
#include "command_exec.h"

// Minimal API to set the routing source for the current thread.
// Use this ONLY around LLM streaming so replies route correctly for INT vs console.
void setCurrentCommandSource(CommandSource s); // implemented in route_context.cpp
