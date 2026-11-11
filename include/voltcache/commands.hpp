#pragma once
#include "voltcache/cache.hpp"
#include "voltcache/protocol.hpp"

namespace voltcache {
Message execute(Cache& cache, Message request, Cache::Time now = Cache::Clock::now());
}
