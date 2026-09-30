// A sampling profiler for one thread; see sampler.cpp.

#pragma once

#include <cstdint>
#include <vector>

namespace wetrix::sampler {

struct Session;

// Starts sampling the calling thread.
Session* start_on_this_thread();

// Starts sampling another thread, given its native (Windows) handle.
Session* start_on_handle(void* handle);

// Stops and returns the sampled addresses, relative to the executable's base.
std::vector<uint64_t> stop(Session* session);

}  // namespace wetrix::sampler
