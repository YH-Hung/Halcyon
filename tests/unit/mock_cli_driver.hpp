#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::testing {

class MockCliDriver final : public detail::cli::ICliDriver {
public:
    using ConnectionHandle = detail::cli::ConnectionHandle;
    using ConnectionParams = detail::cli::ConnectionParams;

    // If non-empty, the next connect() pops and returns this error instead.
    std::deque<Error> connectErrors;
    // Controls what isAlive() reports for the next call(s); defaults to true.
    std::deque<bool> aliveResults;

    int connectCalls = 0;
    int disconnectCalls = 0;
    int aliveCalls = 0;
    std::vector<ConnectionParams> connectParams;

    Result<ConnectionHandle> connect(const ConnectionParams& params) override {
        ++connectCalls;
        connectParams.push_back(params);
        if (!connectErrors.empty()) {
            Error e = connectErrors.front();
            connectErrors.pop_front();
            return Result<ConnectionHandle>(e);
        }
        return Result<ConnectionHandle>(
            static_cast<ConnectionHandle>(++nextHandle_));
    }

    Result<void> disconnect(ConnectionHandle handle) override {
        ++disconnectCalls;
        (void)handle;
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle handle) override {
        ++aliveCalls;
        (void)handle;
        if (!aliveResults.empty()) {
            bool v = aliveResults.front();
            aliveResults.pop_front();
            return Result<bool>(v);
        }
        return Result<bool>(true);
    }

private:
    std::uint64_t nextHandle_ = 0;
};

}  // namespace halcyon::testing
