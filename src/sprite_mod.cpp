#include "sprite_mod.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace gh {

uint64_t SpriteHash(const uint8_t* data, size_t n) {
  // CRC-32 (zlib) in the high half, Adler-32 in the low half: both are one call in Python's zlib.
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu, a = 1, b = 0;
  for (size_t i = 0; i < n; i++) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (uint64_t(crc ^ 0xFFFFFFFFu) << 32) | ((b << 16) | a);
}

std::string SpriteId(uint32_t w, uint32_t h, uint64_t hash) {
  char b[64];
  std::snprintf(b, sizeof(b), "%ux%u_%016llx", w, h, (unsigned long long)hash);
  return b;
}

// ----- inflate (RFC 1951), the compact canonical-Huffman form ----------------------------------------------
namespace {

struct BitReader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool err = false;
  uint32_t Bits(int need) {
    uint32_t v = buf;
    while (cnt < need) {
      if (pos >= n) { err = true; return 0; }
      v |= uint32_t(p[pos++]) << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    return v & ((1u << need) - 1);
  }
};

struct Huff {
  uint16_t count[16];
  uint16_t symbol[320];
};

void BuildHuff(Huff& h, const uint8_t* len, int n) {
  std::memset(h.count, 0, sizeof(h.count));
  for (int i = 0; i < n; i++) h.count[len[i]]++;
  uint16_t offs[16];
  offs[1] = 0;
  for (int i = 1; i < 15; i++) offs[i + 1] = uint16_t(offs[i] + h.count[i]);
  for (int i = 0; i < n; i++)
    if (len[i]) h.symbol[offs[len[i]]++] = uint16_t(i);
}

int DecodeSym(BitReader& b, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; len++) {
    code |= int(b.Bits(1));
    int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
    if (b.err) return -1;
  }
  return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool InflateCodes(BitReader& b, std::vector<uint8_t>& out, const Huff& lit, const Huff& dist) {
  for (;;) {
    int s = DecodeSym(b, lit);
    if (s < 0) return false;
    if (s < 256) { out.push_back(uint8_t(s)); continue; }
    if (s == 256) return true;
    s -= 257;
    if (s >= 29) return false;
    int len = kLenBase[s] + int(b.Bits(kLenExtra[s]));
    int ds = DecodeSym(b, dist);
    if (ds < 0 || ds >= 30) return false;
    size_t d = kDistBase[ds] + b.Bits(kDistExtra[ds]);
    if (b.err || d > out.size()) return false;
    size_t from = out.size() - d;
    for (int i = 0; i < len; i++) out.push_back(out[from + i]);
  }
}

bool Inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  BitReader b{src, n};
  int last;
  do {
    last = int(b.Bits(1));
    int type = int(b.Bits(2));
    if (b.err) return false;
    if (type == 0) {
      b.buf = 0; b.cnt = 0;
      if (b.pos + 4 > n) return false;
      uint32_t len = src[b.pos] | (src[b.pos + 1] << 8);
      b.pos += 4;
      if (b.pos + len > n) return false;
      out.insert(out.end(), src + b.pos, src + b.pos + len);
      b.pos += len;
    } else if (type == 1) {
      static Huff fl, fd;
      static bool init = false;
      if (!init) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        BuildHuff(fl, l, 288);
        uint8_t dl[30];
        for (i = 0; i < 30; i++) dl[i] = 5;
        BuildHuff(fd, dl, 30);
        init = true;
      }
      if (!InflateCodes(b, out, fl, fd)) return false;
    } else if (type == 2) {
      int nlen = int(b.Bits(5)) + 257, ndist = int(b.Bits(5)) + 1, ncode = int(b.Bits(4)) + 4;
      if (nlen > 286 || ndist > 30) return false;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t lens[320] = {};
      for (int i = 0; i < ncode; i++) lens[order[i]] = uint8_t(b.Bits(3));
      Huff cl;
      BuildHuff(cl, lens, 19);
      uint8_t ll[320] = {};
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = DecodeSym(b, cl);
        if (sym < 0) return false;
        if (sym < 16) { ll[idx++] = uint8_t(sym); continue; }
        int rep, val = 0;
        if (sym == 16) {
          if (idx == 0) return false;
          val = ll[idx - 1];
          rep = 3 + int(b.Bits(2));
        } else if (sym == 17) rep = 3 + int(b.Bits(3));
        else rep = 11 + int(b.Bits(7));
        if (idx + rep > nlen + ndist) return false;
        while (rep--) ll[idx++] = uint8_t(val);
      }
      Huff lit, dist;
      BuildHuff(lit, ll, nlen);
      BuildHuff(dist, ll + nlen, ndist);
      if (!InflateCodes(b, out, lit, dist)) return false;
    } else {
      return false;
    }
    if (b.err) return false;
  } while (!last);
  return true;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

}  // namespace

// ----- PNG -----------------------------------------------------------------------------------------------
bool DecodePng(const std::vector<uint8_t>& f, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err,
               std::vector<uint8_t>* indices) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (f.size() < 33 || std::memcmp(f.data(), sig, 8) != 0) { err = "not a PNG"; return false; }
  uint32_t depth = 0, ctype = 0;
  std::vector<uint8_t> idat, plte, trns;
  size_t pos = 8;
  bool have_ihdr = false;
  while (pos + 12 <= f.size()) {
    uint32_t len = Be32(&f[pos]);
    const uint8_t* t = &f[pos + 4];
    if (pos + 12 + len > f.size()) { err = "truncated chunk"; return false; }
    const uint8_t* d = &f[pos + 8];
    if (!std::memcmp(t, "IHDR", 4) && len >= 13) {
      w = Be32(d); h = Be32(d + 4); depth = d[8]; ctype = d[9];
      if (d[12] != 0) { err = "interlaced PNG not supported"; return false; }
      have_ihdr = true;
    } else if (!std::memcmp(t, "PLTE", 4)) plte.assign(d, d + len);
    else if (!std::memcmp(t, "tRNS", 4)) trns.assign(d, d + len);
    else if (!std::memcmp(t, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
    else if (!std::memcmp(t, "IEND", 4)) break;
    pos += 12 + len;
  }
  if (!have_ihdr || !w || !h || w > 16384 || h > 16384) { err = "bad IHDR"; return false; }
  uint32_t ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if (!ch || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { err = "unsupported PNG type"; return false; }
  if (idat.size() < 6) { err = "no image data"; return false; }
  std::vector<uint8_t> raw;
  raw.reserve(size_t(w) * h * ch * depth / 8 + h);
  if (!Inflate(idat.data() + 2, idat.size() - 2, raw)) { err = "inflate failed"; return false; }
  const size_t bpp = std::max<size_t>(1, ch * depth / 8);
  const size_t stride = (size_t(w) * ch * depth + 7) / 8;
  if (raw.size() < (stride + 1) * h) { err = "short image data"; return false; }
  std::vector<uint8_t> img(stride * h);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* s = &raw[y * (stride + 1)];
    uint8_t* o = &img[y * stride];
    const uint8_t* up = y ? &img[(y - 1) * stride] : nullptr;
    const uint8_t ft = s[0];
    for (size_t x = 0; x < stride; x++) {
      int a = x >= bpp ? o[x - bpp] : 0, b = up ? up[x] : 0, c = (x >= bpp && up) ? up[x - bpp] : 0;
      int v = s[1 + x];
      switch (ft) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: err = "bad filter"; return false;
      }
      o[x] = uint8_t(v);
    }
  }
  rgba.assign(size_t(w) * h * 4, 255);
  if (indices) {
    indices->clear();
    if (ctype == 3) {
      indices->resize(size_t(w) * h);
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* row = &img[y * stride];
          uint32_t per = 8 / depth;
          uint32_t byte = row[x / per];
          (*indices)[size_t(y) * w + x] = depth == 8 ? uint8_t(byte) : uint8_t((byte >> (8 - depth * (x % per + 1))) & ((1u << depth) - 1));
        }
    }
  }
  auto sample = [&](const uint8_t* row, uint32_t idx) -> uint32_t {  // idx-th sample of the row, scaled as stored
    if (depth == 8) return row[idx];
    if (depth == 16) return row[idx * 2];
    uint32_t per = 8 / depth, byte = row[idx / per];
    return (byte >> (8 - depth * (idx % per + 1))) & ((1u << depth) - 1);
  };
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* row = &img[y * stride];
    for (uint32_t x = 0; x < w; x++) {
      uint8_t* px = &rgba[(size_t(y) * w + x) * 4];
      if (ctype == 3) {
        uint32_t i = sample(row, x);
        if (i * 3 + 2 < plte.size()) { px[0] = plte[i * 3]; px[1] = plte[i * 3 + 1]; px[2] = plte[i * 3 + 2]; }
        px[3] = i < trns.size() ? trns[i] : 255;
      } else if (ctype == 0 || ctype == 4) {
        uint32_t g = sample(row, x * ch);
        if (depth < 8) g = g * 255 / ((1u << depth) - 1);
        px[0] = px[1] = px[2] = uint8_t(g);
        if (ctype == 4) px[3] = uint8_t(sample(row, x * ch + 1) * (depth < 8 ? 255 / ((1u << depth) - 1) : 1));
        else if (trns.size() >= 2 && depth == 8 && g == trns[1]) px[3] = 0;
      } else {
        for (int k = 0; k < 3; k++) px[k] = uint8_t(sample(row, x * ch + k));
        if (ctype == 6) px[3] = uint8_t(sample(row, x * ch + 3));
        else if (trns.size() >= 6 && depth == 8 && px[0] == trns[1] && px[1] == trns[3] && px[2] == trns[5]) px[3] = 0;
      }
    }
  }
  return true;
}

// ----- quantiser: <= 255 colours, index 0 transparent, alpha is binary ------------------------------------
bool QuantiseRgba(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, ModSprite& out) {
  struct Col { uint8_t c[3]; uint32_t n; };
  std::map<uint32_t, uint32_t> hist;  // packed RGB -> count
  const size_t px = size_t(w) * h;
  for (size_t i = 0; i < px; i++) {
    const uint8_t* p = &rgba[i * 4];
    if (p[3] < 128) continue;
    hist[(uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]]++;
  }
  std::vector<Col> cols;
  cols.reserve(hist.size());
  for (auto& kv : hist) cols.push_back(Col{{uint8_t(kv.first >> 16), uint8_t(kv.first >> 8), uint8_t(kv.first)}, kv.second});
  std::memset(out.palette, 0, sizeof(out.palette));
  std::vector<std::pair<uint32_t, uint8_t>> lut;  // packed colour -> palette index
  if (cols.size() <= 255) {
    for (size_t i = 0; i < cols.size(); i++) {
      uint8_t* e = &out.palette[(i + 1) * 4];
      e[0] = cols[i].c[0]; e[1] = cols[i].c[1]; e[2] = cols[i].c[2]; e[3] = 255;
    }
  } else {
    // Median cut over the distinct colours, weighted by pixel count.
    struct Box { size_t a, b; };  // [a, b) in `cols`
    std::vector<Box> boxes{{0, cols.size()}};
    while (boxes.size() < 255) {
      int best = -1, bch = 0;
      int bestrange = 0;
      for (size_t i = 0; i < boxes.size(); i++) {
        if (boxes[i].b - boxes[i].a < 2) continue;
        for (int k = 0; k < 3; k++) {
          int lo = 255, hi = 0;
          for (size_t j = boxes[i].a; j < boxes[i].b; j++) { lo = std::min<int>(lo, cols[j].c[k]); hi = std::max<int>(hi, cols[j].c[k]); }
          if (hi - lo > bestrange) { bestrange = hi - lo; best = int(i); bch = k; }
        }
      }
      if (best < 0) break;
      Box bx = boxes[best];
      std::sort(cols.begin() + bx.a, cols.begin() + bx.b, [bch](const Col& l, const Col& r) { return l.c[bch] < r.c[bch]; });
      uint64_t total = 0;
      for (size_t j = bx.a; j < bx.b; j++) total += cols[j].n;
      uint64_t acc = 0;
      size_t split = bx.a + 1;
      for (size_t j = bx.a; j < bx.b - 1; j++) {
        acc += cols[j].n;
        split = j + 1;
        if (acc * 2 >= total) break;
      }
      boxes[best] = Box{bx.a, split};
      boxes.push_back(Box{split, bx.b});
    }
    for (size_t i = 0; i < boxes.size(); i++) {
      uint64_t sum[3] = {0, 0, 0}, tot = 0;
      for (size_t j = boxes[i].a; j < boxes[i].b; j++) {
        for (int k = 0; k < 3; k++) sum[k] += uint64_t(cols[j].c[k]) * cols[j].n;
        tot += cols[j].n;
      }
      uint8_t* e = &out.palette[(i + 1) * 4];
      for (int k = 0; k < 3; k++) e[k] = uint8_t(tot ? (sum[k] + tot / 2) / tot : 0);
      e[3] = 255;
    }
  }
  // Index every distinct colour once: exact when it is in the palette, else the nearest entry.
  size_t used = 1;
  while (used < 256 && out.palette[used * 4 + 3]) used++;
  std::unordered_map<uint32_t, uint8_t> map;
  map.reserve(cols.size() * 2);
  for (const Col& c : cols) {
    int bestd = 1 << 30; uint8_t bi = 1;
    for (size_t i = 1; i < used; i++) {
      const uint8_t* e = &out.palette[i * 4];
      int dr = int(e[0]) - c.c[0], dg = int(e[1]) - c.c[1], db = int(e[2]) - c.c[2];
      int d = dr * dr + dg * dg + db * db;
      if (d < bestd) { bestd = d; bi = uint8_t(i); if (!d) break; }
    }
    map[(uint32_t(c.c[0]) << 16) | (uint32_t(c.c[1]) << 8) | c.c[2]] = bi;
  }
  out.w = w;
  out.h = h;
  out.index.assign(px, 0);
  for (size_t i = 0; i < px; i++) {
    const uint8_t* p = &rgba[i * 4];
    if (p[3] < 128) continue;
    out.index[i] = map[(uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]];
  }
  return true;
}

// ----- the store ---------------------------------------------------------------------------------------------
size_t SpriteMod::Init(const std::string& dir) {
  dir_ = dir;
  if (!dir_.empty() && dir_.back() != '/') dir_ += '/';
  files_.clear();
  DIR* d = opendir(dir_.c_str());
  if (!d) return 0;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".png") != 0) continue;
    std::string stem = n.substr(0, n.size() - 4);
    size_t p = stem.rfind("__");
    std::string id = p == std::string::npos ? stem : stem.substr(p + 2);
    files_[id] = dir_ + n;
  }
  closedir(d);
  return files_.size();
}

ModSprite* SpriteMod::Find(uint32_t w, uint32_t h, uint64_t hash) {
  if (files_.empty()) return nullptr;
  const std::string id = SpriteId(w, h, hash);
  auto lit = loaded_.find(id);
  if (lit != loaded_.end()) return lit->second;
  auto fit = files_.find(id);
  if (fit == files_.end()) return nullptr;
  ModSprite* result = nullptr;
  std::vector<uint8_t> file;
  if (FILE* fp = std::fopen(fit->second.c_str(), "rb")) {
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz > 0 && sz < (64 << 20)) {
      file.resize(size_t(sz));
      if (std::fread(file.data(), 1, file.size(), fp) != file.size()) file.clear();
    }
    std::fclose(fp);
  }
  uint32_t pw = 0, ph = 0;
  std::vector<uint8_t> rgba, indices;
  if (file.empty()) error_ = "cannot read " + fit->second;
  else if (DecodePng(file, pw, ph, rgba, error_, &indices)) {
    auto* s = new ModSprite();
    if (!indices.empty()) {
      s->w = pw; s->h = ph; s->index = indices; s->game_palette = true;
      result = s;
    } else if (QuantiseRgba(rgba, pw, ph, *s)) result = s;
    else delete s;
  } else {
    error_ = fit->second + ": " + error_;
  }
  loaded_[id] = result;
  return result;
}

}  // namespace gh
