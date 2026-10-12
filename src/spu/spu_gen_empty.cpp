// Used when no recompiled SPU code has been generated (local/gen/spu is empty): every chunk runs
// in the interpreter.
#include "spu/spu_gen.h"

namespace spu_gen {
const Chunk g_chunks[1] = {};
const u32 g_chunk_count = 0;
}  // namespace spu_gen
