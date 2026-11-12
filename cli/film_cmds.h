#pragma once
#include <string>
#include <vector>

namespace dapple::filmtool {
// film | film-pack | film-bench | film-render-bench | film-compare | film-info. Returns the process exit code.
int run(const std::string& cmd, const std::vector<std::string>& args);
bool handles(const std::string& cmd);
// Points the develop pipeline at the film pack (DAPPLE_FILM_PACK or core/data/film/dapple.dfp near the cwd), lazily:
// nothing is read until an edit asks for a film. No-op when no pack is found.
void installDefaultLibrary();
}  // namespace dapple::filmtool
