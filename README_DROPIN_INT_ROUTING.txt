INT routing drop-in (minimal)
================================

This drop-in provides ONE tiny setter so the LLM streaming path can route
output correctly (INT -> serial, console -> console) without using any global override.

Files included
--------------
- include/route_context.h
- src/route_context.cpp

Build
-----
Your Makefile already compiles everything in src/ and includes -Iinclude.
No other build changes needed.

How to apply (one small edit in your existing code)
---------------------------------------------------
1) Include the header at the top of src/ollama_client.cpp:
   #include "route_context.h"

2) In sendMessageToOllama(...), wrap the streaming call so the thread's source
   matches the caller. Replace ONLY the routing-related lines as shown below.

PATCH (copy-paste inside sendMessageToOllama, before curl_easy_perform):
-----------------------------------------------------------------------
    // Route replies according to the caller’s source and restore on exit
    const CommandSource prev = getCurrentCommandSource();
    setCurrentCommandSource(prev);        // assert caller’s route for this thread

    route_output("[Thinking..]", true);

    // ... curl setup ...
    CURLcode res = curl_easy_perform(curl);

    // ... after cleanup and before return ...
    setCurrentCommandSource(prev);

Why this works
--------------
- curl_easy_perform runs the write callback on the SAME THREAD for the easy API.
- route_output already decides destination based on the current thread's source.
- We simply assert the caller's source during streaming, then restore it.

If you still have DIAG lines, you should now see:
- For INT:  to=s src=1 for Thinking.., tokens, and final response
- For console ASK: to=c src=0 for all output
