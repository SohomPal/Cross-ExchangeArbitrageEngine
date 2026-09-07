#pragma once

namespace core {

enum class BookState {
    Initializing,
    Valid,
    Stale,
    Resyncing,
    Disconnected,
    Invalid
};

} // namespace core
