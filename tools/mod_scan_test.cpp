// Host self test for the data mod folders (src/sprite_mod.cpp): mod.toml parse, the scan order, the conflict rule
// and the on/off switch at lookup. It writes a small tree under the folder given as the first argument.
//   g++ -std=c++17 -Wall -Wextra -o mod_scan_test tools/mod_scan_test.cpp src/sprite_mod.cpp && ./mod_scan_test <empty dir>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/sprite_mod.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

// A 1x1 RGBA PNG (one red pixel).
static const unsigned char kPng[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
                                     8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 248, 207,
                                     192, 240, 31, 0, 5, 0, 1, 255, 137, 153, 61, 29, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66,
                                     96, 130};

static void Put(const std::string& path, const void* data, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::printf("cannot write %s\n", path.c_str()); std::exit(2); }
  std::fwrite(data, 1, n, f);
  std::fclose(f);
}
static void Text(const std::string& path, const std::string& t) { Put(path, t.data(), t.size()); }
static void Png(const std::string& path) { Put(path, kPng, sizeof(kPng)); }

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string root = std::string(argv[1]) + "/";

  // 1. mod.toml
  {
    gh::ModInfo m;
    std::string err;
    CHECK(gh::ParseModToml("# a mod\nid = \"hd-heroes\"\nname = \"HD Heroes\"  # shown in the menu\nversion = \"1.2\"\n"
                           "author = 'Someone'\ndescription = \"a = b\"\nkind = \"sprites\"\nlicence = \"CC-BY-4.0\"\n"
                           "order = -5\n\n[extra]\nunknown = 1\r\n", m, err));
    CHECK(m.id == "hd-heroes" && m.name == "HD Heroes" && m.version == "1.2" && m.author == "Someone");
    CHECK(m.description == "a = b" && m.kind == "sprites" && m.license == "CC-BY-4.0" && m.order == -5);
    gh::ModInfo bad;
    CHECK(!gh::ParseModToml("name \"x\"\n", bad, err) && !err.empty());
    CHECK(!gh::ParseModToml("name = \"x\n", bad, err));
  }

  // 2. The scan: order, legacy folders, skipped folders.
  const std::string a = gh::SpriteId(1, 1, 0xAAAA), b = gh::SpriteId(1, 1, 0xBBBB), c = gh::SpriteId(1, 1, 0xCCCC);
  for (const char* d : {"", "zeta", "zeta/sprites", "alpha", "alpha/sprites", "alpha/textures", "first", "first/sprites",
                        "nomanifest", "nomanifest/sprites", "wrongid", "wrongid/sprites", "empty", "sprites", "textures"})
    MKDIR((root + d).c_str());
  Text(root + "zeta/mod.toml", "name = \"Zeta\"\n");
  Text(root + "alpha/mod.toml", "id = \"alpha\"\nname = \"Alpha pack\"\nauthor = \"A\"\n");
  Text(root + "first/mod.toml", "name = \"First\"\norder = -1\n");
  Text(root + "wrongid/mod.toml", "id = \"other\"\n");
  Text(root + "empty/mod.toml", "name = \"Empty\"\n");
  Png(root + "zeta/sprites/x__" + a + ".png");
  Png(root + "zeta/sprites/" + c + ".png");
  Png(root + "alpha/sprites/hero__" + a + ".png");
  Png(root + "alpha/sprites/" + b + ".png");
  Png(root + "alpha/textures/" + a + ".png");
  Png(root + "first/sprites/" + b + ".png");
  Png(root + "sprites/old__" + a + ".png");
  Png(root + "nomanifest/sprites/" + a + ".png");

  std::vector<std::string> notes;
  const std::vector<gh::ModInfo> mods = gh::ScanMods(root, &notes);
  CHECK(mods.size() == 4);
  if (mods.size() == 4) {
    CHECK(mods[0].id == "first" && mods[1].id == "alpha" && mods[2].id == "zeta" && mods[3].id == "legacy-sprites");
    CHECK(mods[1].name == "Alpha pack" && mods[2].name == "Zeta" && mods[3].legacy);
    CHECK(!mods[1].textures_dir.empty() && mods[2].textures_dir.empty());
    CHECK(mods[3].sprites_dir == root + "sprites/");
  }
  CHECK(notes.size() == 3);  // nomanifest, wrongid, empty; the empty legacy textures/ folder is no mod

  // 3. Conflicts and the switches.
  if (mods.size() == 4) {
    bool on[4] = {true, true, true, true};
    gh::SpriteMod store;
    store.Reset(false);
    for (int i = 0; i < 4; i++) store.AddDir(mods[i].sprites_dir, i);
    store.SetEnabled([&](int m) { return on[m]; });
    CHECK(store.file_count() == 6);
    CHECK(store.conflicts().size() == 3);  // b: first over alpha; a: alpha over zeta, alpha over legacy
    for (const auto& k : store.conflicts()) CHECK(k.winner < k.loser);
    gh::ModSprite* s = store.Find(1, 1, 0xAAAA);
    CHECK(s && s->mod == 1);
    on[1] = false;
    s = store.Find(1, 1, 0xAAAA);
    CHECK(s && s->mod == 2);
    on[2] = false;
    s = store.Find(1, 1, 0xAAAA);
    CHECK(s && s->mod == 3);
    on[3] = false;
    CHECK(store.Find(1, 1, 0xAAAA) == nullptr);
    on[1] = true;
    s = store.Find(1, 1, 0xAAAA);
    CHECK(s && s->mod == 1);
    s = store.Find(1, 1, 0xBBBB);
    CHECK(s && s->mod == 0);
    CHECK(store.Find(1, 1, 0xDDDD) == nullptr);
    CHECK(store.HasSize(1, 1) && !store.HasSize(2, 1));
  }

  std::printf(g_fail ? "%d check(s) failed\n" : "mod_scan_test: all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
