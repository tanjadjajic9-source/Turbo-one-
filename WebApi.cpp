#include "WebApi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace XboxOTFM {

namespace {

const char* const kJson = "application/json; charset=utf-8";
const char* const kPng = "image/png";

bool ParseUnsignedText(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

uint64_t QueryUInt(const HttpRequest& req, const char* name, uint64_t fallback, uint64_t lo, uint64_t hi) {
    uint64_t v = 0;
    if (!req.HasQuery(name) || !ParseUnsignedText(req.Query(name), v)) return std::clamp(fallback, lo, hi);
    return std::clamp(v, lo, hi);
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// "/api/pictures/123.png" with prefix "/api/pictures/" and suffix ".png" -> 123
bool IdBetween(const std::string& path, const char* prefix, const char* suffix, uint64_t& id) {
    if (!StartsWith(path, prefix) || !EndsWith(path, suffix)) return false;
    const size_t a = std::strlen(prefix);
    const size_t b = std::strlen(suffix);
    if (path.size() <= a + b) return false;
    return ParseUnsignedText(path.substr(a, path.size() - a - b), id);
}

// "/api/cards/<id>/painted.png" -> id (when the id has only the characters a card id can have)
bool CardIdBetween(const std::string& path, const char* suffix, std::string& id) {
    const char* prefix = "/api/cards/";
    if (!StartsWith(path, prefix) || !EndsWith(path, suffix)) return false;
    const size_t a = std::strlen(prefix);
    const size_t b = std::strlen(suffix);
    if (path.size() <= a + b) return false;
    id = path.substr(a, path.size() - a - b);
    return IsSafeCardId(id);
}

std::string HashText(uint64_t hash) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>((hash ^ (hash >> 32)) & 0xFFFFFFFFu));
    return buf;
}

const char* AddStatusName(Dataset::AddStatus s) {
    switch (s) {
        case Dataset::AddStatus::Added: return "added";
        case Dataset::AddStatus::Duplicate: return "duplicate";
        case Dataset::AddStatus::Full: return "full";
        case Dataset::AddStatus::Invalid: return "invalid";
        default: return "diskError";
    }
}

std::vector<uint8_t> PngOf(const Rgb8Image& img) {
    return EncodePngRgb(img.px.data(), img.w, img.h);
}

} // namespace

WebApi::WebApi(AppCore& core, WebApiHooks hooks, const uint8_t* pageBytes, size_t pageSize)
    : m_core(core), m_hooks(std::move(hooks)), m_page(pageBytes), m_pageSize(pageSize) {
}

void WebApi::Ok(HttpResponse& resp, JsonWriter& w) {
    resp.SetText(200, kJson, w.Take());
}

void WebApi::Fail(HttpResponse& resp, int status, const std::string& message) {
    JsonWriter w;
    w.BeginObject().Field("ok", false).Field("error", message).EndObject();
    resp.SetText(status, kJson, w.Take());
}

bool WebApi::ParseBodyObject(const HttpRequest& req, JsonValue& out, std::string* error) {
    if (req.body.empty()) {
        out = JsonValue();
        out.type = JsonValue::Type::Object;
        return true;
    }
    const std::string text(req.body.begin(), req.body.end());
    std::string err;
    if (!JsonParser::Parse(text, out, &err) || !out.IsObject()) {
        if (error) *error = "The request was not understood (it should be a JSON object).";
        return false;
    }
    return true;
}

size_t WebApi::BodyLimit(const std::string& method, const std::string& path) const {
    if (method == "POST") {
        if (path == "/api/pictures") return kMaxRawPicturePixels * 3 + 1024;
        if (path == "/api/pictures/file") return kMaxPictureFileBytes;
        // up to 100,000 picture numbers (about 0.9 MB). The JSON reader takes at most 1 MB of text, so a longer list is refused
        // here with a clear "too large" instead of a confusing "not understood" later.
        if (path == "/api/pictures/delete" || path == "/api/convert/start" || path == "/api/cards/delete") return 1024 * 1024;
    }
    return 64 * 1024;
}

void WebApi::ServePage(HttpResponse& resp) const {
    resp.status = 200;
    resp.contentType = "text/html; charset=utf-8";
    resp.body.assign(m_page, m_page + m_pageSize);
    resp.cacheControl = "no-store";
    resp.csp = "default-src 'self'; img-src 'self' data: blob:; font-src data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
               "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'";
}

void WebApi::Handle(const HttpRequest& req, HttpResponse& resp) {
    const bool isRead = req.method == "GET" || req.method == "HEAD";
    const std::string& p = req.path;

    if (isRead && (p == "/" || p == "/index.html")) {
        ServePage(resp);
        return;
    }
    if (isRead && p == "/favicon.ico") {
        resp.status = 204;
        return;
    }
    if (!StartsWith(p, "/api/")) {
        Fail(resp, 404, "There is nothing at this address. Open the main page instead.");
        return;
    }
    // Everything that changes something needs this header; a web page on another site can't add it (browsers block that), so
    // other sites can't press the buttons on your Xbox.
    if (!isRead && req.Header("x-otfm") != "1") {
        Fail(resp, 403, "The safety header is missing. Reload the page and try again.");
        return;
    }

    uint64_t id = 0;
    std::string cardId;

    // ---- read-only state ----
    if (isRead) {
        if (p == "/api/health") {
            JsonWriter w;
            w.BeginObject().Field("ok", true).EndObject();
            Ok(resp, w);
        } else if (p == "/api/status") {
            ServeStatus(resp);
        } else if (p == "/api/toasts") {
            ServeToasts(req, resp);
        } else if (p == "/api/logs") {
            ServeLogs(req, resp);
        } else if (p == "/api/settings") {
            ServeSettings(req, resp);
        } else if (p == "/api/live") {
            ServeLive(req, false, resp);
        } else if (p == "/api/live.png") {
            ServeLive(req, true, resp);
        } else if (p == "/api/pictures") {
            ServePictures(req, resp);
        } else if (IdBetween(p, "/api/pictures/", ".png", id)) {
            ServePictureImage(req, id, resp);
        } else if (p == "/api/cards") {
            ServeCards(req, resp);
        } else if (CardIdBetween(p, "/painted.png", cardId)) {
            ServeCardImage(req, cardId, true, resp);
        } else if (CardIdBetween(p, "/original.png", cardId)) {
            ServeCardImage(req, cardId, false, resp);
        } else {
            Fail(resp, 404, "Unknown request.");
        }
        return;
    }

    // ---- changes ----
    if (req.method == "POST") {
        if (p == "/api/settings" || p == "/api/settings/reset") {
            ServeSettings(req, resp);
        } else if (StartsWith(p, "/api/convert/")) {
            ServeConvert(req, p.substr(13), resp);
        } else if (p == "/api/selftest") {
            ServeSelfTest(resp);
        } else if (p == "/api/pictures") {
            ServePictureUpload(req, resp);
        } else if (p == "/api/pictures/file") {
            ServePictureFileUpload(req, resp);
        } else if (p == "/api/pictures/delete" || p == "/api/pictures/clear") {
            ServePictureDelete(req, resp);
        } else if (p == "/api/cards/delete" || p == "/api/cards/clear") {
            ServeCardDelete(req, resp);
        } else {
            Fail(resp, 404, "Unknown request.");
        }
        return;
    }
    if (req.method == "DELETE") {
        if (StartsWith(p, "/api/pictures/") && ParseUnsignedText(p.substr(14), id)) {
            const bool removed = m_core.Pictures().Remove(id);
            JsonWriter w;
            w.BeginObject().Field("ok", true).Field("removed", removed).Field("total", static_cast<uint64_t>(m_core.Pictures().Size())).EndObject();
            Ok(resp, w);
        } else if (StartsWith(p, "/api/cards/") && IsSafeCardId(p.substr(11))) {
            const bool removed = m_core.DeleteCards({ p.substr(11) }) == 1;
            JsonWriter w;
            w.BeginObject().Field("ok", true).Field("removed", removed).Field("total", static_cast<uint64_t>(m_core.Cards().Count())).EndObject();
            Ok(resp, w);
        } else {
            Fail(resp, 404, "Unknown request.");
        }
        return;
    }
    Fail(resp, 405, "That kind of request is not supported here.");
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
void WebApi::ServeStatus(HttpResponse& resp) {
    const std::string j = m_core.SnapshotJson();
    resp.SetText(200, kJson, std::string("{\"ok\":true,") + j.substr(1));
}

void WebApi::ServeToasts(const HttpRequest& req, HttpResponse& resp) {
    const uint64_t after = QueryUInt(req, "after", 0, 0, ~0ull >> 2);
    const std::vector<Toast> toasts = m_core.ToastsAfter(after);
    JsonWriter w;
    w.BeginObject().Field("ok", true);
    w.Key("toasts").BeginArray();
    uint64_t latest = after;
    for (const Toast& t : toasts) {
        w.BeginObject().Field("id", t.id).Field("level", t.level).Field("text", t.text).EndObject();
        latest = std::max(latest, t.id);
    }
    w.EndArray();
    w.Field("latest", latest);
    w.EndObject();
    Ok(resp, w);
}

void WebApi::ServeLogs(const HttpRequest& req, HttpResponse& resp) {
    resp.SetText(200, "text/plain; charset=utf-8", m_core.LogText());
    if (req.HasQuery("download")) resp.downloadName = "image-trainer-log.txt";
}

// ---------------------------------------------------------------------------------------------
// Settings and converting
// ---------------------------------------------------------------------------------------------
void WebApi::ServeSettings(const HttpRequest& req, HttpResponse& resp) {
    std::vector<std::string> changed;
    if (req.method == "POST") {
        if (req.path == "/api/settings/reset") {
            m_core.ResetSettingsToRecommended();
        } else {
            JsonValue patch;
            std::string err;
            if (!ParseBodyObject(req, patch, &err)) {
                Fail(resp, 400, err);
                return;
            }
            if (!m_core.PatchSettings(patch, &err, &changed)) {
                Fail(resp, 400, err);
                return;
            }
        }
    }
    JsonWriter w;
    w.BeginObject().Field("ok", true);
    w.Key("changed").BeginArray();
    for (const std::string& c : changed) w.Value(c);
    w.EndArray();
    w.Key("settings");
    WriteSettingsJson(w, m_core.GetSettings());
    w.Key("defaults");
    WriteSettingsJson(w, AppSettings());
    w.EndObject();
    Ok(resp, w);
}

void WebApi::ServeConvert(const HttpRequest& req, const std::string& action, HttpResponse& resp) {
    if (action == "start") {
        JsonValue body;
        std::string err;
        if (!ParseBodyObject(req, body, &err)) {
            Fail(resp, 400, err);
            return;
        }
        ConvertRequest cr;
        if (const JsonValue* ids = body.Find("ids")) {
            if (ids->type != JsonValue::Type::Array || ids->items.empty()) {
                Fail(resp, 400, "Pick at least one picture, or leave the list out to convert all of them.");
                return;
            }
            for (const JsonValue& v : ids->items) {
                if (v.type == JsonValue::Type::Number && v.number >= 0 && v.number < 1e15) cr.ids.push_back(static_cast<uint64_t>(v.number));
            }
            if (cr.ids.empty()) {
                Fail(resp, 400, "None of the picture numbers made sense.");
                return;
            }
        }
        body.GetBool("redo", cr.redo);
        if (!m_core.StartConvert(cr, &err)) {
            Fail(resp, 409, err);
            return;
        }
    } else if (action == "stop") {
        m_core.StopConvert();
    } else {
        Fail(resp, 404, "Unknown request.");
        return;
    }
    JsonWriter w;
    w.BeginObject().Field("ok", true).Field("converting", m_core.IsConverting()).EndObject();
    Ok(resp, w);
}

void WebApi::ServeSelfTest(HttpResponse& resp) {
    std::string report, err;
    const bool passed = m_core.RunSelfTestAgain(&report, &err);
    if (report.empty()) {   // refused: nothing was tested
        Fail(resp, 409, err.empty() ? std::string("The self-test can't run right now.") : err);
        return;
    }
    JsonWriter w;
    w.BeginObject().Field("ok", true).Field("passed", passed).Field("report", report).EndObject();
    Ok(resp, w);
}

void WebApi::ServeLive(const HttpRequest& req, bool image, HttpResponse& resp) {
    m_core.NoteLiveInterest();   // somebody is looking: the converter keeps copying the picture for the next few seconds
    const std::shared_ptr<const LiveView> v = m_core.GetLive();
    if (!image) {
        JsonWriter w;
        w.BeginObject().Field("ok", true);
        if (!v) {
            w.Field("version", 0).EndObject();
        } else {
            w.Field("version", v->version).Field("id", v->pictureId).Field("name", v->name).Field("index", v->index)
                .Field("groupSize", v->groupSize).Field("strokesDone", v->strokesDone).Field("strokesTotal", v->strokesTotal)
                .Field("error", v->error).Field("w", v->painted.w).Field("h", v->painted.h).EndObject();
        }
        Ok(resp, w);
        return;
    }
    if (!v) {
        Fail(resp, 404, "Nothing is being painted right now.");
        return;
    }
    const bool painted = req.Query("which", "painted") != "original";
    Rgb8Image img = painted ? v->painted : v->original;
    if (!img.Valid()) {
        Fail(resp, 404, "Nothing is being painted right now.");
        return;
    }
    if (req.HasQuery("s")) {
        const uint32_t s = static_cast<uint32_t>(QueryUInt(req, "s", img.w, 16, 512));
        if (s != img.w) img = ResizeRgb(img.px.data(), img.w, img.h, s, s);
    }
    resp.SetBytes(200, kPng, PngOf(img));
    resp.cacheControl = "no-store";
}

// ---------------------------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------------------------
void WebApi::ServePictures(const HttpRequest& req, HttpResponse& resp) {
    Dataset& ds = m_core.Pictures();
    const size_t offset = static_cast<size_t>(QueryUInt(req, "offset", 0, 0, 100000000));
    const size_t limit = static_cast<size_t>(QueryUInt(req, "limit", 60, 1, 300));
    const std::vector<DatasetCardInfo> items = ds.List(offset, limit);
    JsonWriter w;
    w.BeginObject().Field("ok", true);
    w.Field("total", static_cast<uint64_t>(ds.Size()));
    w.Field("capacity", static_cast<uint64_t>(ds.Capacity()));
    w.Field("version", ds.Version());
    w.Field("offset", static_cast<uint64_t>(offset));
    w.Key("items").BeginArray();
    for (const DatasetCardInfo& it : items) {
        w.BeginObject().Field("id", it.id).Field("k", HashText(it.hash)).Field("name", it.name).EndObject();
    }
    w.EndArray().EndObject();
    Ok(resp, w);
}

void WebApi::ServePictureImage(const HttpRequest& req, uint64_t id, HttpResponse& resp) {
    Rgb8Image img;
    if (!m_core.Pictures().GetImage(id, img) || !img.Valid()) {
        Fail(resp, 404, "That picture is not in the set (it may have been removed).");
        return;
    }
    const uint32_t s = static_cast<uint32_t>(QueryUInt(req, "s", img.w, 16, img.w));
    if (s != img.w) img = ResizeRgb(img.px.data(), img.w, img.h, s, s);
    resp.SetBytes(200, kPng, PngOf(img));
    resp.cacheControl = "private, max-age=86400"; // the page adds ?k=<fingerprint>, so a changed picture never hits an old copy
}

void WebApi::ServePictureUpload(const HttpRequest& req, HttpResponse& resp) {
    const uint64_t w = QueryUInt(req, "w", 0, 0, 100000);
    const uint64_t h = QueryUInt(req, "h", 0, 0, 100000);
    if (w < 1 || h < 1 || w > 1024 || h > 1024) {
        Fail(resp, 400, "The picture size is missing or out of range (1 to 1024 pixels).");
        return;
    }
    if (req.body.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 3) {
        Fail(resp, 400, "The picture data does not have the size it announced.");
        return;
    }
    const Dataset::AddResult r = m_core.Pictures().Add(req.body.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h), req.Query("name"),
                                                       m_core.GetSettings().resize);
    JsonWriter jw;
    jw.BeginObject().Field("ok", r.status != Dataset::AddStatus::Invalid && r.status != Dataset::AddStatus::DiskError);
    jw.Field("status", AddStatusName(r.status)).Field("id", r.id).Field("message", r.message);
    jw.Field("total", static_cast<uint64_t>(m_core.Pictures().Size())).EndObject();
    Ok(resp, jw);
}

void WebApi::ServePictureFileUpload(const HttpRequest& req, HttpResponse& resp) {
    if (!m_hooks.decodeImageFile) {
        Fail(resp, 501, "This build can't open picture files on the Xbox. Use the page's own picture picker instead.");
        return;
    }
    Rgb8Image img;
    std::string err;
    if (req.body.empty() || !m_hooks.decodeImageFile(req.body.data(), req.body.size(), img, &err) || !img.Valid()) {
        JsonWriter jw;
        jw.BeginObject().Field("ok", false).Field("status", "invalid")
          .Field("error", err.empty() ? std::string("That file is not a picture the Xbox can read.") : err).EndObject();
        resp.SetText(200, kJson, jw.Take());
        return;
    }
    const Dataset::AddResult r = m_core.Pictures().Add(img.px.data(), img.w, img.h, req.Query("name"), m_core.GetSettings().resize);
    JsonWriter jw;
    jw.BeginObject().Field("ok", r.status != Dataset::AddStatus::Invalid && r.status != Dataset::AddStatus::DiskError);
    jw.Field("status", AddStatusName(r.status)).Field("id", r.id).Field("message", r.message);
    jw.Field("total", static_cast<uint64_t>(m_core.Pictures().Size())).EndObject();
    Ok(resp, jw);
}

void WebApi::ServePictureDelete(const HttpRequest& req, HttpResponse& resp) {
    Dataset& ds = m_core.Pictures();
    size_t removed = 0;
    if (req.path == "/api/pictures/clear") {
        removed = ds.Size();
        ds.Clear();
        m_core.AppendLog("All pictures were removed from the set.");
    } else {
        JsonValue body;
        std::string err;
        if (!ParseBodyObject(req, body, &err)) {
            Fail(resp, 400, err);
            return;
        }
        const JsonValue* ids = body.Find("ids");
        if (!ids || ids->type != JsonValue::Type::Array) {
            Fail(resp, 400, "Say which pictures to remove.");
            return;
        }
        std::vector<uint64_t> list;
        for (const JsonValue& v : ids->items) {
            if (v.type == JsonValue::Type::Number && v.number >= 0 && v.number < 1e15) list.push_back(static_cast<uint64_t>(v.number));
        }
        removed = ds.RemoveMany(list);
        m_core.AppendLog(std::to_string(removed) + " picture(s) removed from the set.");
    }
    JsonWriter w;
    w.BeginObject().Field("ok", true).Field("removed", static_cast<uint64_t>(removed)).Field("total", static_cast<uint64_t>(ds.Size())).EndObject();
    Ok(resp, w);
}

// ---------------------------------------------------------------------------------------------
// Finished pictures (cards)
// ---------------------------------------------------------------------------------------------
void WebApi::ServeCards(const HttpRequest& req, HttpResponse& resp) {
    const size_t offset = static_cast<size_t>(QueryUInt(req, "offset", 0, 0, 100000000));
    const size_t limit = static_cast<size_t>(QueryUInt(req, "limit", 60, 1, 300));
    const std::vector<CardInfo> all = m_core.Cards().List();   // newest first
    JsonWriter w;
    w.BeginObject().Field("ok", true);
    w.Field("total", static_cast<uint64_t>(all.size()));
    w.Field("version", m_core.Cards().Version());
    w.Field("bytes", m_core.Cards().TotalBytes());
    w.Field("offset", static_cast<uint64_t>(offset));
    w.Key("items").BeginArray();
    for (size_t i = offset; i < all.size() && i < offset + limit; ++i) {
        const CardInfo& c = all[i];
        DatasetCardInfo di;
        const bool hasOriginal = m_core.Pictures().FindByHash(c.key.pictureHash, di);
        w.BeginObject();
        w.Field("id", c.id).Field("name", c.name).Field("preset", c.preset).Field("presetName", PresetName(std::min(c.preset, 3u)));
        w.Field("strokes", c.key.strokes).Field("fit", c.key.fit).Field("seed", c.key.seed).Field("candidates", c.key.candidates)
            .Field("rounds", c.key.rounds);
        w.Key("flights").BeginArray().Value(c.perFlight[0]).Value(c.perFlight[1]).Value(c.perFlight[2]).EndArray();
        w.Field("empty", c.emptyStrokes).Field("error", c.errorRms).Field("seconds", c.seconds).Field("speed", c.strokesPerSecond);
        w.Field("group", c.groupSize).Field("created", c.createdUnix).Field("bytes", c.fileBytes).Field("hasOriginal", hasOriginal);
        w.EndObject();
    }
    w.EndArray().EndObject();
    Ok(resp, w);
}

void WebApi::ServeCardImage(const HttpRequest& req, const std::string& cardId, bool painted, HttpResponse& resp) {
    const uint32_t s = static_cast<uint32_t>(QueryUInt(req, "s", 128, 16, 512));
    Rgb8Image img;
    if (painted) {
        if (!m_core.PaintedPicture(cardId, s, img)) {
            Fail(resp, 404, "That finished picture is not saved (it may have been deleted).");
            return;
        }
    } else {
        CardInfo ci;
        if (!m_core.Cards().Get(cardId, ci)) {
            Fail(resp, 404, "That finished picture is not saved (it may have been deleted).");
            return;
        }
        if (!m_core.OriginalPicture(ci, s, img)) {
            Fail(resp, 404, "The original picture is not in the set any more.");
            return;
        }
    }
    resp.SetBytes(200, kPng, PngOf(img));
    resp.cacheControl = "private, max-age=86400"; // the page adds ?k=<time it was made>, so a card made again never shows an old copy
}

void WebApi::ServeCardDelete(const HttpRequest& req, HttpResponse& resp) {
    size_t removed = 0;
    if (req.path == "/api/cards/clear") {
        removed = m_core.DeleteAllCards();
    } else {
        JsonValue body;
        std::string err;
        if (!ParseBodyObject(req, body, &err)) {
            Fail(resp, 400, err);
            return;
        }
        const JsonValue* ids = body.Find("ids");
        if (!ids || ids->type != JsonValue::Type::Array) {
            Fail(resp, 400, "Say which finished pictures to delete.");
            return;
        }
        std::vector<std::string> list;
        for (const JsonValue& v : ids->items) {
            if (v.type == JsonValue::Type::String && IsSafeCardId(v.text)) list.push_back(v.text);
        }
        removed = m_core.DeleteCards(list);
    }
    JsonWriter w;
    w.BeginObject().Field("ok", true).Field("removed", static_cast<uint64_t>(removed)).Field("total", static_cast<uint64_t>(m_core.Cards().Count())).EndObject();
    Ok(resp, w);
}

} // namespace XboxOTFM
