// The DevX single-page UI, served from memory.
//
// Inlined rather than read from disk so the binary is self-contained: there is
// no asset path to get wrong and nothing to install alongside it.
#pragma once

#include <string>

namespace mpi::devx {

// The page, with `token` substituted so every fetch the UI makes is
// authenticated without the operator pasting anything.
std::string index_html(const std::string& token, const std::string& version,
                       const std::string& ruleset);

}  // namespace mpi::devx
