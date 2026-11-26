#include "sprite_mod.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

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
void SpriteMod::Reset(bool true_colour) {
  true_colour_ = true_colour;
  files_.clear();
  sizes_.clear();
  conflicts_.clear();
  file_count_ = 0;
}

size_t SpriteMod::Init(const std::string& dir, bool true_colour) {
  Reset(true_colour);
  return AddDir(dir, 0);
}

size_t SpriteMod::AddDir(const std::string& dir_in, int mod) {
  std::string dir = dir_in;
  if (!dir.empty() && dir.back() != '/') dir += '/';
  DIR* d = opendir(dir.c_str());
  if (!d) return 0;
  size_t found = 0;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".png") != 0) continue;
    std::string stem = n.substr(0, n.size() - 4);
    size_t p = stem.rfind("__");
    std::string id = p == std::string::npos ? stem : stem.substr(p + 2);
    std::vector<File>& files = files_[id];
    if (!files.empty() && files.back().mod == mod) { files.back().path = dir + n; continue; }  // two names, one id
    if (!files.empty()) conflicts_.push_back({id, files.front().mod, mod});
    files.push_back({mod, dir + n});
    found++;
    unsigned w = 0, h = 0;
    if (std::sscanf(id.c_str(), "%ux%u_", &w, &h) == 2) sizes_.insert((uint64_t(w) << 32) | h);
  }
  closedir(d);
  file_count_ += found;
  return found;
}

ModSprite* SpriteMod::Find(uint32_t w, uint32_t h, uint64_t hash) {
  error_.clear();
  if (files_.empty()) return nullptr;
  auto fit = files_.find(SpriteId(w, h, hash));
  if (fit == files_.end()) return nullptr;
  for (const File& f : fit->second) {
    if (enabled_ && !enabled_(f.mod)) continue;
    auto lit = loaded_.find(f.path);
    ModSprite* s = lit != loaded_.end() ? lit->second : Load(f.path, f.mod);
    if (s) { error_.clear(); return s; }
  }
  return nullptr;
}

ModSprite* SpriteMod::Load(const std::string& path, int mod) {
  ModSprite* result = nullptr;
  std::vector<uint8_t> file;
  if (FILE* fp = std::fopen(path.c_str(), "rb")) {
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
  if (file.empty()) error_ = "cannot read " + path;
  else if (DecodePng(file, pw, ph, rgba, error_, true_colour_ ? nullptr : &indices)) {
    auto* s = new ModSprite();
    if (true_colour_) {
      s->w = pw; s->h = ph; s->true_colour = true; s->rgba = std::move(rgba);
      result = s;
    } else if (!indices.empty()) {
      s->w = pw; s->h = ph; s->index = indices; s->game_palette = true;
      result = s;
    } else if (QuantiseRgba(rgba, pw, ph, *s)) result = s;
    else delete s;
  } else {
    error_ = path + ": " + error_;
  }
  if (result) result->mod = mod;
  loaded_[path] = result;
  return result;
}

// ---------------------------------------------------------------------------------------------
// Mod folders
// ---------------------------------------------------------------------------------------------

bool ParseModToml(const std::string& text, ModInfo& out, std::string& err) {
  size_t pos = 0;
  int line_no = 0;
  auto trim = [](std::string v) {
    const char* ws = " \t\r";
    size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    return v.substr(a, v.find_last_not_of(ws) - a + 1);
  };
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = trim(text.substr(pos, end - pos));
    pos = end + 1;
    line_no++;
    if (line.empty() || line[0] == '#' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) { err = "line " + std::to_string(line_no) + ": no '='"; return false; }
    const std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
      size_t close = value.find(value[0], 1);
      if (close == std::string::npos) { err = "line " + std::to_string(line_no) + ": the text has no closing quote"; return false; }
      value = value.substr(1, close - 1);
    } else {
      value = trim(value.substr(0, value.find('#')));
    }
    if (key == "id") out.id = value;
    else if (key == "name") out.name = value;
    else if (key == "version") out.version = value;
    else if (key == "author") out.author = value;
    else if (key == "description") out.description = value;
    else if (key == "kind") out.kind = value;
    else if (key == "license" || key == "licence") out.license = value;
    else if (key == "order") out.order = std::atoi(value.c_str());
  }
  return true;
}

namespace {

bool HasPng(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return false;
  bool found = false;
  while (dirent* e = readdir(d)) {
    const size_t n = std::strlen(e->d_name);
    if (n > 4 && !std::strcmp(e->d_name + n - 4, ".png")) { found = true; break; }
  }
  closedir(d);
  return found;
}

bool IsDir(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (d) closedir(d);
  return d != nullptr;
}

bool ValidId(const std::string& id) {
  if (id.empty() || id.size() > 31) return false;
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}

}  // namespace

std::vector<ModInfo> ScanMods(const std::string& mods_dir_in, std::vector<std::string>* notes) {
  std::string mods_dir = mods_dir_in;
  if (!mods_dir.empty() && mods_dir.back() != '/') mods_dir += '/';
  auto note = [&](const std::string& t) { if (notes) notes->push_back(t); };
  std::vector<ModInfo> mods;
  std::vector<std::string> names;
  if (DIR* d = opendir(mods_dir.c_str())) {
    while (dirent* e = readdir(d)) names.push_back(e->d_name);
    closedir(d);
  }
  for (const std::string& n : names) {
    if (n == "." || n == ".." || n == "sprites" || n == "textures") continue;
    const std::string dir = mods_dir + n + "/";
    if (!IsDir(dir)) continue;
    std::string text;
    if (FILE* fp = std::fopen((dir + "mod.toml").c_str(), "rb")) {
      char buf[4096];
      size_t got;
      while ((got = std::fread(buf, 1, sizeof(buf), fp)) > 0 && text.size() < 65536) text.append(buf, got);
      std::fclose(fp);
    } else {
      note(n + ": no mod.toml, skipped");
      continue;
    }
    ModInfo m;
    std::string err;
    if (!ParseModToml(text, m, err)) { note(n + ": mod.toml " + err + ", skipped"); continue; }
    if (m.id.empty()) m.id = n;
    if (m.id != n) { note(n + ": the id \"" + m.id + "\" is not the folder name, skipped"); continue; }
    if (!ValidId(m.id) || m.id.compare(0, 7, "legacy-") == 0) {
      note(n + ": the id must be 1-31 characters of a-z, 0-9, _ and -, and not start with legacy-; skipped");
      continue;
    }
    if (m.name.empty()) m.name = m.id;
    if (IsDir(dir + "sprites/")) m.sprites_dir = dir + "sprites/";
    if (IsDir(dir + "textures/")) m.textures_dir = dir + "textures/";
    if (m.sprites_dir.empty() && m.textures_dir.empty()) { note(n + ": no sprites/ or textures/ folder, skipped"); continue; }
    mods.push_back(m);
  }
  std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) {
    return a.order != b.order ? a.order < b.order : a.id < b.id;
  });
  if (HasPng(mods_dir + "sprites/")) {
    ModInfo m;
    m.id = "legacy-sprites"; m.name = "legacy sprites"; m.kind = "sprites"; m.legacy = true;
    m.description = "PNG files placed directly in mods/sprites/ (the old layout)";
    m.sprites_dir = mods_dir + "sprites/";
    mods.push_back(m);
  }
  if (HasPng(mods_dir + "textures/")) {
    ModInfo m;
    m.id = "legacy-textures"; m.name = "legacy textures"; m.kind = "textures"; m.legacy = true;
    m.description = "PNG files placed directly in mods/textures/ (the old layout)";
    m.textures_dir = mods_dir + "textures/";
    mods.push_back(m);
  }
  return mods;
}

}  // namespace gh
