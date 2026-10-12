// HLE: libvdec (cellVdec), the video decoder library used by cellSail. MPEG-2 pictures are
// decoded by the runtime's own decoder (runtime/media/mpeg2_video); the stream headers are also
// parsed here for the per-picture information (CellVdecMpeg2Info). Commands run in order on a
// decoder thread created with the client's priority and stack size, which calls the client back
// like the firmware decoder: AUDONE when an access unit has been consumed, PICOUT per picture in
// display order (after the standard MPEG-2 reordering delay only), SEQDONE once EndSeq has
// drained the decoder. Other codecs (AVC, MPEG-4, VC-1, DivX) are not provided and are refused at
// QueryAttr/Open. Closed-caption user data is not extracted (ccDataLength stays 0).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/hle.h"
#include "runtime/media/mpeg2_video.h"
#include "runtime/ppu_thread.h"

namespace rt {
namespace lv2 {
extern u32 g_sdk_version;
}
namespace {

constexpr s32 CELL_VDEC_ERROR_ARG = s32(0x80610101);
constexpr s32 CELL_VDEC_ERROR_SEQ = s32(0x80610102);
constexpr s32 CELL_VDEC_ERROR_BUSY = s32(0x80610103);
constexpr s32 CELL_VDEC_ERROR_EMPTY = s32(0x80610104);
constexpr s32 CELL_VDEC_ERROR_FATAL = s32(0x80610180);

constexpr s32 kCodecMpeg2 = 0;
constexpr s32 kMsgAuDone = 0, kMsgPicOut = 1, kMsgSeqDone = 2, kMsgError = 3;
constexpr s32 kModeNormal = 0, kModeBSkip = 1;
constexpr s32 kAttrNormal = 0, kAttrSkipped = 1;
constexpr u32 kFmtArgb = 0, kFmtRgba = 1, kFmtUyvy = 2, kFmtYuv420 = 3;
constexpr u32 kCmdDepth = 4;     // access units queued at once (AUDONE frees a slot)
constexpr size_t kOutMax = 60;   // decoded pictures held before the decoder waits for GetPicture
constexpr u64 kTsInvalid = ~0ull;

struct CellCodecTimeStamp {
  be32 upper, lower;
};
struct CellVdecType {
  bes32 codecType;
  be32 profileLevel;
};
struct CellVdecTypeEx {
  bes32 codecType;
  be32 profileLevel;
  be32 codecSpecificInfo;
};
struct CellVdecAttr {
  be32 memSize;
  u8 cmdDepth;
  be32 decoderVerUpper, decoderVerLower;
};
struct CellVdecResource {
  be32 memAddr, memSize;
  bes32 ppuThreadPriority;
  be32 ppuThreadStackSize;
  bes32 spuThreadPriority;
  be32 numOfSpus;
};
struct CellVdecResourceEx {
  be32 memAddr, memSize;
  bes32 ppuThreadPriority;
  be32 ppuThreadStackSize;
  bes32 spuThreadPriority;
  be32 numOfSpus, spursResource;
};
struct CellVdecCb {
  be32 cbFunc, cbArg;
};
struct CellVdecAuInfo {
  be32 startAddr, size;
  CellCodecTimeStamp pts, dts;
  be64 userData, codecSpecificData;
};
struct CellVdecPicItem {
  bes32 codecType;
  be32 startAddr, size;
  u8 auNum;
  CellCodecTimeStamp auPts[2], auDts[2];
  be64 auUserData[2];
  bes32 status, attr;
  be32 picInfo;
};
struct CellVdecPicFormat {
  be32 formatType, colorMatrixType;
  u8 alpha;
};
struct CellVdecMpeg2SpecificInfo {
  be32 thisSize;
  be16 maxDecodedFrameWidth, maxDecodedFrameHeight;
};
struct CellVdecMpeg2Info {
  be16 horizontal_size, vertical_size;
  u8 aspect_ratio_information, frame_rate_code, progressive_sequence, low_delay, video_format, colour_description,
      colour_primaries, transfer_characteristics, matrix_coefficients;
  be16 temporal_reference[2];
  u8 picture_coding_type[2], picture_structure[2], top_field_first, repeat_first_field, progressive_frame;
  be32 time_code;
  u8 closed_gop, broken_link;
  be16 vbv_delay[2];
  be16 display_horizontal_size, display_vertical_size;
  u8 number_of_frame_centre_offsets[2];
  be16 frame_centre_horizontal_offset[2][3], frame_centre_vertical_offset[2][3];
  be32 headerPresentFlags, headerRetentionFlags;
  u8 mpeg1Flag;
  u8 ccDataLength[2];
  u8 ccData[2][128];
  be64 reserved[2];
};
static_assert(sizeof(CellVdecAttr) == 16 && sizeof(CellVdecAuInfo) == 40 && sizeof(CellVdecPicItem) == 80);
static_assert(offsetof(CellVdecMpeg2Info, temporal_reference) == 14 && offsetof(CellVdecMpeg2Info, time_code) == 28 &&
              offsetof(CellVdecMpeg2Info, headerPresentFlags) == 68 && offsetof(CellVdecMpeg2Info, ccData) == 79 &&
              sizeof(CellVdecMpeg2Info) == 352);

constexpr u32 kItemSlot = 512;  // CellVdecPicItem followed by its CellVdecMpeg2Info, in the decoder memory
static_assert(sizeof(CellVdecPicItem) + sizeof(CellVdecMpeg2Info) <= kItemSlot);

// MPEG-2 header flags (CellVdecMpeg2Info::headerPresentFlags); second-field flags are << 4.
enum : u32 {
  kSeqHdr = 0x1, kSeqExt = 0x2, kSeqDspExt = 0x4, kSeqUsrDat = 0x8, kSeqEnd = 0x10, kGopHdr = 0x20, kGopUsrDat = 0x40,
  kPicHdr1 = 0x100, kPicExt1 = 0x200, kPicDspExt1 = 0x400, kPicUsrDat1 = 0x800,
};

// ---- MPEG-2 video headers (ISO/IEC 13818-2 §6.2) ----
class Bits {
 public:
  Bits(const u8* d, size_t n) : p_(d), n_(n) {}
  u32 get(int k) {
    u32 v = 0;
    while (k--) {
      v = v << 1 | (pos_ < n_ * 8 ? (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1 : 0);
      ++pos_;
    }
    return v;
  }

 private:
  const u8* p_;
  size_t n_, pos_ = 0;
};

struct SeqState {
  bool have_seq = false, mpeg2 = false, have_dsp = false;
  u32 hsize = 0, vsize = 0;
  u8 aspect = 0, frame_rate_code = 0, progressive_sequence = 1, low_delay = 0, frext_n = 0, frext_d = 0;
  u8 video_format = 5, colour_description = 0, colour_primaries = 1, transfer = 1, matrix = 1;
  u16 disp_h = 0, disp_v = 0;
  u32 time_code = 0;
  u8 closed_gop = 0, broken_link = 0;
};

struct AuHeaders {
  u32 present = 0;
  int pics = 0;
  u16 tref[2] = {}, vbv[2] = {};
  u8 pct[2] = {}, pstr[2] = {3, 3};
  u8 tff = 0, rff = 0, progressive_frame = 1;
  u8 nfco[2] = {};
  u16 fch[2][3] = {}, fcv[2][3] = {};
};

// Parses every header of one access unit, updating the sequence state.
AuHeaders parse_au(SeqState& s, const u8* d, size_t n) {
  AuHeaders h;
  int ctx = 0;  // where user data belongs: 0 sequence, 1 GOP, 2 picture
  for (size_t i = 0; i + 3 < n;) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) {
      ++i;
      continue;
    }
    u8 code = d[i + 3];
    size_t b = i + 4, e = b;
    while (e + 2 < n && (d[e] || d[e + 1] || d[e + 2] != 1)) ++e;
    if (e + 2 >= n) e = n;
    Bits r(d + b, e - b);
    int f = h.pics > 0 ? h.pics - 1 : 0;
    if (code == 0xB3) {
      s.have_seq = true;
      s.mpeg2 = false;
      s.hsize = r.get(12);
      s.vsize = r.get(12);
      s.aspect = u8(r.get(4));
      s.frame_rate_code = u8(r.get(4));
      s.progressive_sequence = 1;
      s.low_delay = 0;
      s.frext_n = s.frext_d = 0;
      h.present |= kSeqHdr;
      ctx = 0;
    } else if (code == 0xB5) {
      u32 id = r.get(4);
      if (id == 1) {  // sequence extension
        s.mpeg2 = true;
        r.get(8);     // profile_and_level_indication
        s.progressive_sequence = u8(r.get(1));
        r.get(2);     // chroma_format
        s.hsize = (s.hsize & 0xfff) | r.get(2) << 12;
        s.vsize = (s.vsize & 0xfff) | r.get(2) << 12;
        r.get(12 + 1 + 8);  // bit_rate_extension, marker, vbv_buffer_size_extension
        s.low_delay = u8(r.get(1));
        s.frext_n = u8(r.get(2));
        s.frext_d = u8(r.get(5));
        h.present |= kSeqExt;
      } else if (id == 2) {  // sequence display extension
        s.have_dsp = true;
        s.video_format = u8(r.get(3));
        s.colour_description = u8(r.get(1));
        if (s.colour_description) {
          s.colour_primaries = u8(r.get(8));
          s.transfer = u8(r.get(8));
          s.matrix = u8(r.get(8));
        }
        s.disp_h = u16(r.get(14));
        r.get(1);
        s.disp_v = u16(r.get(14));
        h.present |= kSeqDspExt;
      } else if (id == 8 && h.pics) {  // picture coding extension
        r.get(16 + 2);                 // f_codes, intra_dc_precision
        h.pstr[f] = u8(r.get(2));
        h.tff = u8(r.get(1));
        r.get(5);  // frame_pred_frame_dct, concealment_motion_vectors, q_scale_type, intra_vlc_format, alternate_scan
        h.rff = u8(r.get(1));
        r.get(1);  // chroma_420_type
        h.progressive_frame = u8(r.get(1));
        h.present |= kPicExt1 << (4 * f);
      } else if (id == 7 && h.pics) {  // picture display extension
        int count;
        if (s.progressive_sequence) count = h.rff ? (h.tff ? 3 : 2) : 1;
        else if (h.pstr[f] != 3) count = 1;
        else count = h.rff ? 3 : 2;
        h.nfco[f] = u8(count);
        for (int k = 0; k < count; ++k) {
          h.fch[f][k] = u16(r.get(16));
          r.get(1);
          h.fcv[f][k] = u16(r.get(16));
          r.get(1);
        }
        h.present |= kPicDspExt1 << (4 * f);
      }
    } else if (code == 0xB8) {
      s.time_code = r.get(25);
      s.closed_gop = u8(r.get(1));
      s.broken_link = u8(r.get(1));
      h.present |= kGopHdr;
      ctx = 1;
    } else if (code == 0x00) {
      if (h.pics < 2) {
        int k = h.pics++;
        h.tref[k] = u16(r.get(10));
        h.pct[k] = u8(r.get(3));
        h.vbv[k] = u16(r.get(16));
        h.present |= kPicHdr1 << (4 * k);
      }
      ctx = 2;
    } else if (code == 0xB2) {
      h.present |= ctx == 2 ? kPicUsrDat1 << (4 * f) : ctx == 1 ? kGopUsrDat : kSeqUsrDat;
    } else if (code == 0xB7) {
      h.present |= kSeqEnd;
    }
    i = e;
  }
  return h;
}

void fill_info(CellVdecMpeg2Info& m, const SeqState& s, const AuHeaders& h, u32 retained) {
  std::memset(&m, 0, sizeof(m));
  m.horizontal_size = u16(s.hsize);
  m.vertical_size = u16(s.vsize);
  m.aspect_ratio_information = s.aspect;
  m.frame_rate_code = s.frame_rate_code;
  m.progressive_sequence = s.progressive_sequence;
  m.low_delay = s.low_delay;
  m.video_format = s.have_dsp ? s.video_format : 5;  // unspecified
  m.colour_description = s.colour_description;
  m.colour_primaries = s.colour_primaries;
  m.transfer_characteristics = s.transfer;
  m.matrix_coefficients = s.matrix;
  for (int k = 0; k < 2; ++k) {
    m.temporal_reference[k] = h.tref[k];
    m.picture_coding_type[k] = h.pct[k];
    m.picture_structure[k] = k < h.pics ? (s.mpeg2 ? h.pstr[k] : 3) : 0;
    m.vbv_delay[k] = h.vbv[k];
    m.number_of_frame_centre_offsets[k] = h.nfco[k];
    for (int j = 0; j < 3; ++j) {
      m.frame_centre_horizontal_offset[k][j] = h.fch[k][j];
      m.frame_centre_vertical_offset[k][j] = h.fcv[k][j];
    }
  }
  m.top_field_first = h.tff;
  m.repeat_first_field = h.rff;
  m.progressive_frame = s.mpeg2 ? h.progressive_frame : 1;
  m.time_code = s.time_code;
  m.closed_gop = s.closed_gop;
  m.broken_link = s.broken_link;
  m.display_horizontal_size = u16(s.have_dsp ? s.disp_h : s.hsize);
  m.display_vertical_size = u16(s.have_dsp ? s.disp_v : s.vsize);
  m.headerPresentFlags = h.present;
  m.headerRetentionFlags = retained;
  m.mpeg1Flag = s.mpeg2 ? 0 : 1;
}

// Picture period in 90 kHz units for a CellVdecFrameRate code (0x80..0x87), or from the sequence
// header's frame_rate_code and extension with repeat_first_field applied.
u64 period_frc(u32 frc) {
  static const u32 t[8] = {3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500};
  return t[frc & 7];
}
u64 period_stream(const SeqState& s, const AuHeaders& h) {
  static const u32 num[9] = {0, 24000, 24, 25, 30000, 30, 50, 60000, 60}, den[9] = {1, 1001, 1, 1, 1001, 1, 1, 1001, 1};
  u32 c = s.frame_rate_code;
  u64 p = c >= 1 && c <= 8 ? u64(90000) * den[c] * (s.frext_d + 1) / (u64(num[c]) * (s.frext_n + 1)) : 3003;
  if (h.rff) p = s.progressive_sequence ? p * (h.tff ? 3 : 2) : p * 3 / 2;
  return p;
}

// ---- decoder ----
enum class Seq { Closed, Dormant, Ready, Ending };
enum class CmdType { Start, Decode, Drain, End, FrameRate, Close };

struct Cmd {
  CmdType type;
  u64 seq = 0;
  s32 mode = 0;
  u64 pts = kTsInvalid, dts = kTsInvalid, user = 0;
  u32 frc = 0;
  std::vector<u8> data;
};

struct Picture {
  u64 seq = 0;
  u32 width = 0, height = 0;
  std::vector<u8> yuv;  // I420 (Y, U, V planes); empty for a skipped picture
  u64 pts = kTsInvalid, dts = kTsInvalid, user = 0;
  s32 attr = kAttrNormal;
  CellVdecMpeg2Info info{};
  bool item_taken = false;
};

// An access unit given to the decoder, waiting for its picture (keyed by its tag).
struct AuRecord {
  u64 pts, dts, user, period;
  s32 attr;
  CellVdecMpeg2Info info;
};

struct Vdec {
  u32 handle = 0;
  u32 mem_addr = 0, mem_size = 0;
  u32 cb_func = 0, cb_arg = 0;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<Cmd> cmds;
  std::deque<Picture> out;
  Seq state = Seq::Dormant;
  u64 seq_id = 0;
  u32 au_count = 0;
  u32 item_next = 0;
  u32 thread_id = 0;
  bool closing = false, finished = false;

  // Decoder thread only.
  media::Mpeg2Decoder dec;
  SeqState seq;
  u32 seen = 0;  // sequence/GOP headers whose values are held (headerRetentionFlags)
  std::map<u64, AuRecord> pending;
  u64 next_tag = 0;
  u64 next_pts = kTsInvalid, next_dts = kTsInvalid;
  u32 frc_override = 0;
  bool warned_no_seq = false, warned_userdata = false;
  u32 errors = 0;
};

std::mutex g_mu;
std::map<u32, std::shared_ptr<Vdec>> g_vdecs;
u32 g_next_handle = 0xf0000000;

std::shared_ptr<Vdec> get(u32 handle) {
  std::lock_guard lk(g_mu);
  auto it = g_vdecs.find(handle);
  return it == g_vdecs.end() ? nullptr : it->second;
}

// Time stamps for an output picture: pictures without their own continue from the previous one.
void stamp(Vdec& v, const AuRecord& r, Picture& p) {
  p.pts = r.pts != kTsInvalid ? r.pts : v.next_pts;
  p.dts = r.dts != kTsInvalid ? r.dts : v.next_dts;
  u64 period = v.frc_override ? period_frc(v.frc_override) : r.period;
  v.next_pts = p.pts == kTsInvalid ? kTsInvalid : p.pts + period;
  v.next_dts = p.dts == kTsInvalid ? kTsInvalid : p.dts + period;
  p.user = r.user;
  p.attr = r.attr;
  p.info = r.info;
}

// Pictures leaving the decoder (display order) become queue entries with their access unit's data.
void take(Vdec& v, std::vector<media::Mpeg2Picture>& decoded, std::vector<Picture>& pics) {
  for (media::Mpeg2Picture& d : decoded) {
    Picture p;
    auto it = v.pending.find(d.tag);
    if (it != v.pending.end()) {
      stamp(v, it->second, p);
      v.pending.erase(it);
    }
    p.width = d.width;
    p.height = d.height;
    p.yuv = std::move(d.yuv);
    pics.push_back(std::move(p));
  }
  decoded.clear();
  // Access units that produced no picture (damaged data) would otherwise accumulate.
  while (v.pending.size() > 8) v.pending.erase(v.pending.begin());
}

void notify(ppu::Context& ctx, Vdec& v, s32 msg, s32 data) {
  hle::call_guest(ctx, v.cb_func, {v.handle, u64(s64(msg)), u64(s64(data)), v.cb_arg});
}

// Decodes one access unit; pictures it completes are appended to `pics`.
void decode_au(Vdec& v, const Cmd& c, std::vector<Picture>& pics) {
  AuHeaders h = parse_au(v.seq, c.data.data(), c.data.size());
  if ((h.present & (kPicUsrDat1 | kPicUsrDat1 << 4)) && !v.warned_userdata) {
    LOG_I("vdec", "handle 0x%x: stream carries picture user data; closed captions are not extracted", v.handle);
    v.warned_userdata = true;
  }
  v.seen |= h.present & (kSeqHdr | kSeqExt | kSeqDspExt | kGopHdr);
  if (!v.seq.have_seq) {
    if (!v.warned_no_seq) LOG_W("vdec", "handle 0x%x: access unit before any sequence header skipped", v.handle);
    v.warned_no_seq = true;
    return;
  }
  // Only the B pictures skipped by B_SKIP mode are marked skipped; I and P pictures decode normally.
  bool skip_b = c.mode == kModeBSkip && h.pics && h.pct[0] == 3;
  AuRecord r{c.pts, c.dts, c.user, period_stream(v.seq, h), skip_b ? kAttrSkipped : kAttrNormal, {}};
  fill_info(r.info, v.seq, h, v.seen);
  LOG_D("vdec", "handle 0x%x: AU %zu bytes type %u mode %d pts 0x%llx", v.handle, c.data.size(), h.pics ? h.pct[0] : 0u, c.mode,
        (unsigned long long)c.pts);
  std::vector<media::Mpeg2Picture> decoded;
  if (skip_b) {
    // A skipped B picture is not decoded; it follows the pictures already output in display order.
    Picture p;
    stamp(v, r, p);
    p.width = v.seq.hsize;
    p.height = v.seq.vsize;
    pics.push_back(std::move(p));
  } else {
    u64 tag = ++v.next_tag;
    v.pending[tag] = r;
  }
  if (!v.dec.decode(c.data.data(), c.data.size(), v.next_tag, skip_b, &decoded) && v.errors++ < 8)
    LOG_W("vdec", "handle 0x%x: damaged access unit (%s)", v.handle, v.dec.error().c_str());
  take(v, decoded, pics);
}

// Queues decoded pictures for GetPicture, waiting while the queue is full; PICOUT per picture.
void deliver(ppu::Context& ctx, Vdec& v, std::vector<Picture>& pics, u64 seq) {
  for (Picture& p : pics) {
    {
      std::unique_lock lk(v.mu);
      auto ready = [&] { return v.out.size() < kOutMax || v.seq_id != seq || v.closing; };
      if (!ready()) note_block();
      v.cv.wait(lk, ready);
      if (v.seq_id != seq || v.closing) return;
      p.seq = seq;
      LOG_D("vdec", "handle 0x%x: PICOUT pts 0x%llx attr %d%s (%zu queued)", v.handle, (unsigned long long)p.pts, p.attr,
            p.yuv.empty() ? " (no image)" : "", v.out.size() + 1);
      v.out.push_back(std::move(p));
    }
    notify(ctx, v, kMsgPicOut, CELL_OK);
  }
}

void decoder_main(const std::shared_ptr<Vdec>& vp, ppu::Context& ctx) {
  Vdec& v = *vp;
  for (;;) {
    Cmd c;
    {
      std::unique_lock lk(v.mu);
      if (v.cmds.empty()) note_block();
      v.cv.wait(lk, [&] { return !v.cmds.empty(); });
      c = std::move(v.cmds.front());
      v.cmds.pop_front();
    }
    if (c.type == CmdType::Close) break;
    bool current;
    {
      std::lock_guard lk(v.mu);
      current = c.seq == v.seq_id && !v.closing;
    }
    std::vector<Picture> pics;
    switch (c.type) {
      case CmdType::Start:  // a new sequence: no picture refers to the previous one
        v.dec.reset();
        v.pending.clear();
        v.next_pts = v.next_dts = kTsInvalid;
        v.frc_override = 0;
        v.seen = 0;
        {
          std::lock_guard lk(v.mu);
          v.out.clear();
        }
        v.cv.notify_all();
        break;
      case CmdType::Decode:
        if (current) decode_au(v, c, pics);
        {
          std::lock_guard lk(v.mu);
          if (v.au_count) --v.au_count;
        }
        notify(ctx, v, kMsgAuDone, CELL_OK);  // also for access units of an abandoned sequence
        deliver(ctx, v, pics, c.seq);
        break;
      case CmdType::Drain:
        if (current) {
          std::vector<media::Mpeg2Picture> decoded;
          v.dec.drain(&decoded);
          take(v, decoded, pics);
        }
        v.pending.clear();
        deliver(ctx, v, pics, c.seq);
        break;
      case CmdType::End:
        {
          std::lock_guard lk(v.mu);
          if (v.state == Seq::Ending) v.state = Seq::Dormant;
        }
        notify(ctx, v, kMsgSeqDone, CELL_OK);
        break;
      case CmdType::FrameRate:
        v.frc_override = c.frc;
        break;
      case CmdType::Close:
        break;
    }
  }
  {
    std::lock_guard lk(v.mu);
    v.finished = true;
    v.out.clear();
  }
  v.cv.notify_all();
}

// ---- conversion for GetPicture ----
inline u8 clamp8(int x) { return u8(x < 0 ? 0 : x > 255 ? 255 : x); }

void convert(const Picture& p, u32 fmt, u32 matrix, u8 alpha, u8* dst) {
  u32 w = p.width, h = p.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const u8* Y = p.yuv.data();
  const u8* U = Y + size_t(w) * h;
  const u8* V = U + size_t(cw) * ch;
  if (fmt == kFmtYuv420) {
    std::memcpy(dst, p.yuv.data(), p.yuv.size());
    return;
  }
  if (fmt == kFmtUyvy) {
    for (u32 y = 0; y < h; ++y) {
      const u8* u = U + (y / 2) * cw;
      const u8* v = V + (y / 2) * cw;
      u8* o = dst + size_t(y) * w * 2;
      for (u32 x = 0; x < w; x += 2) {
        o[2 * x + 0] = u[x / 2];
        o[2 * x + 1] = Y[size_t(y) * w + x];
        o[2 * x + 2] = v[x / 2];
        o[2 * x + 3] = x + 1 < w ? Y[size_t(y) * w + x + 1] : Y[size_t(y) * w + x];
      }
    }
    return;
  }
  // Studio-range Y'CbCr to full-range RGB, 8.8 fixed point (BT.601 or BT.709 coefficients).
  const int rv = matrix ? 459 : 409, gu = matrix ? 55 : 100, gv = matrix ? 136 : 208, bu = matrix ? 541 : 516;
  for (u32 y = 0; y < h; ++y) {
    const u8* yr = Y + size_t(y) * w;
    const u8* ur = U + (y / 2) * cw;
    const u8* vr = V + (y / 2) * cw;
    u8* o = dst + size_t(y) * w * 4;
    for (u32 x = 0; x < w; ++x) {
      int c = (int(yr[x]) - 16) * 298 + 128, d = int(ur[x / 2]) - 128, e = int(vr[x / 2]) - 128;
      u8 r = clamp8((c + rv * e) >> 8), g = clamp8((c - gu * d - gv * e) >> 8), b = clamp8((c + bu * d) >> 8);
      if (fmt == kFmtArgb) {
        o[0] = alpha, o[1] = r, o[2] = g, o[3] = b;
      } else {
        o[0] = r, o[1] = g, o[2] = b, o[3] = alpha;
      }
      o += 4;
    }
  }
}

// ---- library functions ----
s32 check_type(s32 codec, u32 level, u32 specific, u32* mem_size) {
  if (codec != kCodecMpeg2) {
    LOG_E("vdec", "codec type %d is not provided by this port (MPEG-2 only)", codec);
    return CELL_VDEC_ERROR_ARG;
  }
  if (level > 3) return CELL_VDEC_ERROR_ARG;  // MP@LL, MP@ML, MP@H14, MP@HL
  static const u32 maxw[4] = {352, 720, 1440, 1920}, maxh[4] = {288, 576, 1152, 1152};
  if (specific) {
    const auto* s = gp<CellVdecMpeg2SpecificInfo>(specific);
    if (s->thisSize != sizeof(CellVdecMpeg2SpecificInfo)) return CELL_VDEC_ERROR_ARG;
    if (s->maxDecodedFrameWidth > maxw[level] || s->maxDecodedFrameHeight > maxh[level]) return CELL_VDEC_ERROR_ARG;
  }
  // The firmware decoder's requirement for the profile level (its upper bound; this decoder
  // keeps its pictures in host memory and only places picture items in the client's memory).
  static const u32 mem_new[4] = {0x11290b, 0x2dfb8b, 0xa0270b, 0xd2f40b}, mem_old[4] = {0x2a610b, 0x47110b, 0xb8f90b, 0xeb990b};
  *mem_size = lv2::g_sdk_version >= 0x210000 ? mem_new[level] : mem_old[level];
  return CELL_OK;
}

s32 query_attr(s32 codec, u32 level, u32 specific, Ptr<CellVdecAttr> attr) {
  if (!attr) return CELL_VDEC_ERROR_ARG;
  attr->memSize = 0;
  u32 mem = 0;
  if (s32 r = check_type(codec, level, specific, &mem)) return r;
  attr->memSize = mem;
  attr->cmdDepth = u8(kCmdDepth);
  attr->decoderVerUpper = 0x4890000;
  attr->decoderVerLower = 0;  // codec core version: not reproduced
  return CELL_OK;
}

s32 cellVdecQueryAttr(Ptr<CellVdecType> type, Ptr<CellVdecAttr> attr) {
  if (!type) return CELL_VDEC_ERROR_ARG;
  return query_attr(type->codecType, type->profileLevel, 0, attr);
}
s32 cellVdecQueryAttrEx(Ptr<CellVdecTypeEx> type, Ptr<CellVdecAttr> attr) {
  if (!type) return CELL_VDEC_ERROR_ARG;
  return query_attr(type->codecType, type->profileLevel, type->codecSpecificInfo, attr);
}

s32 open(s32 codec, u32 level, u32 specific, u32 mem_addr, u32 mem_size, s32 prio, u32 stack, s32 spu_prio, Ptr<CellVdecCb> cb,
         Ptr<be32> handle) {
  if (!cb || !handle || !cb->cbFunc) return CELL_VDEC_ERROR_ARG;
  if (!mem_addr || u32(prio) >= 3072 || u32(spu_prio) >= 256 || stack < 4096) return CELL_VDEC_ERROR_ARG;
  u32 need = 0;
  if (s32 r = check_type(codec, level, specific, &need)) return r;
  if (need > mem_size) return CELL_VDEC_ERROR_ARG;
  auto v = std::make_shared<Vdec>();
  v->mem_addr = mem_addr;
  v->mem_size = mem_size;
  v->cb_func = cb->cbFunc;
  v->cb_arg = cb->cbArg;
  {
    std::lock_guard lk(g_mu);
    v->handle = g_next_handle;
    g_next_handle += 0x100;
    g_vdecs[v->handle] = v;
  }
  v->thread_id = thread_create_host("_cellVdecDecoder", prio, stack, [v](ppu::Context& ctx) { decoder_main(v, ctx); });
  *handle = v->handle;
  LOG_I("vdec", "open handle 0x%x: MPEG-2 level %u, memory 0x%x+0x%x, thread prio %d", v->handle, level, mem_addr, mem_size, prio);
  return CELL_OK;
}

s32 cellVdecOpen(Ptr<CellVdecType> type, Ptr<CellVdecResource> res, Ptr<CellVdecCb> cb, Ptr<be32> handle) {
  if (!type || !res) return CELL_VDEC_ERROR_ARG;
  return open(type->codecType, type->profileLevel, 0, res->memAddr, res->memSize, res->ppuThreadPriority, res->ppuThreadStackSize,
              res->spuThreadPriority, cb, handle);
}
s32 cellVdecOpenEx(Ptr<CellVdecTypeEx> type, Ptr<CellVdecResourceEx> res, Ptr<CellVdecCb> cb, Ptr<be32> handle) {
  if (!type || !res) return CELL_VDEC_ERROR_ARG;
  return open(type->codecType, type->profileLevel, type->codecSpecificInfo, res->memAddr, res->memSize, res->ppuThreadPriority,
              res->ppuThreadStackSize, res->spuThreadPriority, cb, handle);
}

s32 cellVdecClose(u32 handle) {
  auto v = get(handle);
  if (!v) return CELL_VDEC_ERROR_ARG;
  {
    std::lock_guard lk(v->mu);
    if (v->state == Seq::Closed) return CELL_VDEC_ERROR_SEQ;
    v->state = Seq::Closed;
    v->closing = true;
    v->cmds.push_back(Cmd{CmdType::Close});
  }
  v->cv.notify_all();
  if (cur_thread() != thread_get(v->thread_id)) {  // a callback closing its own decoder cannot wait for it
    std::unique_lock lk(v->mu);
    note_block();
    v->cv.wait(lk, [&] { return v->finished; });
  }
  {
    std::lock_guard lk(g_mu);
    g_vdecs.erase(handle);
  }
  LOG_I("vdec", "close handle 0x%x", handle);
  return CELL_OK;
}

s32 cellVdecStartSeq(u32 handle) {
  auto v = get(handle);
  if (!v) return CELL_VDEC_ERROR_ARG;
  {
    std::lock_guard lk(v->mu);
    if (v->state != Seq::Dormant && v->state != Seq::Ready) return CELL_VDEC_ERROR_SEQ;
    v->state = Seq::Ready;
    v->cmds.push_back(Cmd{CmdType::Start, ++v->seq_id});
  }
  v->cv.notify_all();
  return CELL_OK;
}

s32 cellVdecEndSeq(u32 handle) {
  auto v = get(handle);
  if (!v) return CELL_VDEC_ERROR_ARG;
  {
    std::lock_guard lk(v->mu);
    if (v->state != Seq::Ready) return CELL_VDEC_ERROR_SEQ;
    v->state = Seq::Ending;
    v->cmds.push_back(Cmd{CmdType::Drain, v->seq_id});
    v->cmds.push_back(Cmd{CmdType::End, v->seq_id});
  }
  v->cv.notify_all();
  return CELL_OK;
}

s32 cellVdecDecodeAu(u32 handle, s32 mode, Ptr<CellVdecAuInfo> au) {
  auto v = get(handle);
  if (!v || !au || !au->size || !au->startAddr) return CELL_VDEC_ERROR_ARG;
  {
    std::lock_guard lk(v->mu);
    if (v->state != Seq::Ready) return CELL_VDEC_ERROR_SEQ;
  }
  if (mode != kModeNormal && mode != kModeBSkip) return CELL_VDEC_ERROR_ARG;  // PB_SKIP: AVC only; 3: VC-1 only
  Cmd c{CmdType::Decode};
  c.mode = mode;
  c.pts = u64(au->pts.upper) << 32 | au->pts.lower;
  c.dts = u64(au->dts.upper) << 32 | au->dts.lower;
  c.user = au->userData;
  c.data.assign(ppu::mem() + au->startAddr, ppu::mem() + au->startAddr + au->size);
  {
    std::lock_guard lk(v->mu);
    if (v->au_count >= kCmdDepth) return CELL_VDEC_ERROR_BUSY;  // queue full: the client retries
    ++v->au_count;
    c.seq = v->seq_id;
    v->cmds.push_back(std::move(c));
  }
  v->cv.notify_all();
  return CELL_OK;
}

s32 cellVdecGetPicture(u32 handle, Ptr<CellVdecPicFormat> format, u32 out_buff) {
  auto v = get(handle);
  if (!v || !format) return CELL_VDEC_ERROR_ARG;
  u32 fmt = format->formatType, matrix = format->colorMatrixType;
  if (fmt > kFmtYuv420 || (fmt <= kFmtRgba && matrix > 1)) return CELL_VDEC_ERROR_ARG;
  Picture p;
  u64 seq = 0;
  {
    std::lock_guard lk(v->mu);
    if (v->state == Seq::Closed) return CELL_VDEC_ERROR_SEQ;
    if (v->out.empty()) return CELL_VDEC_ERROR_EMPTY;
    p = std::move(v->out.front());
    v->out.pop_front();
    seq = v->seq_id;
  }
  v->cv.notify_all();  // room for the decoder
  if (p.seq != seq) return CELL_VDEC_ERROR_EMPTY;
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1) < 8)
    LOG_I("vdec", "handle 0x%x: GetPicture format %u matrix %u alpha %u -> 0x%x (%ux%u)", handle, fmt, matrix, format->alpha,
          out_buff, p.width, p.height);
  if (out_buff && !p.yuv.empty()) convert(p, fmt, matrix, format->alpha, ppu::mem() + out_buff);
  return CELL_OK;
}

s32 cellVdecGetPicItem(u32 handle, Ptr<be32> item_out) {
  auto v = get(handle);
  if (!v || !item_out) return CELL_VDEC_ERROR_ARG;
  std::lock_guard lk(v->mu);
  if (v->state == Seq::Closed) return CELL_VDEC_ERROR_SEQ;
  auto it = std::find_if(v->out.begin(), v->out.end(), [](const Picture& p) { return !p.item_taken; });
  if (it == v->out.end() || it->seq != v->seq_id) return CELL_VDEC_ERROR_EMPTY;
  it->item_taken = true;
  u32 slots = std::clamp<u32>(v->mem_size / kItemSlot, 1, 128);
  u32 addr = v->mem_addr + (v->item_next++ % slots) * kItemSlot;
  auto* item = gp<CellVdecPicItem>(addr);
  std::memset(item, 0, kItemSlot);
  item->codecType = kCodecMpeg2;
  item->startAddr = 0;  // pictures stay inside the decoder; GetPicture copies them out
  item->size = (it->width * it->height * 3 / 2 + 127) & ~127u;
  item->auNum = 1;
  item->auPts[0].upper = u32(it->pts >> 32);
  item->auPts[0].lower = u32(it->pts);
  item->auPts[1].upper = item->auPts[1].lower = 0xffffffffu;
  item->auDts[0].upper = u32(it->dts >> 32);
  item->auDts[0].lower = u32(it->dts);
  item->auDts[1].upper = item->auDts[1].lower = 0xffffffffu;
  item->auUserData[0] = it->user;
  item->status = CELL_OK;
  item->attr = it->attr;
  item->picInfo = addr + sizeof(CellVdecPicItem);
  std::memcpy(gp<u8>(addr + sizeof(CellVdecPicItem)), &it->info, sizeof(CellVdecMpeg2Info));
  *item_out = addr;
  return CELL_OK;
}

s32 cellVdecSetFrameRate(u32 handle, s32 frc) {
  auto v = get(handle);
  if (!v || (frc & 0xf8) != 0x80) return CELL_VDEC_ERROR_ARG;
  {
    std::lock_guard lk(v->mu);
    if (v->state == Seq::Closed) return CELL_VDEC_ERROR_SEQ;
    Cmd c{CmdType::FrameRate, v->seq_id};
    c.frc = u32(frc) & 0x87;
    v->cmds.push_back(std::move(c));
  }
  v->cv.notify_all();
  return CELL_OK;
}

}  // namespace

// Diagnostics at shutdown: decoder states.
void vdec_dump() {
  std::lock_guard lk(g_mu);
  for (auto& [h, v] : g_vdecs) {
    std::lock_guard vl(v->mu);
    LOG_I("dump", "vdec 0x%x: state %d seq %llu, %zu commands queued, %u AUs in flight, %zu pictures queued (%zu items taken), "
          "%zu access units held by the decoder, %llu AUs given",
          h, int(v->state), (unsigned long long)v->seq_id, v->cmds.size(), v->au_count, v->out.size(),
          size_t(std::count_if(v->out.begin(), v->out.end(), [](const Picture& p) { return p.item_taken; })), v->pending.size(),
          (unsigned long long)v->next_tag);
  }
}

static hle::Module s_module("libvdec", {
    HLE_FN(cellVdecClose),
    HLE_FN(cellVdecDecodeAu),
    HLE_FN(cellVdecEndSeq),
    HLE_FN(cellVdecGetPicItem),
    HLE_FN(cellVdecGetPicture),
    HLE_FN(cellVdecOpen),
    HLE_FN(cellVdecOpenEx),
    HLE_FN(cellVdecQueryAttr),
    HLE_FN(cellVdecQueryAttrEx),
    HLE_FN(cellVdecSetFrameRate),
    HLE_FN(cellVdecStartSeq),
});

}  // namespace rt
