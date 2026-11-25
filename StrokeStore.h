#pragma once
// The saved cards (the converted pictures). One small file per card, written atomically (temp file + rename), so a power cut or a crash
// can never leave a half-written card, and a card that is already there is never touched by a new run.
//
// A card is found by its KEY: the fingerprint of the original picture plus every setting that changes what the converter makes (number of
// strokes, working size, seed, candidates, tuning rounds and the version of the converter's maths). The same key always gives the same
// card, so "skip pictures that are already converted" is just a look-up. A card made with other settings, or by an older converter, has a
// different key and is simply not reused.
//
// File layout (little-endian):
//   "OTCD" | u32 version (1) | u32 headerBytes | header JSON (UTF-8) | u32 dialCount | dialCount bytes (each 0..127) | u32 CRC-32 of every byte before it
//
// No Windows code in here, so everything is tested natively.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace XboxOTFM {

// Raise this whenever the converter's maths change (Strokes.hlsl or Converter.cpp): cards made by the old maths are then left alone and
// the picture is converted again.
constexpr uint32_t kCardEngineVersion = 1;

struct CardKey {
    uint64_t pictureHash = 0;   // fingerprint of the original picture's pixels (Dataset's hash)
    uint32_t strokes = 0;
    uint32_t fit = 0;           // the size the converter worked at
    uint32_t seed = 1;
    uint32_t candidates = 64;
    uint32_t rounds = 3;
    uint32_t engine = kCardEngineVersion;
    bool operator==(const CardKey& o) const {
        return pictureHash == o.pictureHash && strokes == o.strokes && fit == o.fit && seed == o.seed && candidates == o.candidates &&
               rounds == o.rounds && engine == o.engine;
    }
};

// The file name (without extension) of a card: only lower-case letters, digits and '-'.
std::string CardId(const CardKey& key);
bool IsSafeCardId(const std::string& id);

struct CardInfo {
    std::string id;                  // filled in by Save
    CardKey key;
    std::string name;                // the picture's name when it was converted
    uint32_t preset = 0;             // 0 Low, 1 Medium, 2 High, 3 Ultra (only for showing; the key decides what is reused)
    uint32_t perFlight[3] = { 0, 0, 0 };
    uint32_t emptyStrokes = 0;       // strokes that could not lower the error (stored empty)
    double errorRms = 0.0;           // leftover error against the working-size picture, 0..255 scale
    double seconds = 0.0;            // how long the group this picture was in took
    double strokesPerSecond = 0.0;   // speed of that group, all its pictures together
    uint32_t groupSize = 1;
    uint64_t createdUnix = 0;        // filled in by Save when 0
    uint64_t fileBytes = 0;          // filled in by Save / List
};

class StrokeStore {
public:
    static constexpr uint32_t kMaxStrokes = 200000;   // same limit as the converter

    explicit StrokeStore(std::string folder);

    // Creates the folder, removes temp files a power cut may have left, and reads the small header of every card (once).
    bool Init(std::string* error = nullptr);
    // True when the folder exists and can be written to.
    bool Persistent() const;

    bool Has(const CardKey& key) const;
    // Writes (or replaces) the card. info.id, info.fileBytes and (when 0) info.createdUnix are filled in. dialCount must be 3 + 12 * strokes
    // and every dial 0..127.
    bool Save(CardInfo& info, const uint8_t* dials, size_t dialCount, std::string* error = nullptr);
    // info and/or dials may be null. The checksum is verified.
    bool Load(const std::string& id, CardInfo* info, std::vector<uint8_t>* dials, std::string* error = nullptr) const;
    bool Find(const CardKey& key, CardInfo& info) const;
    bool Get(const std::string& id, CardInfo& info) const;      // from the list in memory (no file is read)

    std::vector<CardInfo> List() const;                          // newest first
    size_t Count() const;
    uint64_t TotalBytes() const;
    uint64_t Version() const;                                    // goes up with every change

    bool Delete(const std::string& id);
    size_t Clear();                                              // removes every card, returns how many

    // Whole-file encode / decode (exposed for the tests).
    static std::vector<uint8_t> Serialize(const CardInfo& info, const uint8_t* dials, size_t dialCount);
    static bool Parse(const uint8_t* bytes, size_t size, CardInfo* info, std::vector<uint8_t>* dials, std::string* error);

    const std::string& Folder() const { return m_folder; }

private:
    std::string PathFor(const std::string& id) const;
    static bool ReadHeader(const std::string& path, CardInfo& info);

    std::string m_folder;
    mutable std::mutex m_mutex;
    std::map<std::string, CardInfo> m_index;   // by id
    uint64_t m_version = 1;
    uint64_t m_totalBytes = 0;
    bool m_persistent = false;
};

} // namespace XboxOTFM
