#pragma once

#include <string>
#include <vector>

enum class WordKind {
    Open,   // ShellExecute a target (settings page, tool, URI)
    Exec,   // run a fixed command through cmd.exe and capture output
};

struct Word {
    const wchar_t* name;
    const wchar_t* category;
    const wchar_t* desc;
    WordKind       kind;
    const wchar_t* target;
    const wchar_t* args;    // only for Open; may be nullptr
    const wchar_t* alias;   // '|' separated; may be nullptr
};

const std::vector<Word>& AllWords();
const Word* FindWord(const std::wstring& name);
