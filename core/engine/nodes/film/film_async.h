#pragma once
// Background film tables (wave 8): listeners are told when a table the frame thread asked for has landed and the frames since then showed the previous
// step's table, so the view / host can repaint without any further input. Called on the table worker thread; keep the callback short.
namespace dapple::engine::filmnode {
using TableLandedFn = void (*)(void* user);
void addTableLandedListener(TableLandedFn fn, void* user);
void removeTableLandedListener(TableLandedFn fn, void* user);
// frames since the last landing that used a stale table (true until a table lands), for tests and the log
bool filmTablesStale();
}  // namespace dapple::engine::filmnode
