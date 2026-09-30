// Scripted-run helpers: stop after a frame (WETRIX_EXIT_AT / WETRIX_NO_EXIT) and
// detect gameplay from the triangle count of a frame.

#ifndef WETRIX_RUN_H
#define WETRIX_RUN_H

#include <cstdint>

namespace wetrix {

void exit_if_run_finished(uint64_t frame);

bool observe_frame(uint64_t composed_frame, uint64_t triangles_submitted);

bool in_gameplay();

uint64_t first_gameplay_frame();

uint64_t gameplay_frames();

}  // namespace wetrix

#endif  // WETRIX_RUN_H
