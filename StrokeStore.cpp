// The C file functions used below (fopen and friends) are fine to use here: every result is checked. Microsoft's compiler flags
// them as "unsafe" and, in an Xbox app, treats that as an error, so the warning is switched off before any header is read.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "StrokeStore.h"

#include "ImageUtil.h"
#include "Json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace XboxOTFM {

namespace {

constexpr uint32_t kFileVersion = 1;
constexpr uint32_t kMaxHeaderBytes = 8 * 1024;
constexpr size_t kMaxNameBytes = 80;
constexpr char kExt[] = ".otcd";
constexpr size_t kExtLen = sizeof(kExt) - 1;

void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void Fail(std::string* error, const std::string& text) {
    if (error) *error = text;
}

uint64_t NowUnix() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string CleanName(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) continue;
        out += static_cast<char>(c);
        if (out.size() >= kMaxNameBytes) break;
    }
    // never leave half of a multi-byte character at the end (a complete last character stays)
    size_t i = out.size();
    size_t continuation = 0;
    while (i > 0 && (static_cast<unsigned char>(out[i - 1]) & 0xC0) == 0x80 && continuation < 3) {
        --i;
        ++continuation;
    }
    if (i > 0) {
        const unsigned char lead = static_cast<unsigned char>(out[i - 1]);
        const size_t need = lead >= 0xF0 ? 3 : (lead >= 0xE0 ? 2 : (lead >= 0xC0 ? 1 : 0));
        if (lead >= 0xC0 && continuation < need) out.resize(i - 1);
    }
    return out;
}

std::string HashHex(uint64_t h) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool ParseHashHex(const std::string& s, uint64_t& out) {
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

std::string HeaderJson(const CardInfo& c) {
    JsonWriter w;
    w.BeginObject();
    w.Field("v", kFileVersion);
    w.Field("name", c.name);
    w.Field("preset", c.preset);
    w.Field("hash", HashHex(c.key.pictureHash));
    w.Field("strokes", c.key.strokes);
    w.Field("fit", c.key.fit);
    w.Field("seed", c.key.seed);
    w.Field("k", c.key.candidates);
    w.Field("r", c.key.rounds);
    w.Field("e", c.key.engine);
    w.Key("flights").BeginArray().Value(c.perFlight[0]).Value(c.perFlight[1]).Value(c.perFlight[2]).EndArray();
    w.Field("empty", c.emptyStrokes);
    w.Field("err", c.errorRms);
    w.Field("sec", c.seconds);
    w.Field("sps", c.strokesPerSecond);
    w.Field("group", c.groupSize);
    w.Field("created", c.createdUnix);
    w.EndObject();
    return w.Take();
}

bool ParseHeaderJson(const std::string& text, CardInfo& out, std::string* error) {
    JsonValue v;
    std::string err;
    if (!JsonParser::Parse(text, v, &err) || !v.IsObject()) {
        Fail(error, "The card's description could not be read.");
        return false;
    }
    CardInfo c;
    double n = 0.0;
    std::string s;
    auto num = [&](const char* key, double fallback) { return v.GetNumber(key, n) ? n : fallback; };
    if (!v.GetString("hash", s) || !ParseHashHex(s, c.key.pictureHash)) {
        Fail(error, "The card does not say which picture it belongs to.");
        return false;
    }
    const double strokes = num("strokes", 0), fit = num("fit", 0);
    if (!(strokes >= 3 && strokes <= StrokeStore::kMaxStrokes) || !(fit >= 16 && fit <= 256)) {
        Fail(error, "The card's size numbers are out of range.");
        return false;
    }
    c.key.strokes = static_cast<uint32_t>(strokes);
    c.key.fit = static_cast<uint32_t>(fit);
    c.key.seed = static_cast<uint32_t>(std::clamp(num("seed", 1), 0.0, 4294967295.0));
    c.key.candidates = static_cast<uint32_t>(std::clamp(num("k", 64), 0.0, 100000.0));
    c.key.rounds = static_cast<uint32_t>(std::clamp(num("r", 3), 0.0, 100000.0));
    c.key.engine = static_cast<uint32_t>(std::clamp(num("e", 0), 0.0, 4294967295.0));
    c.preset = static_cast<uint32_t>(std::clamp(num("preset", 0), 0.0, 99.0));
    c.emptyStrokes = static_cast<uint32_t>(std::clamp(num("empty", 0), 0.0, 4294967295.0));
    c.errorRms = num("err", 0.0);
    c.seconds = num("sec", 0.0);
    c.strokesPerSecond = num("sps", 0.0);
    c.groupSize = static_cast<uint32_t>(std::clamp(num("group", 1), 1.0, 100000.0));
    c.createdUnix = static_cast<uint64_t>(std::clamp(num("created", 0), 0.0, 1e12));
    if (const JsonValue* fl = v.Find("flights")) {
        if (fl->type == JsonValue::Type::Array) {
            for (size_t i = 0; i < 3 && i < fl->items.size(); ++i) {
                if (fl->items[i].type == JsonValue::Type::Number && std::isfinite(fl->items[i].number) && fl->items[i].number >= 0 && fl->items[i].number < 4e9) {
                    c.perFlight[i] = static_cast<uint32_t>(fl->items[i].number);
                }
            }
        }
    }
    v.GetString("name", c.name);
    c.name = CleanName(c.name);
    out = std::move(c);
    return true;
}

bool WriteFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            // some systems refuse to replace a file that is there already: remove it and try once more
            std::error_code ec2;
            std::filesystem::remove(path, ec2);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
        ok = !ec;
    }
    if (!ok) {
        std::error_code ec3;
        std::filesystem::remove(tmp, ec3);
    }
    return ok;
}

} // namespace

std::string CardId(const CardKey& k) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "h%016llx-n%u-f%u-s%u-k%u-r%u-e%u", static_cast<unsigned long long>(k.pictureHash), k.strokes, k.fit, k.seed,
                  k.candidates, k.rounds, k.engine);
    return buf;
}

bool IsSafeCardId(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    for (unsigned char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// File format
// ---------------------------------------------------------------------------------------------
std::vector<uint8_t> StrokeStore::Serialize(const CardInfo& info, const uint8_t* dials, size_t dialCount) {
    const std::string header = HeaderJson(info);
    std::vector<uint8_t> out;
    out.reserve(12 + header.size() + 8 + dialCount);
    out.push_back('O'); out.push_back('T'); out.push_back('C'); out.push_back('D');
    Put32(out, kFileVersion);
    Put32(out, static_cast<uint32_t>(header.size()));
    out.insert(out.end(), header.begin(), header.end());
    Put32(out, static_cast<uint32_t>(dialCount));
    if (dialCount > 0) out.insert(out.end(), dials, dials + dialCount);
    Put32(out, Crc32(out.data(), out.size()));
    return out;
}

bool StrokeStore::Parse(const uint8_t* bytes, size_t size, CardInfo* info, std::vector<uint8_t>* dials, std::string* error) {
    if (size < 12 + 4 + 4 || std::memcmp(bytes, "OTCD", 4) != 0) {
        Fail(error, "This is not a card file.");
        return false;
    }
    if (Get32(bytes + 4) != kFileVersion) {
        Fail(error, "This card was made by a different version of the app.");
        return false;
    }
    const uint32_t headerBytes = Get32(bytes + 8);
    if (headerBytes > kMaxHeaderBytes || 12ull + headerBytes + 4 + 4 > size) {
        Fail(error, "The card file is damaged (its description is cut off).");
        return false;
    }
    if (Crc32(bytes, size - 4) != Get32(bytes + size - 4)) {
        Fail(error, "The card file is damaged (its checksum does not match).");
        return false;
    }
    CardInfo c;
    if (!ParseHeaderJson(std::string(reinterpret_cast<const char*>(bytes + 12), headerBytes), c, error)) return false;
    const uint32_t dialCount = Get32(bytes + 12 + headerBytes);
    const uint64_t expected = 3ull + 12ull * c.key.strokes;
    if (dialCount != expected || 12ull + headerBytes + 4 + dialCount + 4 != size) {
        Fail(error, "The card file is damaged (its size does not match its stroke count).");
        return false;
    }
    const uint8_t* d = bytes + 12 + headerBytes + 4;
    for (uint32_t i = 0; i < dialCount; ++i) {
        if (d[i] > 127) {
            Fail(error, "The card file is damaged (a dial is out of range).");
            return false;
        }
    }
    c.id = CardId(c.key);
    c.fileBytes = size;
    if (dials) dials->assign(d, d + dialCount);
    if (info) *info = std::move(c);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The folder
// ---------------------------------------------------------------------------------------------
StrokeStore::StrokeStore(std::string folder) : m_folder(std::move(folder)) {}

std::string StrokeStore::PathFor(const std::string& id) const {
    return m_folder + "/" + id + kExt;
}

bool StrokeStore::ReadHeader(const std::string& path, CardInfo& info) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        uint8_t head[12];
        if (std::fread(head, 1, sizeof(head), f) != sizeof(head)) break;
        if (std::memcmp(head, "OTCD", 4) != 0 || Get32(head + 4) != kFileVersion) break;
        const uint32_t headerBytes = Get32(head + 8);
        if (headerBytes > kMaxHeaderBytes) break;
        std::string text(headerBytes, '\0');
        if (headerBytes > 0 && std::fread(&text[0], 1, headerBytes, f) != headerBytes) break;
        uint8_t cnt[4];
        if (std::fread(cnt, 1, sizeof(cnt), f) != sizeof(cnt)) break;
        CardInfo c;
        if (!ParseHeaderJson(text, c, nullptr)) break;
        std::error_code ec;
        const uint64_t sz = std::filesystem::file_size(path, ec);
        if (ec) break;
        const uint32_t dialCount = Get32(cnt);
        // the file must have exactly the size its header promises (a cut-off file is ignored)
        if (dialCount != 3ull + 12ull * c.key.strokes || sz != 12ull + headerBytes + 4 + dialCount + 4) break;
        c.id = CardId(c.key);
        c.fileBytes = sz;
        info = std::move(c);
        ok = true;
    } while (false);
    std::fclose(f);
    return ok;
}

bool StrokeStore::Init(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_index.clear();
    m_totalBytes = 0;
    m_persistent = false;
    ++m_version;
    std::error_code ec;
    std::filesystem::create_directories(m_folder, ec);
    if (ec) {
        Fail(error, "The folder for the cards could not be created: " + ec.message());
        return false;
    }
    std::vector<std::filesystem::path> temps;
    for (std::filesystem::directory_iterator it(m_folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            temps.push_back(it->path());
            continue;
        }
        if (name.size() <= kExtLen || name.compare(name.size() - kExtLen, kExtLen, kExt) != 0) continue;
        const std::string stem = name.substr(0, name.size() - kExtLen);
        if (!IsSafeCardId(stem)) continue;
        CardInfo info;
        if (!ReadHeader(m_folder + "/" + name, info)) continue;
        if (info.id != stem) continue; // the file name does not match what is inside: not one of ours
        m_totalBytes += info.fileBytes;
        m_index[info.id] = std::move(info);
    }
    for (const auto& p : temps) {
        std::error_code ec2;
        std::filesystem::remove(p, ec2);
    }
    m_persistent = true;
    return true;
}

bool StrokeStore::Persistent() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_persistent;
}

bool StrokeStore::Has(const CardKey& key) const {
    const std::string id = CardId(key);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.find(id) != m_index.end();
}

bool StrokeStore::Find(const CardKey& key, CardInfo& info) const {
    return Get(CardId(key), info);
}

bool StrokeStore::Get(const std::string& id, CardInfo& info) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_index.find(id);
    if (it == m_index.end()) return false;
    info = it->second;
    return true;
}

bool StrokeStore::Save(CardInfo& info, const uint8_t* dials, size_t dialCount, std::string* error) {
    if (info.key.strokes < 3 || info.key.strokes > kMaxStrokes || info.key.fit < 16 || info.key.fit > 256) {
        Fail(error, "The card's size numbers are out of range.");
        return false;
    }
    if (dialCount != 3ull + 12ull * info.key.strokes || !dials) {
        Fail(error, "The card does not have the number of dials its stroke count needs.");
        return false;
    }
    for (size_t i = 0; i < dialCount; ++i) {
        if (dials[i] > 127) {
            Fail(error, "The card has a dial outside 0..127, so it was not saved.");
            return false;
        }
    }
    info.id = CardId(info.key);
    if (info.createdUnix == 0) info.createdUnix = NowUnix();
    info.name = CleanName(info.name);
    if (info.groupSize < 1) info.groupSize = 1;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_persistent) {
        Fail(error, "The folder for the cards is not available, so nothing can be saved.");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_folder, ec);
    const std::vector<uint8_t> bytes = Serialize(info, dials, dialCount);
    if (!WriteFileAtomically(PathFor(info.id), bytes)) {
        Fail(error, "The card could not be written (is the storage full?).");
        return false;
    }
    info.fileBytes = bytes.size();
    const auto it = m_index.find(info.id);
    if (it != m_index.end()) m_totalBytes -= std::min<uint64_t>(m_totalBytes, it->second.fileBytes);
    m_index[info.id] = info;
    m_totalBytes += info.fileBytes;
    ++m_version;
    return true;
}

bool StrokeStore::Load(const std::string& id, CardInfo* info, std::vector<uint8_t>* dials, std::string* error) const {
    if (!IsSafeCardId(id)) {
        Fail(error, "Unknown card.");
        return false;
    }
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        FILE* f = std::fopen(PathFor(id).c_str(), "rb");
        if (!f) {
            Fail(error, "That card no longer exists.");
            return false;
        }
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(PathFor(id), ec);
        bool ok = !ec && size < (64ull << 20);
        if (ok) {
            bytes.resize(static_cast<size_t>(size));
            ok = size == 0 || std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        }
        std::fclose(f);
        if (!ok) {
            Fail(error, "The card file could not be read.");
            return false;
        }
    }
    CardInfo parsed;
    if (!Parse(bytes.data(), bytes.size(), &parsed, dials, error)) return false;
    if (parsed.id != id) {
        Fail(error, "The card file does not match its name.");
        if (dials) dials->clear();
        return false;
    }
    if (info) *info = std::move(parsed);
    return true;
}

std::vector<CardInfo> StrokeStore::List() const {
    std::vector<CardInfo> out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.reserve(m_index.size());
        for (const auto& kv : m_index) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const CardInfo& a, const CardInfo& b) {
        if (a.createdUnix != b.createdUnix) return a.createdUnix > b.createdUnix;
        return a.id > b.id;
    });
    return out;
}

size_t StrokeStore::Count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.size();
}

uint64_t StrokeStore::TotalBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalBytes;
}

uint64_t StrokeStore::Version() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_version;
}

bool StrokeStore::Delete(const std::string& id) {
    if (!IsSafeCardId(id)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_index.find(id);
    if (it == m_index.end()) return false;
    std::error_code ec;
    std::filesystem::remove(PathFor(id), ec);
    m_totalBytes -= std::min<uint64_t>(m_totalBytes, it->second.fileBytes);
    m_index.erase(it);
    ++m_version;
    return true;
}

size_t StrokeStore::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t n = m_index.size();
    for (const auto& kv : m_index) {
        std::error_code ec;
        std::filesystem::remove(PathFor(kv.first), ec);
    }
    m_index.clear();
    m_totalBytes = 0;
    ++m_version;
    return n;
}

} // namespace XboxOTFM
