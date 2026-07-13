#pragma once

/// \file halcyon.hpp
/// \brief Umbrella header for the Halcyon C++17 IBM Db2 client library.
///
/// Include this single header to pull in the complete public API:
/// Database, Connection, Transaction, ResultSet, Result<T>, Error,
/// PoolConfig, Batch/batchOf, and the async entry points.
#include "halcyon/async.hpp"
#include "halcyon/connection.hpp"
#include "halcyon/database.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/observability/config.hpp"
#include "halcyon/observability/logging.hpp"
#include "halcyon/observability/metrics.hpp"
#include "halcyon/observability/tracing.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"
#include "halcyon/version.hpp"
