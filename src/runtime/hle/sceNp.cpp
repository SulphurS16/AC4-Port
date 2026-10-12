// HLE: sceNp (PlayStation Network). The port is offline with no PSN account: the library
// initializes and its local objects (callbacks, handlers, contexts) work, the manager status is
// permanently OFFLINE, and every operation that needs the network fails with the error the
// library reports when the user is not signed in.
#include <cstring>

#include "runtime/hle.h"

namespace rt {
namespace {

constexpr s32 SCE_NP_ERROR_NOT_INITIALIZED = s32(0x8002aa01);
constexpr s32 SCE_NP_ERROR_ALREADY_INITIALIZED = s32(0x8002aa02);
constexpr s32 SCE_NP_ERROR_INVALID_ARGUMENT = s32(0x8002aa03);
constexpr s32 SCE_NP_ERROR_ID_NOT_FOUND = s32(0x8002aa06);
constexpr s32 SCE_NP_ERROR_OFFLINE = s32(0x8002aa0c);
constexpr s32 SCE_NP_ERROR_INSUFFICIENT_BUFFER = s32(0x8002aa0f);
constexpr s32 SCE_NP_BASIC_ERROR_NOT_INITIALIZED = s32(0x8002a662);
constexpr s32 SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = s32(0x8002a665);
constexpr s32 SCE_NP_BASIC_ERROR_NO_EVENT = s32(0x8002a66a);
constexpr s32 SCE_NP_BASIC_ERROR_EXCEEDS_MAX = s32(0x8002a66b);
constexpr s32 SCE_NP_BASIC_ERROR_NOT_REGISTERED = s32(0x8002a66d);
constexpr s32 SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = s32(0x8002ab01);
constexpr s32 SCE_NP_UTIL_ERROR_NOT_MATCH = s32(0x8002ab0f);
constexpr s32 SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED = s32(0x8002a101);
constexpr s32 SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED = s32(0x8002a102);
constexpr s32 SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT = s32(0x8002a104);
constexpr s32 SCE_NP_COMMUNITY_ERROR_NO_LOGIN = s32(0x8002a106);
constexpr s32 SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS = s32(0x8002a107);
constexpr s32 SCE_NP_COMMUNITY_ERROR_INVALID_ID = s32(0x8002a1a3);
constexpr s32 SCE_NP_MATCHING_ERROR_NOT_INITIALIZED = s32(0x8002a701);
constexpr s32 SCE_NP_MATCHING_ERROR_INVALID_ARG = s32(0x8002a703);
constexpr s32 SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND = s32(0x8002a709);
constexpr s32 SCE_NP_MATCHING_ERROR_CTX_MAX = s32(0x8002a716);
constexpr s32 SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED = s32(0x8002a801);
constexpr s32 SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND = s32(0x8002a805);
constexpr s32 SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND = s32(0x8002a80e);
constexpr s32 SCE_NP_SIGNALING_ERROR_CTX_MAX = s32(0x8002a812);
constexpr s32 SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT = s32(0x8002a815);
constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;
constexpr u32 kMinPoolSize = 128 * 1024;

std::mutex g_mu;
bool g_init = false, g_score_init = false;
u32 g_manager_cb = 0, g_manager_cb_arg = 0;
u32 g_basic_handler = 0, g_basic_arg = 0;
// Local context ids (contexts hold no network state while offline).
std::map<s32, bool> g_score_title_ctx, g_score_trans_ctx;
std::map<u32, bool> g_matching_ctx, g_signaling_ctx;
s32 g_next_score_id = 1;
u32 g_next_matching_id = 1, g_next_signaling_id = 1;
bool g_logged_offline = false;

s32 offline(const char* fn, s32 err) {
  {
    std::lock_guard lk(g_mu);
    if (!g_logged_offline) {
      g_logged_offline = true;
      LOG_W("np", "%s: PlayStation Network is unavailable in this port (offline); online features fail", fn);
    }
  }
  return err;
}
bool np_init() {
  std::lock_guard lk(g_mu);
  return g_init;
}

// ---- core ----
s32 sceNpInit(u32 poolsize, u32 poolptr) {
  std::lock_guard lk(g_mu);
  if (g_init) return SCE_NP_ERROR_ALREADY_INITIALIZED;
  if (!poolsize || !poolptr) return SCE_NP_ERROR_INVALID_ARGUMENT;
  if (poolsize < kMinPoolSize) return SCE_NP_ERROR_INSUFFICIENT_BUFFER;
  g_init = true;
  LOG_I("np", "sceNpInit: offline (no PSN account; manager status OFFLINE)");
  return CELL_OK;
}
s32 sceNpTerm() {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_ERROR_NOT_INITIALIZED;
  g_init = false;
  g_manager_cb = g_manager_cb_arg = 0;
  g_basic_handler = g_basic_arg = 0;
  return CELL_OK;
}

// ---- manager ----
s32 sceNpManagerGetStatus(Ptr<bes32> status) {
  if (!np_init()) return SCE_NP_ERROR_NOT_INITIALIZED;
  if (!status) return SCE_NP_ERROR_INVALID_ARGUMENT;
  *status = SCE_NP_MANAGER_STATUS_OFFLINE;
  return CELL_OK;
}
s32 sceNpManagerRegisterCallback(u32 cb, u32 arg) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_ERROR_NOT_INITIALIZED;
  if (!cb) return SCE_NP_ERROR_INVALID_ARGUMENT;
  g_manager_cb = cb;  // never invoked: the status never changes while offline
  g_manager_cb_arg = arg;
  return CELL_OK;
}
s32 sceNpManagerUnregisterCallback() {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_ERROR_NOT_INITIALIZED;
  g_manager_cb = g_manager_cb_arg = 0;
  return CELL_OK;
}
s32 manager_query(const char* fn, u32 out, u32 out2 = 1) {
  if (!np_init()) return SCE_NP_ERROR_NOT_INITIALIZED;
  if (!out || !out2) return SCE_NP_ERROR_INVALID_ARGUMENT;
  return offline(fn, SCE_NP_ERROR_OFFLINE);
}
s32 sceNpManagerGetNpId(u32 npid) { return manager_query("sceNpManagerGetNpId", npid); }
s32 sceNpManagerGetOnlineName(u32 name) { return manager_query("sceNpManagerGetOnlineName", name); }
s32 sceNpManagerGetAvatarUrl(u32 url) { return manager_query("sceNpManagerGetAvatarUrl", url); }
s32 sceNpManagerGetNetworkTime(u32 tick) { return manager_query("sceNpManagerGetNetworkTime", tick); }
// Parental-control state of the local user. This port configures no parental-control
// restrictions, so content and chat are reported as unrestricted (the age is then unused and
// reported as 0). Games read these outputs without checking the result.
bool g_logged_restrictions = false;
void log_restrictions(const char* fn) {
  std::lock_guard lk(g_mu);
  if (g_logged_restrictions) return;
  g_logged_restrictions = true;
  LOG_I("np", "%s: no parental-control restrictions are configured in this port; reported as unrestricted", fn);
}
s32 sceNpManagerGetChatRestrictionFlag(Ptr<bes32> restricted) {
  if (!np_init()) return SCE_NP_ERROR_NOT_INITIALIZED;
  if (!restricted) return SCE_NP_ERROR_INVALID_ARGUMENT;
  *restricted = 0;
  log_restrictions("sceNpManagerGetChatRestrictionFlag");
  return CELL_OK;
}
s32 sceNpManagerGetContentRatingFlag(Ptr<bes32> restricted, Ptr<bes32> age) {
  if (!np_init()) return SCE_NP_ERROR_NOT_INITIALIZED;
  if (!restricted || !age) return SCE_NP_ERROR_INVALID_ARGUMENT;
  *restricted = 0;
  *age = 0;
  log_restrictions("sceNpManagerGetContentRatingFlag");
  return CELL_OK;
}

// ---- basic (friends, presence, players history) ----
s32 sceNpBasicRegisterContextSensitiveHandler(u32 comm_id, u32 handler, u32 arg) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (g_basic_handler) return SCE_NP_BASIC_ERROR_EXCEEDS_MAX;
  if (!comm_id || !handler) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  g_basic_handler = handler;  // no events are ever delivered while offline
  g_basic_arg = arg;
  return CELL_OK;
}
s32 sceNpBasicUnregisterHandler() {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (!g_basic_handler) return SCE_NP_BASIC_ERROR_NOT_REGISTERED;
  g_basic_handler = g_basic_arg = 0;
  return CELL_OK;
}
s32 sceNpBasicGetEvent(u32 event, u32 from, u32 data, u32 size) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (!g_basic_handler) return SCE_NP_BASIC_ERROR_NOT_REGISTERED;
  if (!event || !from || !data || !size) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  return SCE_NP_BASIC_ERROR_NO_EVENT;
}
s32 sceNpBasicGetFriendListEntryCount(u32 count) {
  if (!np_init()) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (!count) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  return offline("sceNpBasicGetFriendListEntryCount", SCE_NP_ERROR_ID_NOT_FOUND);
}
s32 sceNpBasicGetFriendListEntry(u32 index, u32 npid) {
  if (!np_init()) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (!npid) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  return offline("sceNpBasicGetFriendListEntry", SCE_NP_ERROR_ID_NOT_FOUND);
}
s32 sceNpBasicGetMatchingInvitationEntryCount(u32 count) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
    if (!g_basic_handler) return SCE_NP_BASIC_ERROR_NOT_REGISTERED;
  }
  if (!count) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  return offline("sceNpBasicGetMatchingInvitationEntryCount", SCE_NP_ERROR_ID_NOT_FOUND);
}
s32 sceNpBasicAddPlayersHistory(u32 npid, u32 description) {
  if (!np_init()) return SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
  if (!npid || !ppu::r8(npid)) return SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
  // Players met online are recorded by the system; there are none while offline.
  return offline("sceNpBasicAddPlayersHistory", SCE_NP_ERROR_OFFLINE);
}

// ---- utilities ----
s32 sceNpUtilCmpNpId(u32 id1, u32 id2) {
  if (!id1 || !id2) return SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;
  // SceNpId: handle.data[16] + term + dummy[3], opt[8], reserved[8].
  std::string a = read_cstr(id1, 16), b = read_cstr(id2, 16);
  if (a != b) return SCE_NP_UTIL_ERROR_NOT_MATCH;
  u8 oa[8], ob[8];
  for (int i = 0; i < 8; ++i) {
    oa[i] = ppu::r8(id1 + 20 + i);
    ob[i] = ppu::r8(id2 + 20 + i);
  }
  if (std::memcmp(oa, ob, 4) != 0) return SCE_NP_UTIL_ERROR_NOT_MATCH;
  if (oa[4] && ob[4] && std::memcmp(oa + 4, ob + 4, 4) != 0) return SCE_NP_UTIL_ERROR_NOT_MATCH;
  return CELL_OK;
}

// ---- score (rankings) ----
s32 sceNpScoreInit() {
  std::lock_guard lk(g_mu);
  if (g_score_init) return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
  if (!g_init) return SCE_NP_ERROR_NOT_INITIALIZED;
  g_score_init = true;
  return CELL_OK;
}
s32 sceNpScoreTerm() {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  g_score_init = false;
  g_score_title_ctx.clear();
  g_score_trans_ctx.clear();
  return CELL_OK;
}
s32 sceNpScoreCreateTitleCtx(u32 comm_id, u32 passphrase, u32 self_npid) {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  if (!comm_id || !passphrase || !self_npid) return SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
  if (g_score_title_ctx.size() >= 32) return SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
  s32 id = g_next_score_id++;
  g_score_title_ctx[id] = true;
  return id;
}
s32 sceNpScoreDestroyTitleCtx(s32 id) {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  return g_score_title_ctx.erase(id) ? CELL_OK : SCE_NP_COMMUNITY_ERROR_INVALID_ID;
}
s32 sceNpScoreCreateTransactionCtx(s32 title_ctx) {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  if (!g_score_title_ctx.count(title_ctx)) return SCE_NP_COMMUNITY_ERROR_INVALID_ID;
  // Creating a transaction needs a signed-in user.
  return offline("sceNpScoreCreateTransactionCtx", SCE_NP_COMMUNITY_ERROR_NO_LOGIN);
}
s32 sceNpScoreDestroyTransactionCtx(s32 id) {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  return g_score_trans_ctx.erase(id) ? CELL_OK : SCE_NP_COMMUNITY_ERROR_INVALID_ID;
}
s32 sceNpScoreAbortTransaction(s32 id) {
  std::lock_guard lk(g_mu);
  if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  return g_score_trans_ctx.count(id) ? CELL_OK : SCE_NP_COMMUNITY_ERROR_INVALID_ID;
}
s32 score_request(const char* fn) {
  {
    std::lock_guard lk(g_mu);
    if (!g_score_init) return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
  }
  return offline(fn, SCE_NP_COMMUNITY_ERROR_NO_LOGIN);
}
s32 sceNpScoreGetGameData(ppu::Ctx&) { return score_request("sceNpScoreGetGameData"); }
s32 sceNpScoreGetRankingByNpId(ppu::Ctx&) { return score_request("sceNpScoreGetRankingByNpId"); }
s32 sceNpScoreGetRankingByRange(ppu::Ctx&) { return score_request("sceNpScoreGetRankingByRange"); }
s32 sceNpScoreRecordGameData(ppu::Ctx&) { return score_request("sceNpScoreRecordGameData"); }
s32 sceNpScoreRecordScore(ppu::Ctx&) { return score_request("sceNpScoreRecordScore"); }

// ---- matching (rooms and lobby GUI) ----
s32 sceNpMatchingCreateCtx(u32 npid, u32 handler, u32 arg, Ptr<be32> ctx_id) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_MATCHING_ERROR_NOT_INITIALIZED;
  if (!npid || !handler || !ctx_id) return SCE_NP_MATCHING_ERROR_INVALID_ARG;
  if (g_matching_ctx.size() >= 1) return SCE_NP_MATCHING_ERROR_CTX_MAX;
  u32 id = g_next_matching_id++;
  g_matching_ctx[id] = true;
  *ctx_id = id;
  return CELL_OK;
}
s32 sceNpMatchingDestroyCtx(u32 id) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_MATCHING_ERROR_NOT_INITIALIZED;
  return g_matching_ctx.erase(id) ? CELL_OK : SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND;
}
s32 matching_request(const char* fn, u32 ctx_id) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return SCE_NP_MATCHING_ERROR_NOT_INITIALIZED;
    if (!g_matching_ctx.count(ctx_id)) return SCE_NP_MATCHING_ERROR_CTX_NOT_FOUND;
  }
  return offline(fn, SCE_NP_ERROR_OFFLINE);
}
#define NP_MATCHING_REQ(name) \
  s32 name(u32 ctx_id) { return matching_request(#name, ctx_id); }
NP_MATCHING_REQ(sceNpMatchingAcceptInvitationGUI)
NP_MATCHING_REQ(sceNpMatchingCreateRoomGUI)
NP_MATCHING_REQ(sceNpMatchingGetResult)
NP_MATCHING_REQ(sceNpMatchingGetResultGUI)
NP_MATCHING_REQ(sceNpMatchingGetRoomListLimitGUI)
NP_MATCHING_REQ(sceNpMatchingGrantOwnership)
NP_MATCHING_REQ(sceNpMatchingJoinRoomGUI)
NP_MATCHING_REQ(sceNpMatchingKickRoomMember)
NP_MATCHING_REQ(sceNpMatchingLeaveRoom)
NP_MATCHING_REQ(sceNpMatchingSearchJoinRoomGUI)
NP_MATCHING_REQ(sceNpMatchingSendInvitationGUI)
NP_MATCHING_REQ(sceNpMatchingSetRoomInfo)
NP_MATCHING_REQ(sceNpMatchingSetRoomSearchFlag)
#undef NP_MATCHING_REQ

// ---- signaling (peer-to-peer connections) ----
s32 sceNpSignalingCreateCtx(u32 npid, u32 handler, u32 arg, Ptr<be32> ctx_id) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
  if (!npid || !ctx_id) return SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
  if (g_signaling_ctx.size() >= 8) return SCE_NP_SIGNALING_ERROR_CTX_MAX;
  u32 id = g_next_signaling_id++;
  g_signaling_ctx[id] = true;
  *ctx_id = id;
  return CELL_OK;
}
s32 sceNpSignalingDestroyCtx(u32 id) {
  std::lock_guard lk(g_mu);
  if (!g_init) return SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
  return g_signaling_ctx.erase(id) ? CELL_OK : SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
}
s32 signaling_conn(const char* fn, u32 ctx_id) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!g_signaling_ctx.count(ctx_id)) return SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
  }
  return offline(fn, SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}
s32 sceNpSignalingActivateConnection(u32 ctx_id) { return signaling_conn("sceNpSignalingActivateConnection", ctx_id); }
s32 sceNpSignalingGetConnectionStatus(u32 ctx_id) { return signaling_conn("sceNpSignalingGetConnectionStatus", ctx_id); }
s32 sceNpSignalingTerminateConnection(u32 ctx_id) { return signaling_conn("sceNpSignalingTerminateConnection", ctx_id); }

}  // namespace

static hle::Module s_module("sceNp", {
    HLE_FN(sceNpBasicAddPlayersHistory),
    HLE_FN(sceNpBasicGetEvent),
    HLE_FN(sceNpBasicGetFriendListEntry),
    HLE_FN(sceNpBasicGetFriendListEntryCount),
    HLE_FN(sceNpBasicGetMatchingInvitationEntryCount),
    HLE_FN(sceNpBasicRegisterContextSensitiveHandler),
    HLE_FN(sceNpBasicUnregisterHandler),
    HLE_FN(sceNpInit),
    HLE_FN(sceNpManagerGetAvatarUrl),
    HLE_FN(sceNpManagerGetChatRestrictionFlag),
    HLE_FN(sceNpManagerGetContentRatingFlag),
    HLE_FN(sceNpManagerGetNetworkTime),
    HLE_FN(sceNpManagerGetNpId),
    HLE_FN(sceNpManagerGetOnlineName),
    HLE_FN(sceNpManagerGetStatus),
    HLE_FN(sceNpManagerRegisterCallback),
    HLE_FN(sceNpManagerUnregisterCallback),
    HLE_FN(sceNpMatchingAcceptInvitationGUI),
    HLE_FN(sceNpMatchingCreateCtx),
    HLE_FN(sceNpMatchingCreateRoomGUI),
    HLE_FN(sceNpMatchingDestroyCtx),
    HLE_FN(sceNpMatchingGetResult),
    HLE_FN(sceNpMatchingGetResultGUI),
    HLE_FN(sceNpMatchingGetRoomListLimitGUI),
    HLE_FN(sceNpMatchingGrantOwnership),
    HLE_FN(sceNpMatchingJoinRoomGUI),
    HLE_FN(sceNpMatchingKickRoomMember),
    HLE_FN(sceNpMatchingLeaveRoom),
    HLE_FN(sceNpMatchingSearchJoinRoomGUI),
    HLE_FN(sceNpMatchingSendInvitationGUI),
    HLE_FN(sceNpMatchingSetRoomInfo),
    HLE_FN(sceNpMatchingSetRoomSearchFlag),
    HLE_FN(sceNpScoreAbortTransaction),
    HLE_FN(sceNpScoreCreateTitleCtx),
    HLE_FN(sceNpScoreCreateTransactionCtx),
    HLE_FN(sceNpScoreDestroyTitleCtx),
    HLE_FN(sceNpScoreDestroyTransactionCtx),
    HLE_FN(sceNpScoreGetGameData),
    HLE_FN(sceNpScoreGetRankingByNpId),
    HLE_FN(sceNpScoreGetRankingByRange),
    HLE_FN(sceNpScoreInit),
    HLE_FN(sceNpScoreRecordGameData),
    HLE_FN(sceNpScoreRecordScore),
    HLE_FN(sceNpScoreTerm),
    HLE_FN(sceNpSignalingActivateConnection),
    HLE_FN(sceNpSignalingCreateCtx),
    HLE_FN(sceNpSignalingDestroyCtx),
    HLE_FN(sceNpSignalingGetConnectionStatus),
    HLE_FN(sceNpSignalingTerminateConnection),
    HLE_FN(sceNpTerm),
    HLE_FN(sceNpUtilCmpNpId),
});

}  // namespace rt
