// HLE: cellPamf
#include "runtime/hle.h"

namespace rt {
namespace {

}  // namespace

static hle::Module s_module("cellPamf", {
    HLE_TODO(cellPamfReaderGetNumberOfSpecificStreams),
    HLE_TODO(cellPamfReaderGetStreamInfo),
    HLE_TODO(cellPamfReaderGetStreamTypeAndChannel),
    HLE_TODO(cellPamfReaderSetStreamWithTypeAndIndex),
});

}  // namespace rt
