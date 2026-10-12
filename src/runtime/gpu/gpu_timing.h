// GPU time per pass (--gpu-timing, diagnostics): D3D11 timestamp queries at pass boundaries
// (render target changes, feedback copies, depth-as-color passes, clears, presentation), issued in
// stream order on the submission thread and read back a few frames later without waiting. Every
// 5 s the passes that took the most GPU time per frame are logged (channel "gputime"). The time
// between two marks is charged to the first; a shared GPU (other programs) inflates all of it.
// --gpu-timing-draws also marks every draw with its fragment program (heavier; for finding the
// programs that cost the most).
#pragma once
#include <string>

namespace rt::gpu::timing {

bool enabled();
bool per_draw();
// Starts a pass: the GPU time from here to the next mark is charged to `label`.
void mark(std::string label);
// End of a frame (after its presentation was recorded).
void frame_end();

}  // namespace rt::gpu::timing
