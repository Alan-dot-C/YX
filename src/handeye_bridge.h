#pragma once

#include <string>

// Used by yx when a word of kind "open" is executed in-process.
// Returns 0 on success, 3 on ShellExecute failure.
int HandeyeOpen(const std::wstring& target, const std::wstring& args);
