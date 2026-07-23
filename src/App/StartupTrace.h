#pragma once

#include <string_view>

namespace openreplay::ui {

void TraceStartup(std::wstring_view message) noexcept;

}  // namespace openreplay::ui
