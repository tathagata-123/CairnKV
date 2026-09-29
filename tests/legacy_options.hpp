#pragma once

#include "minilsm/options.hpp"

// Original milestone tests assert synchronous installation and exact read
// counts. Keep those fixtures deterministic; db_revision_test exercises the
// new default asynchronous/cache/compression configuration separately.
inline minilsm::Options LegacyOptions() {
    minilsm::Options options;
    options.background_maintenance = false;
    options.legacy_compaction_for_testing = true;
    options.paranoid_checks = true;
    options.block_cache_bytes = 0U;
    options.prefix_compression = false;
    return options;
}
