#include "openreplay/CaptureStateMachine.h"

#include <algorithm>
#include <utility>

namespace openreplay {

void CaptureStateMachine::SetDesired(bool enabled) {
    desired_ = enabled;
    if (!enabled) {
        state_ = CaptureState::Disabled;
        retry_count_ = 0;
        error_.clear();
    } else if (state_ == CaptureState::Disabled || state_ == CaptureState::Failed) {
        state_ = CaptureState::Starting;
        retry_count_ = 0;
        error_.clear();
    }
}

void CaptureStateMachine::MarkAttemptStarted() { if (desired_) state_ = CaptureState::Starting; }

void CaptureStateMachine::MarkRunning() {
    if (!desired_) return;
    state_ = CaptureState::Running;
    retry_count_ = 0;
    error_.clear();
}

void CaptureStateMachine::MarkDegraded(std::string reason) {
    if (!desired_) return;
    state_ = CaptureState::Degraded;
    error_ = std::move(reason);
}

void CaptureStateMachine::MarkTransientFailure(std::string reason) {
    if (!desired_) return;
    state_ = CaptureState::Recovering;
    error_ = std::move(reason);
    ++retry_count_;
}

void CaptureStateMachine::MarkPermanentFailure(std::string reason) {
    if (!desired_) return;
    state_ = CaptureState::Failed;
    error_ = std::move(reason);
}

void CaptureStateMachine::MarkDisplayMissing() { if (desired_) state_ = CaptureState::WaitingForDisplay; }

void CaptureStateMachine::MarkDisplayAvailable() {
    if (desired_ && state_ == CaptureState::WaitingForDisplay) state_ = CaptureState::Starting;
}

void CaptureStateMachine::MarkRetryDue() {
    if (desired_ && state_ == CaptureState::Recovering) state_ = CaptureState::Starting;
}

std::chrono::seconds CaptureStateMachine::retry_delay() const noexcept {
    const auto exponent = std::min(retry_count_, 5u);
    return std::chrono::seconds{std::min(1u << exponent, 30u)};
}

}  // namespace openreplay
