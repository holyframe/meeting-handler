#pragma once

#include <QString>
#include <functional>
#include <optional>
#include <stop_token>

namespace windows_captions {

struct Result {
    std::optional<bool> enabled;
    QString error;
};

struct Backend {
    std::function<bool()> windowOpen;
    std::function<Result(bool enabled, std::stop_token stop)> setEnabled;
};

inline constexpr auto settingsUri = "ms-settings:easeofaccess-closedcaptioning";
Backend systemBackend();

}
