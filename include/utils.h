#ifndef UTILS_H
#define UTILS_H

#include <string>

// Lets the user pick a file interactively from defaultDir
// Returns the selected full path, or empty string if cancelled
std::string pickFile(const std::string& defaultDir);

#endif
