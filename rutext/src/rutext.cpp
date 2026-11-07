// RuText.asi - shows hard-coded English text of RDR2 ScriptHook mods in another language
// (Russian dictionaries are what it was made for) and remembers Bandit Hideouts hideouts.
//
// Mods built against ScriptHookRDR2 call natives through three exports of
// ScriptHookRDR2.dll: nativeInit(hash), nativePush64(arg) for every argument and
// nativeCall(). Text reaches the screen through CREATE_VAR_STRING(flags,
// "LITERAL_STRING", text) and DATABINDING_ADD_DATA_STRING(parent, name, text).
// RuText rewrites the import table entries of those three exports in every loaded
// module that imports them, and when a text argument is pushed it swaps the
// pointer for the Russian string from its dictionaries. Mod files stay untouched.
//
// Dictionaries: <game>\RuText\*.txt, UTF-8. The part of the file name before the
// first dot selects the module (PedDamageOverhaul.txt applies to
// PedDamageOverhaul.asi); "_common" applies to every module.
//   English text => Русский текст      exact match
//   Load Slot * => Загрузить слот *    pattern, each * captures text
//   ~Big Valley => Большая долина      replaced inside any longer string
//   # comment; \n and \t escapes work on both sides
// Untranslated strings that reach the screen are written to
// <game>\RuText\untranslated\<module>.txt when Capture=1 in RuText.ini.
//
// Hideout memory (section [HideoutMemory] in RuText.ini): Bandit Hideouts with
// ALWAYS_SHOW_HIDEOUT_BLIP=1 puts a skull on the map for every hideout. RuText watches
// the blips that module creates, hides the skulls of hideouts the player has not been
// near yet (BLIP_MODIFIER_HIDDEN) and shows a skull for good once the player comes
// within Radius metres. Visited hideouts are kept in <game>\RuText\HideoutMemory.dat.
// Bandit Hideouts still removes the skull of a cleared hideout until it respawns.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kCreateVarString = 0xFA925AC00EB830B9ULL;
constexpr uint64_t kDatabindingAddDataString = 0x617FCA1C5652BBADULL;

// hideout memory
constexpr uint64_t kBlipAddForCoords = 0x554D9D53F696D002ULL;
constexpr uint64_t kBlipAddForRadius = 0x45F13B7E0A15C880ULL;  // what Bandit Hideouts uses
constexpr uint64_t kSetBlipSprite = 0x74F74D3207ED525CULL;
constexpr uint64_t kDoesBlipExist = 0xCD82FA174080B3B1ULL;
constexpr uint64_t kBlipAddModifier = 0x662D364ABF16DE2FULL;
constexpr uint64_t kBlipRemoveModifier = 0xB059D7BD3D78C16FULL;
constexpr uint64_t kPlayerPedId = 0x096275889B8E0EE0ULL;
constexpr uint64_t kGetEntityCoords = 0xA86D5F069399F44DULL;
constexpr uint32_t kModifierHidden = 0xB946AEF0;   // BLIP_MODIFIER_HIDDEN
constexpr uint32_t kSpriteHideout = 0x5846C31D;    // blip_ambient_bounty_target (HIDEOUT_ICON=1)

constexpr const char* kExpInit = "?nativeInit@@YAX_K@Z";
constexpr const char* kExpPush = "?nativePush64@@YAX_K@Z";
constexpr const char* kExpCall = "?nativeCall@@YAPEA_KXZ";
constexpr const char* kExpScriptRegister = "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z";
constexpr const char* kExpScriptWait = "?scriptWait@@YAXK@Z";

using FnInit = void (*)(uint64_t);
using FnPush = void (*)(uint64_t);
using FnCall = uint64_t* (*)();
using FnScriptRegister = void (*)(HMODULE, void (*)());
using FnScriptWait = void (*)(DWORD);

HMODULE g_self = nullptr;
HMODULE g_sh = nullptr;
FnInit g_origInit = nullptr;
FnPush g_origPush = nullptr;
FnCall g_origCall = nullptr;
FnScriptWait g_scriptWait = nullptr;

std::wstring g_dir;  // <game>\RuText
bool g_capture = true;
bool g_enabled = true;

SRWLOCK g_lock = SRWLOCK_INIT;  // guards logs, memo and module table

// ---------------------------------------------------------------- logging

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// ---------------------------------------------------------------- dictionaries

struct Pattern {
  std::vector<std::string> parts;  // literal pieces between '*'
  std::string out;                 // '*' in out = next capture
};

// A long text a mod word-wraps itself before drawing: each drawn line reaches us
// alone. Paragraphs are separated by the mod's own "|n|" marker on both sides.
struct LongEntry {
  std::string en, ru;
  std::vector<std::pair<size_t, size_t>> enPara, ruPara;  // [begin, end) per paragraph
};

struct Scope {
  std::unordered_map<std::string, std::string> exact;
  std::unordered_map<std::string, std::string> lowerExact;  // ASCII-lowercased keys
  std::vector<Pattern> patterns;
  std::vector<std::pair<std::string, std::string>> subs;  // longest first
  std::vector<LongEntry> longs;                           // for wrapped lines
};

std::vector<std::pair<size_t, size_t>> Paragraphs(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> r;
  size_t a = 0;
  for (;;) {
    size_t b = s.find("|n|", a);
    r.emplace_back(a, b == std::string::npos ? s.size() : b);
    if (b == std::string::npos) break;
    a = b + 3;
  }
  return r;
}

using ScopeMap = std::unordered_map<std::string, Scope>;  // key: lower module name or "_common"
ScopeMap* g_dict = new ScopeMap;
std::vector<ScopeMap*> g_oldDicts;  // kept alive: the game may still hold pointers into them

std::string Unescape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[i + 1];
      if (n == 'n') { r += '\n'; ++i; continue; }
      if (n == 't') { r += '\t'; ++i; continue; }
      if (n == '\\') { r += '\\'; ++i; continue; }
    }
    r += s[i];
  }
  return r;
}

std::string Escape(const char* s) {
  std::string r;
  for (; *s; ++s) {
    if (*s == '\n') r += "\\n";
    else if (*s == '\t') r += "\\t";
    else if (*s == '\r') r += "\\r";
    else r += *s;
  }
  return r;
}

void LoadDictFile(const std::wstring& path, const std::string& scopeName) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return;
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  fclose(f);
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
      (unsigned char)data[2] == 0xBF)
    data.erase(0, 3);

  Scope& sc = (*g_dict)[scopeName];
  int exact = 0, pats = 0, subs = 0;
  size_t pos = 0;
  while (pos <= data.size()) {
    size_t e = data.find('\n', pos);
    if (e == std::string::npos) e = data.size();
    std::string line = data.substr(pos, e - pos);
    pos = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t sep = line.find(" => ");
    if (sep == std::string::npos) continue;
    std::string key = line.substr(0, sep);
    std::string val = Unescape(line.substr(sep + 4));
    if (key.empty()) continue;
    if (key[0] == '~') {
      key = Unescape(key.substr(1));
      if (!key.empty()) { sc.subs.emplace_back(key, val); ++subs; }
    } else if (key.find('*') != std::string::npos) {
      Pattern p;
      std::string k = Unescape(key);
      size_t a = 0;
      for (;;) {
        size_t b = k.find('*', a);
        p.parts.push_back(k.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
      }
      p.out = val;
      sc.patterns.push_back(std::move(p));
      ++pats;
    } else {
      std::string k = Unescape(key);
      sc.lowerExact[Lower(k)] = val;
      sc.exact[k] = val;
      ++exact;
    }
    if (e == data.size()) break;
  }
  std::sort(sc.subs.begin(), sc.subs.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  sc.longs.clear();
  for (const auto& [en, ru] : sc.exact) {
    if (en.size() < 40 || en.find(' ') == std::string::npos) continue;
    LongEntry le{en, ru, Paragraphs(en), Paragraphs(ru)};
    if (le.enPara.size() != le.ruPara.size()) {
      Log("  wrapped-line support off for an entry: %zu vs %zu paragraphs: %.60s", le.enPara.size(),
          le.ruPara.size(), en.c_str());
      continue;
    }
    sc.longs.push_back(std::move(le));
  }
  Log("dictionary %s -> scope '%s': %d exact, %d patterns, %d substrings", Narrow(path).c_str(),
      scopeName.c_str(), exact, pats, subs);
}

void LoadDictionaries() {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_dir + L"\\*.txt").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    Log("no dictionaries in %s", Narrow(g_dir).c_str());
    return;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string name = Narrow(fd.cFileName);
    std::string scope = Lower(name.substr(0, name.find('.')));
    LoadDictFile(g_dir + L"\\" + fd.cFileName, scope);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// Glob with '*' captures, leftmost-shortest per star with backtracking.
bool MatchParts(const std::vector<std::string>& parts, size_t pi, const char* s,
                std::vector<std::string>& caps) {
  if (pi == parts.size()) return *s == '\0';
  const std::string& lit = parts[pi];
  if (pi == 0) {
    if (strncmp(s, lit.c_str(), lit.size()) != 0) return false;
    return MatchParts(parts, 1, s + lit.size(), caps);
  }
  // a capture precedes this literal
  if (pi == parts.size() - 1 && lit.empty()) {
    caps.emplace_back(s);
    return true;
  }
  for (const char* p = s; *p || lit.empty(); ++p) {
    if (strncmp(p, lit.c_str(), lit.size()) == 0) {
      caps.emplace_back(s, p - s);
      if (MatchParts(parts, pi + 1, p + lit.size(), caps)) return true;
      caps.pop_back();
    }
    if (!*p) break;
  }
  return false;
}

bool ApplyPattern(const Pattern& p, const char* s, std::string& out) {
  std::vector<std::string> caps;
  if (!MatchParts(p.parts, 0, s, caps)) return false;
  out.clear();
  size_t ci = 0;
  for (char c : p.out) {
    if (c == '*' && ci < caps.size()) out += caps[ci++];
    else out += c;
  }
  return true;
}

bool ApplySubs(const Scope* const* scopes, int ns, std::string& text) {
  bool changed = false;
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    for (const auto& [from, to] : scopes[i]->subs) {
      size_t p = 0;
      while ((p = text.find(from, p)) != std::string::npos) {
        text.replace(p, from.size(), to);
        p += to.size();
        changed = true;
      }
    }
  }
  return changed;
}

bool IsAllCaps(const std::string& s) {
  int letters = 0;
  for (unsigned char c : s) {
    if (c >= 'a' && c <= 'z') return false;
    if (c >= 'A' && c <= 'Z') ++letters;
  }
  return letters >= 2;
}

std::string UpperUtf8(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return s;
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  CharUpperBuffW(w.data(), (DWORD)w.size());
  return Narrow(w);
}

// Whole-string lookup: exact, then case-insensitive for ALL-CAPS text (result in
// caps), then patterns. Substring rules are applied by the caller.
bool LookupWhole(const Scope* const* scopes, int ns, const std::string& s, std::string& out) {
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    auto e = scopes[i]->exact.find(s);
    if (e != scopes[i]->exact.end()) { out = e->second; return true; }
  }
  if (IsAllCaps(s)) {
    std::string low = Lower(s);
    for (int i = 0; i < ns; ++i) {
      if (!scopes[i]) continue;
      auto e = scopes[i]->lowerExact.find(low);
      if (e != scopes[i]->lowerExact.end()) { out = UpperUtf8(e->second); return true; }
    }
  }
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    for (const Pattern& p : scopes[i]->patterns)
      if (ApplyPattern(p, s.c_str(), out)) return true;
  }
  return false;
}

// Russian position for an English word boundary: same relative place inside the
// paragraph, snapped to the nearest space. Neighbouring lines share the boundary,
// so their Russian pieces tile the paragraph without gaps or overlaps.
size_t SnapBoundary(const std::string& ru, size_t rb, size_t re, double frac) {
  if (frac <= 0.0) return rb;
  if (frac >= 1.0) return re;
  size_t t = rb + (size_t)((re - rb) * frac + 0.5);
  for (size_t d = 0; d < re - rb; ++d) {
    if (t >= rb + d && ru[t - d] == ' ') return t - d;
    if (t + d < re && ru[t + d] == ' ') return t + d;
  }
  return t;
}

bool UtfBoundary(const std::string& s, size_t i) {
  return i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80;
}

struct LastFrag {
  const LongEntry* entry = nullptr;
  size_t end = 0;
};
thread_local LastFrag t_last;
thread_local bool t_last_hit = false;

bool FragmentLookup(const Scope* const* scopes, int ns, const std::string& f, std::string& out,
                    bool& contextual) {
  const LongEntry* best = nullptr;
  size_t pos = std::string::npos;
  contextual = false;
  // the next line of the text matched last time comes first; this also catches
  // short last lines ("lands.") that are too ambiguous to search for on their own
  if (t_last.entry && !f.empty()) {
    size_t p = t_last.entry->en.find(f, t_last.end > 3 ? t_last.end - 3 : 0);
    if (p != std::string::npos && p <= t_last.end + 4) { best = t_last.entry; pos = p; contextual = true; }
  }
  if (!best && (f.size() < 12 || f.find(' ') == std::string::npos)) return false;
  for (int i = 0; i < ns && !best; ++i) {
    if (!scopes[i]) continue;
    for (const LongEntry& le : scopes[i]->longs) {
      size_t p = le.en.find(f);
      if (p == std::string::npos) continue;
      bool startOk = p == 0 || le.en[p - 1] == ' ' || le.en.compare(p >= 3 ? p - 3 : 0, 3, "|n|") == 0;
      if (!startOk) continue;
      if (!best || (p == 0 && pos != 0)) { best = &le; pos = p; }
      if (pos == 0) break;
    }
  }
  if (!best) return false;
  size_t endPos = pos + f.size();
  // paragraph that holds the fragment
  size_t k = 0;
  while (k + 1 < best->enPara.size() && pos >= best->enPara[k + 1].first) ++k;
  auto [eb, ee] = best->enPara[k];
  auto [rb, re] = best->ruPara[k];
  if (ee <= eb) return false;
  size_t startB = (pos > eb && best->en[pos - 1] == ' ') ? pos - 1 : pos;
  double fa = (double)(startB - eb) / (ee - eb), fb = (double)(std::min(endPos, ee) - eb) / (ee - eb);
  size_t a = SnapBoundary(best->ru, rb, re, fa), b = SnapBoundary(best->ru, rb, re, fb);
  if (a > rb && a < re && best->ru[a] == ' ') ++a;
  while (a < b && !UtfBoundary(best->ru, a)) ++a;
  while (b > a && !UtfBoundary(best->ru, b)) --b;
  out = b > a ? best->ru.substr(a, b - a) : std::string();
  while (!out.empty() && out.back() == ' ') out.pop_back();
  t_last.entry = best;
  t_last.end = endPos;
  return true;
}

// ---------------------------------------------------------------- modules

struct ModInfo {
  std::string name;  // lower base name, e.g. "peddamageoverhaul"
  uintptr_t base = 0, end = 0;
  const Scope* scope = nullptr;
  std::unordered_set<std::string> logged;
  FILE* capture = nullptr;
};

// patched modules; append-only so readers never see a reallocation
std::array<ModInfo*, 256> g_mods{};
std::atomic<size_t> g_modCount{0};

ModInfo* FindMod(uintptr_t addr) {
  size_t n = g_modCount.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (addr >= g_mods[i]->base && addr < g_mods[i]->end) return g_mods[i];
  return nullptr;
}

const Scope* g_common = nullptr;

// translated strings live forever (CREATE_VAR_STRING may keep the pointer briefly)
std::deque<std::string> g_store;
struct MemoVal {
  const char* out;
  const struct LongEntry* entry;  // set when the text was a line of a word-wrapped entry
  size_t end;
};
std::unordered_map<std::string, MemoVal> g_memo;  // key: module + '\x01' + text
constexpr size_t kMemoMax = 200000;

bool LooksTranslatable(const char* s) {
  int letters = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    if (*p == 0xD0 || *p == 0xD1) return false;  // UTF-8 Cyrillic: already translated
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) ++letters;
  }
  return letters >= 2;
}

void Capture(ModInfo* m, const char* s) {
  if (!g_capture || !m || !LooksTranslatable(s)) return;
  if (strlen(s) > 4000) return;
  if (!m->logged.insert(s).second) return;
  if (!m->capture) {
    std::wstring dir = g_dir + L"\\untranslated";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring wn(m->name.begin(), m->name.end());
    m->capture = _wfopen((dir + L"\\" + wn + L".txt").c_str(), L"ab");
  }
  if (m->capture) {
    std::string e = Escape(s);
    fprintf(m->capture, "%s => \n", e.c_str());
    fflush(m->capture);
  }
}

const char* Translate(ModInfo* m, const char* s) {
  if (!s || !*s) return s;
  std::string key = (m ? m->name : std::string("?")) + '\x01' + s;
  AcquireSRWLockExclusive(&g_lock);
  auto it = g_memo.find(key);
  if (it != g_memo.end()) {
    const MemoVal& mv = it->second;
    if (mv.entry) { t_last.entry = mv.entry; t_last.end = mv.end; }
    else t_last.entry = nullptr;  // something else was drawn between the lines
    const char* r = mv.out;
    ReleaseSRWLockExclusive(&g_lock);
    return r ? r : s;
  }
  bool memoize = true;
  t_last_hit = false;
  const Scope* scopes[2] = {m ? m->scope : nullptr, g_common};
  const char* result = nullptr;
  for (const Scope* sc : scopes) {
    if (!sc) continue;
    auto e = sc->exact.find(s);
    if (e != sc->exact.end()) { result = e->second.c_str(); break; }
  }
  if (!result) {
    std::string text = s, out;
    bool changed = LookupWhole(scopes, 2, text, out);
    if (!changed) {
      // "Label: value" built at runtime: translate both sides on their own
      size_t c = text.find(": ");
      if (c != std::string::npos && c > 0) {
        std::string left = text.substr(0, c), right = text.substr(c + 2), l2, r2;
        bool lt = LookupWhole(scopes, 2, left, l2), rt = LookupWhole(scopes, 2, right, r2);
        if (lt || rt) {
          out = (lt ? l2 : left) + ": " + (rt ? r2 : right);
          changed = true;
        }
      }
    }
    if (!changed) {
      bool contextual = false;
      changed = FragmentLookup(scopes, 2, text, out, contextual);
      if (changed) {
        t_last_hit = true;
        // a short piece found only through the previous line may belong to another
        // text elsewhere: do not cache it
        if (contextual && (text.size() < 12 || text.find(' ') == std::string::npos)) memoize = false;
      }
    }
    if (!changed) out = text;
    if (ApplySubs(scopes, 2, out)) changed = true;
    if (changed) {
      g_store.push_back(std::move(out));
      result = g_store.back().c_str();
    }
  }
  if (!t_last_hit) t_last.entry = nullptr;
  if (!result) Capture(m, s);
  if (memoize && g_memo.size() < kMemoMax)
    g_memo.emplace(std::move(key), MemoVal{result, t_last_hit ? t_last.entry : nullptr, t_last.end});
  ReleaseSRWLockExclusive(&g_lock);
  return result ? result : s;
}

bool ReadableCString(const void* p) {
  if ((uintptr_t)p < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
  const char* end = (const char*)mbi.BaseAddress + mbi.RegionSize;
  for (const char* c = (const char*)p; c < end; ++c)
    if (*c == '\0') return true;
  return false;
}

// ---------------------------------------------------------------- hideout memory

bool g_hmEnabled = true;
float g_hmRadius = 150.0f;
std::string g_hmModule = "bandit_hideouts";

struct HmBlip {
  int blip;
  float x, y, z;
  bool skull;
  bool shown;
};
std::vector<HmBlip> g_hmBlips;                  // touched only on the game thread (script fibers)
std::vector<std::array<float, 3>> g_hmVisited;  // hideouts the player has been near
int g_hmLogged = 0;
bool g_hmDiag = false;
std::unordered_set<uint64_t> g_hmSeen;

float ArgFloat(uint64_t v) {
  float f;
  uint32_t lo = (uint32_t)v;
  memcpy(&f, &lo, 4);
  return f;
}

void HmTrack(int blip, float x, float y, float z, uint32_t hash) {
  if (!blip) return;
  if (g_hmLogged < 40) {
    ++g_hmLogged;
    Log("hideout memory: blip %d at %.1f %.1f %.1f, hash 0x%08X", blip, x, y, z, hash);
  }
  for (auto& b : g_hmBlips)
    if (b.blip == blip) {
      b = HmBlip{blip, x, y, z, hash == kSpriteHideout, false};
      return;
    }
  g_hmBlips.push_back(HmBlip{blip, x, y, z, hash == kSpriteHideout, false});
}

void HmSprite(int blip, uint32_t sprite) {
  for (auto& b : g_hmBlips)
    if (b.blip == blip) {
      if (g_hmLogged < 40) {
        ++g_hmLogged;
        Log("hideout memory: blip %d sprite 0x%08X", blip, sprite);
      }
      if (sprite == kSpriteHideout) b.skull = true;
      b.shown = false;  // a new sprite may reset modifiers: settle on the next tick
      return;
    }
}

std::wstring HmFile() { return g_dir + L"\\HideoutMemory.dat"; }

void HmLoad() {
  g_hmVisited.clear();
  FILE* f = _wfopen(HmFile().c_str(), L"r");
  if (!f) return;
  float x, y, z;
  while (fscanf(f, "%f %f %f", &x, &y, &z) == 3) g_hmVisited.push_back({x, y, z});
  fclose(f);
}

void HmSave() {
  std::wstring tmp = HmFile() + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"w");
  if (!f) return;
  for (auto& v : g_hmVisited) fprintf(f, "%.1f %.1f %.1f\n", v[0], v[1], v[2]);
  fclose(f);
  MoveFileExW(tmp.c_str(), HmFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool HmVisited(const HmBlip& b) {
  for (auto& v : g_hmVisited) {
    float dx = v[0] - b.x, dy = v[1] - b.y;
    if (dx * dx + dy * dy < 25.0f) return true;  // same hideout: within 5 m
  }
  return false;
}

template <typename R = uint64_t, typename... A>
R Native(uint64_t hash, A... args) {
  g_origInit(hash);
  (g_origPush([](auto v) {
     uint64_t r = 0;
     memcpy(&r, &v, sizeof(v));
     return r;
   }(args)),
   ...);
  uint64_t* res = g_origCall();
  R out{};
  memcpy(&out, res, sizeof(R));
  return out;
}

void HmTick() {
  if (!g_hmEnabled || g_hmBlips.empty()) return;
  int ped = Native<int>(kPlayerPedId);
  g_origInit(kGetEntityCoords);
  g_origPush((uint64_t)(uint32_t)ped);
  g_origPush(1);
  g_origPush(1);
  float v[6];
  memcpy(v, g_origCall(), sizeof v);  // x, pad, y, pad, z, pad
  float px = v[0], py = v[2];
  bool saved = false;
  int skulls = 0, hidden = 0;
  for (size_t i = 0; i < g_hmBlips.size();) {
    HmBlip& b = g_hmBlips[i];
    if (!Native<int>(kDoesBlipExist, b.blip)) {
      g_hmBlips.erase(g_hmBlips.begin() + i);
      continue;
    }
    ++i;
    if (!b.skull) continue;
    ++skulls;
    bool visited = HmVisited(b);
    float dx = px - b.x, dy = py - b.y;
    if (!visited && dx * dx + dy * dy < g_hmRadius * g_hmRadius) {
      g_hmVisited.push_back({b.x, b.y, b.z});
      saved = true;
      visited = true;
      Log("hideout memory: visited the hideout at %.1f %.1f %.1f", b.x, b.y, b.z);
    }
    if (visited) {
      if (!b.shown) {
        Native<int>(kBlipRemoveModifier, b.blip, kModifierHidden);
        b.shown = true;
      }
    } else {
      // re-applied every tick: Bandit Hideouts may restyle its blip and drop modifiers
      Native<int>(kBlipRemoveModifier, b.blip, kModifierHidden);
      Native<int>(kBlipAddModifier, b.blip, kModifierHidden);
      b.shown = false;
      ++hidden;
    }
  }
  if (saved) HmSave();
  static int lastSkulls = -1, lastHidden = -1;
  if (skulls != lastSkulls || hidden != lastHidden) {
    Log("hideout memory: %d hideout skulls, %d hidden, %d visited in total", skulls, hidden, (int)g_hmVisited.size());
    lastSkulls = skulls;
    lastHidden = hidden;
  }
}

// ---------------------------------------------------------------- hooks

struct CallState {
  uint64_t hash = 0;
  int index = 0;
  bool literal = false;
  bool active = false;
  ModInfo* mod = nullptr;
  int blip = 0;  // 1 = BLIP_ADD_FOR_COORDS / _RADIUS, 2 = SET_BLIP_SPRITE from the hideout module
  uint64_t args[4] = {};
};
thread_local CallState t_call;

void HookInit(uint64_t hash) {
  t_call.hash = hash;
  t_call.index = 0;
  t_call.literal = false;
  t_call.active = (hash == kCreateVarString || hash == kDatabindingAddDataString);
  t_call.blip = g_hmEnabled ? ((hash == kBlipAddForCoords || hash == kBlipAddForRadius) ? 1
                                : hash == kSetBlipSprite                               ? 2
                                                                                       : 0)
                            : 0;
  t_call.mod = (t_call.active || t_call.blip) ? FindMod((uintptr_t)__builtin_return_address(0)) : nullptr;
  if (t_call.blip && (!t_call.mod || t_call.mod->name != g_hmModule)) t_call.blip = 0;
  if (g_hmDiag) {
    ModInfo* m = FindMod((uintptr_t)__builtin_return_address(0));
    if (m && m->name == g_hmModule && g_hmSeen.size() < 600 && g_hmSeen.insert(hash).second)
      Log("diag: %s calls 0x%016llX", m->name.c_str(), (unsigned long long)hash);
  }
  g_origInit(hash);
}

void HookPush(uint64_t value) {
  if (t_call.active && g_enabled) {
    int i = t_call.index;
    if (t_call.hash == kCreateVarString) {
      if (i == 1) {
        const char* label = (const char*)value;
        t_call.literal = label && strcmp(label, "LITERAL_STRING") == 0;
      } else if (i == 2 && t_call.literal) {
        value = (uint64_t)Translate(t_call.mod, (const char*)value);
      }
    } else if (t_call.hash == kDatabindingAddDataString && i == 2) {
      if (ReadableCString((const void*)value)) value = (uint64_t)Translate(t_call.mod, (const char*)value);
    }
  }
  if (t_call.blip && t_call.index < 4) t_call.args[t_call.index] = value;
  ++t_call.index;
  g_origPush(value);
}

uint64_t* HookCall() {
  t_call.active = false;
  uint64_t* r = g_origCall();
  if (t_call.blip == 1 && r) {
    int blip = 0;
    memcpy(&blip, r, sizeof blip);
    HmTrack(blip, ArgFloat(t_call.args[1]), ArgFloat(t_call.args[2]), ArgFloat(t_call.args[3]),
            (uint32_t)t_call.args[0]);
  } else if (t_call.blip == 2) {
    HmSprite((int)(uint32_t)t_call.args[0], (uint32_t)t_call.args[1]);
  }
  t_call.blip = 0;
  return r;
}

// ---------------------------------------------------------------- import patching

bool PatchModule(HMODULE mod) {
  auto base = (uint8_t*)mod;
  auto dos = (IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  int patched = 0;
  for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp((const char*)(base + d->Name), "ScriptHookRDR2.dll") != 0) continue;
    if (!d->OriginalFirstThunk) continue;
    auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
    auto slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
      const char* nm = (const char*)ibn->Name;
      uint64_t hook = 0;
      if (!strcmp(nm, kExpInit)) hook = (uint64_t)&HookInit;
      else if (!strcmp(nm, kExpPush)) hook = (uint64_t)&HookPush;
      else if (!strcmp(nm, kExpCall)) hook = (uint64_t)&HookCall;
      if (!hook || slots->u1.Function == hook) continue;
      DWORD old;
      if (VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) {
        slots->u1.Function = hook;
        VirtualProtect(&slots->u1.Function, 8, old, &old);
        ++patched;
      }
    }
  }
  return patched > 0;
}

SRWLOCK g_patchLock = SRWLOCK_INIT;

void PatchAll() {
  AcquireSRWLockExclusive(&g_patchLock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_patchLock); }
  } unlock;
  HMODULE mods[1024];
  DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
  size_t n = std::min<size_t>(need / sizeof(HMODULE), 1024);
  for (size_t i = 0; i < n; ++i) {
    HMODULE m = mods[i];
    if (m == g_self || m == g_sh) continue;
    if (FindMod((uintptr_t)m)) continue;
    if (g_modCount.load() >= g_mods.size()) return;
    // only modules that import ScriptHookRDR2.dll
    if (!PatchModule(m)) continue;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring wp = path;
    std::wstring file = wp.substr(wp.find_last_of(L"\\/") + 1);
    std::string base = Lower(Narrow(file.substr(0, file.find_last_of(L'.'))));
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info);
    auto mi = new ModInfo;
    mi->name = base;
    mi->base = (uintptr_t)info.lpBaseOfDll;
    mi->end = mi->base + info.SizeOfImage;
    auto sc = g_dict->find(base);
    mi->scope = sc == g_dict->end() ? nullptr : &sc->second;
    g_mods[g_modCount.load()] = mi;
    g_modCount.fetch_add(1, std::memory_order_release);
    Log("patched %s (dictionary: %s)", Narrow(file).c_str(), mi->scope ? "yes" : "no");
  }
}

// Runs as a ScriptHook script: between other scripts' ticks, so no mod is in the
// middle of a nativeInit/nativePush64/nativeCall sequence while imports change.
void ReadIni();

// Size+time signature of RuText\*.txt and RuText.ini, to notice edits.
uint64_t DictSignature() {
  uint64_t sig = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_dir + L"\\*.*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring n = fd.cFileName;
    if (n.size() < 4) continue;
    std::wstring ext = n.substr(n.size() - 4);
    if (_wcsicmp(ext.c_str(), L".txt") && _wcsicmp(ext.c_str(), L".ini")) continue;
    sig = sig * 1099511628211ULL + fd.nFileSizeLow + fd.ftLastWriteTime.dwLowDateTime;
    for (wchar_t c : n) sig = sig * 31 + c;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return sig;
}

void ReloadDictionaries() {
  AcquireSRWLockExclusive(&g_lock);
  g_oldDicts.push_back(g_dict);
  g_dict = new ScopeMap;
  ReadIni();
  LoadDictionaries();
  auto c = g_dict->find("_common");
  g_common = c == g_dict->end() ? nullptr : &c->second;
  size_t n = g_modCount.load();
  for (size_t i = 0; i < n; ++i) {
    auto sc = g_dict->find(g_mods[i]->name);
    g_mods[i]->scope = sc == g_dict->end() ? nullptr : &sc->second;
  }
  g_memo.clear();
  ReleaseSRWLockExclusive(&g_lock);
  Log("dictionaries reloaded (Enabled=%d Capture=%d)", g_enabled, g_capture);
}

void ScriptMain() {
  Log("script started");
  uint64_t sig = DictSignature();
  for (int tick = 0;; ++tick) {
    if (tick % 3 == 0) PatchAll();
    uint64_t now = DictSignature();
    if (now != sig) {
      sig = now;
      ReloadDictionaries();
    }
    HmTick();
    g_scriptWait(2000);
  }
}

void ReadIni() {
  std::wstring ini = g_dir + L"\\RuText.ini";
  g_enabled = GetPrivateProfileIntW(L"RuText", L"Enabled", 1, ini.c_str()) != 0;
  g_capture = GetPrivateProfileIntW(L"RuText", L"Capture", 1, ini.c_str()) != 0;
  g_hmEnabled = GetPrivateProfileIntW(L"HideoutMemory", L"Enabled", 1, ini.c_str()) != 0;
  g_hmRadius = (float)GetPrivateProfileIntW(L"HideoutMemory", L"Radius", 150, ini.c_str());
  g_hmDiag = GetPrivateProfileIntW(L"HideoutMemory", L"Diag", 0, ini.c_str()) != 0;
}

bool Register() {
  g_origInit = (FnInit)GetProcAddress(g_sh, kExpInit);
  g_origPush = (FnPush)GetProcAddress(g_sh, kExpPush);
  g_origCall = (FnCall)GetProcAddress(g_sh, kExpCall);
  auto reg = (FnScriptRegister)GetProcAddress(g_sh, kExpScriptRegister);
  g_scriptWait = (FnScriptWait)GetProcAddress(g_sh, kExpScriptWait);
  if (!g_origInit || !g_origPush || !g_origCall || !reg || !g_scriptWait) {
    Log("ScriptHookRDR2 exports not found, RuText inactive");
    return false;
  }
  reg(g_self, ScriptMain);
  Log("registered with ScriptHookRDR2");
  return true;
}

// Patch the mods that are already loaded right away, long before any script runs:
// some create their map blips in their very first tick (Bandit Hideouts), before
// RuText's own script gets its first turn. Later modules are picked up by ScriptMain.
DWORD WINAPI EarlyPatch(LPVOID) {
  PatchAll();
  Log("early patch done (%d modules)", (int)g_modCount.load());
  return 0;
}

// Fallback when RuText loads before any module that imports ScriptHookRDR2.dll.
DWORD WINAPI Boot(LPVOID) {
  for (int i = 0; i < 600 && !(g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll")); ++i) Sleep(100);
  if (!g_sh) {
    Log("ScriptHookRDR2.dll not loaded, RuText inactive");
    return 0;
  }
  if (Register()) EarlyPatch(nullptr);
  return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_self = inst;
  DisableThreadLibraryCalls(inst);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(inst, path, MAX_PATH);
  std::wstring p = path;
  g_dir = p.substr(0, p.find_last_of(L"\\/")) + L"\\RuText";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  g_log = _wfopen((g_dir + L"\\RuText.log").c_str(), L"w");
  Log("RuText 1.4.0 loaded");
  ReadIni();
  Log("Enabled=%d Capture=%d", g_enabled, g_capture);
  HmLoad();
  Log("hideout memory: Enabled=%d Radius=%.0f, %d hideouts visited before", g_hmEnabled, g_hmRadius,
      (int)g_hmVisited.size());
  LoadDictionaries();
  auto c = g_dict->find("_common");
  g_common = c == g_dict->end() ? nullptr : &c->second;
  // ScriptHookRDR2.dll is normally loaded already: earlier .asi files import it.
  if ((g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll"))) {
    if (Register()) {
      HANDLE t = CreateThread(nullptr, 0, EarlyPatch, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
  } else {
    HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
