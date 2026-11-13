#pragma once
#include <string>
// Background film tables (wave 8): listeners are told when a table the frame thread asked for has landed and the frames since then showed the previous
// step's table, so the view / host can repaint without any further input. Called on the table worker thread; keep the callback short.
namespace dapple::engine::filmnode {
using TableLandedFn = void (*)(void* user);
void addTableLandedListener(TableLandedFn fn, void* user);
void removeTableLandedListener(TableLandedFn fn, void* user);
// frames since the last landing that used a stale table (true until a table lands), for tests and the log
bool filmTablesStale();
// the film match of a frame was the previous result while a background refresh runs: markMatchStale before queueing, notifyRepaintIfMatchStale when it landed
void markMatchStale();
void notifyRepaintIfMatchStale();

struct FilmResolved;
// Stock selection (wave 8): are the raw / curve / print tables of this film frame cached, and queue their bake (+ the plan) on the background pool.
bool tablesCachedFor(const FilmResolved& r);
void requestStockPrep(const FilmResolved& r);
// Film tables on disk (wave 9, film_tables.cpp): the directory (empty = off; DAPPLE_FILM_TABLE_DIR when never set) and the build salt (app version + fingerprint) the
// file keys include; the writer thread's backlog can be drained (tests); counters of loads that hit / missed, files written, corrupt files deleted.
void setFilmTableDisk(const std::string& dir, const std::string& salt);
void filmTableDiskDrain();
void filmTableDiskStats(unsigned long long* hits, unsigned long long* misses, unsigned long long* stores, unsigned long long* corrupt);
}  // namespace dapple::engine::filmnode
