// HLE: cellHttp. Clients and transactions are ordinary local objects, but the port has no network
// interface, so sending a request fails the way it does on a console without a connection (the
// host name cannot be resolved) and no response is ever available.
#include "runtime/hle.h"

namespace rt {
namespace {

constexpr s32 CELL_HTTP_ERROR_ALREADY_INITIALIZED = s32(0x80710001);
constexpr s32 CELL_HTTP_ERROR_NOT_INITIALIZED = s32(0x80710002);
constexpr s32 CELL_HTTP_ERROR_NO_MEMORY = s32(0x80710003);
constexpr s32 CELL_HTTP_ERROR_NO_BUFFER = s32(0x80710004);
constexpr s32 CELL_HTTP_ERROR_INVALID_URI = s32(0x80710007);
constexpr s32 CELL_HTTP_ERROR_BAD_METHOD = s32(0x80710009);
constexpr s32 CELL_HTTP_ERROR_BAD_CLIENT = s32(0x80710010);
constexpr s32 CELL_HTTP_ERROR_BAD_TRANS = s32(0x80710011);
constexpr s32 CELL_HTTP_ERROR_NO_REQUEST_SENT = s32(0x80710013);
constexpr s32 CELL_HTTP_ERROR_NET_RESOLVER = s32(0x80710100);

std::mutex g_mu;
bool g_init = false;
std::map<u32, bool> g_clients;
std::map<u32, u32> g_trans;  // transaction -> client
u32 g_next_client = 1, g_next_trans = 1;

s32 cellHttpInit(u32 pool, u32 size) {
  std::lock_guard lk(g_mu);
  if (g_init) return CELL_HTTP_ERROR_ALREADY_INITIALIZED;
  if (!pool || !size) return CELL_HTTP_ERROR_NO_MEMORY;
  g_init = true;
  return CELL_OK;
}
s32 cellHttpEnd() {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  g_init = false;
  g_clients.clear();
  g_trans.clear();
  return CELL_OK;
}
s32 cellHttpCreateClient(Ptr<be32> client) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  if (!client) return CELL_HTTP_ERROR_NO_BUFFER;
  u32 id = g_next_client++;
  g_clients[id] = true;
  *client = id;
  return CELL_OK;
}
s32 cellHttpDestroyClient(u32 client) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  if (!g_clients.erase(client)) return CELL_HTTP_ERROR_BAD_CLIENT;
  for (auto it = g_trans.begin(); it != g_trans.end();) it = it->second == client ? g_trans.erase(it) : std::next(it);
  return CELL_OK;
}
s32 client_setting(u32 client) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  return g_clients.count(client) ? CELL_OK : CELL_HTTP_ERROR_BAD_CLIENT;
}
s32 cellHttpClientSetAuthenticationCallback(u32 client, u32 cb, u32 arg) { return client_setting(client); }
s32 cellHttpClientSetAutoAuthentication(u32 client, u32 enable) { return client_setting(client); }
s32 cellHttpClientSetAutoRedirect(u32 client, u32 enable) { return client_setting(client); }
s32 cellHttpClientSetPipeline(u32 client, u32 enable) { return client_setting(client); }
s32 cellHttpClientSetTransactionStateCallback(u32 client, u32 cb, u32 arg) { return client_setting(client); }
s32 cellHttpCreateTransaction(Ptr<be32> trans, u32 client, u32 method, u32 uri) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  if (!trans) return CELL_HTTP_ERROR_NO_BUFFER;
  if (!g_clients.count(client)) return CELL_HTTP_ERROR_BAD_CLIENT;
  if (!method) return CELL_HTTP_ERROR_BAD_METHOD;
  if (!uri) return CELL_HTTP_ERROR_INVALID_URI;
  u32 id = g_next_trans++;
  g_trans[id] = client;
  *trans = id;
  return CELL_OK;
}
s32 cellHttpDestroyTransaction(u32 trans) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  return g_trans.erase(trans) ? CELL_OK : CELL_HTTP_ERROR_BAD_TRANS;
}
s32 trans_check(u32 trans) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_HTTP_ERROR_NOT_INITIALIZED;
  return g_trans.count(trans) ? CELL_OK : CELL_HTTP_ERROR_BAD_TRANS;
}
s32 cellHttpSendRequest(u32 trans, u32 buf, u32 size, u32 sent) {
  if (s32 e = trans_check(trans)) return e;
  LOG_W("http", "cellHttpSendRequest: no network in this port (offline); host name resolution fails");
  return CELL_HTTP_ERROR_NET_RESOLVER;
}
s32 cellHttpRecvResponse(u32 trans, u32 buf, u32 size, u32 recvd) {
  if (s32 e = trans_check(trans)) return e;
  return CELL_HTTP_ERROR_NO_REQUEST_SENT;
}
s32 cellHttpResponseGetStatusCode(u32 trans, u32 code) {
  if (s32 e = trans_check(trans)) return e;
  return CELL_HTTP_ERROR_NO_REQUEST_SENT;
}
s32 cellHttpResponseGetContentLength(u32 trans, u32 len) {
  if (s32 e = trans_check(trans)) return e;
  return CELL_HTTP_ERROR_NO_REQUEST_SENT;
}

}  // namespace

static hle::Module s_module("cellHttp", {
    HLE_FN(cellHttpClientSetAuthenticationCallback),
    HLE_FN(cellHttpClientSetAutoAuthentication),
    HLE_FN(cellHttpClientSetAutoRedirect),
    HLE_FN(cellHttpClientSetPipeline),
    HLE_FN(cellHttpClientSetTransactionStateCallback),
    HLE_FN(cellHttpCreateClient),
    HLE_FN(cellHttpCreateTransaction),
    HLE_FN(cellHttpDestroyClient),
    HLE_FN(cellHttpDestroyTransaction),
    HLE_FN(cellHttpEnd),
    HLE_FN(cellHttpInit),
    HLE_FN(cellHttpRecvResponse),
    HLE_FN(cellHttpResponseGetContentLength),
    HLE_FN(cellHttpResponseGetStatusCode),
    HLE_FN(cellHttpSendRequest),
});

}  // namespace rt
