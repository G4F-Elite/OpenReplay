#pragma once

#include "openreplay/Types.h"

#include <chrono>
#include <string>

namespace openreplay {

class CaptureStateMachine {
public:
    void SetDesired(bool enabled);
    void MarkAttemptStarted();
    void MarkRunning();
    void MarkDegraded(std::string reason);
    void MarkTransientFailure(std::string reason);
    void MarkPermanentFailure(std::string reason);
    void MarkDisplayMissing();
    void MarkDisplayAvailable();
    void MarkRetryDue();

    [[nodiscard]] bool desired() const noexcept { return desired_; }
    [[nodiscard]] CaptureState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::chrono::seconds retry_delay() const noexcept;

private:
    bool desired_{false};
    CaptureState state_{CaptureState::Disabled};
    std::uint32_t retry_count_{0};
    std::string error_;
};

}  // namespace openreplay
