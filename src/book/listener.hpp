#pragma once

// BookListener interface (DESIGN §6): a compile-time template parameter on
// the engine, never virtual. Derive from NullListener and shadow the handlers
// you care about; unimplemented events cost nothing.

#include "core/types.hpp"

namespace ob::book {

struct NullListener {
    // A displayed order joined the book.
    void on_add(StockLocate, OrderId, Side, Price, Qty) {}
    // Execution against a resting order: (..., exec_price, qty, printable).
    // exec_price may improve on the resting price (C messages);
    // printable=false must stay out of volume.
    void on_exec(StockLocate, OrderId, Side, Price, Qty, bool) {}
    // Shares leaving the book without trading (X partial / D full / U orig leg).
    void on_cancel(StockLocate, OrderId, Side, Price, Qty) {}
    // Aggregate qty at (side, price) changed; zero means the level is gone.
    void on_level_change(StockLocate, Side, Price, Qty) {}
};

}  // namespace ob::book
