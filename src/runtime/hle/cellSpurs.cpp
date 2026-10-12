// HLE: cellSpurs
#include "runtime/hle.h"

namespace rt {
namespace {

}  // namespace

static hle::Module s_module("cellSpurs", {
    HLE_TODO(_cellSpursAttributeInitialize),
    HLE_TODO(_cellSpursEventFlagInitialize),
    HLE_TODO(_cellSpursJobChainAttributeInitialize),
    HLE_TODO(_cellSpursSendSignal),
    HLE_TODO(cellSpursAttachLv2EventQueue),
    HLE_TODO(cellSpursAttributeSetNamePrefix),
    HLE_TODO(cellSpursCreateJobChainWithAttribute),
    HLE_TODO(cellSpursCreateTask),
    HLE_TODO(cellSpursCreateTaskset),
    HLE_TODO(cellSpursDetachLv2EventQueue),
    HLE_TODO(cellSpursEventFlagAttachLv2EventQueue),
    HLE_TODO(cellSpursEventFlagClear),
    HLE_TODO(cellSpursEventFlagDetachLv2EventQueue),
    HLE_TODO(cellSpursEventFlagWait),
    HLE_TODO(cellSpursFinalize),
    HLE_TODO(cellSpursInitialize),
    HLE_TODO(cellSpursInitializeWithAttribute),
    HLE_TODO(cellSpursJoinJobChain),
    HLE_TODO(cellSpursJoinTaskset),
    HLE_TODO(cellSpursRunJobChain),
    HLE_TODO(cellSpursShutdownJobChain),
    HLE_TODO(cellSpursShutdownTaskset),
});

}  // namespace rt
