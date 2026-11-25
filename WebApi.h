#pragma once
// The "API" behind the phone web page: turns web requests into calls on AppCore. No Windows code in here, so it is tested natively.
//
//   GET    /                                 the web page
//   GET    /api/health                       {"ok":true}
//   GET    /api/status                       everything the screens show (AppCore::SnapshotJson)
//   GET    /api/toasts?after=N               short notices newer than N
//   GET    /api/logs[?download]              the activity log (text)
//   GET    /api/settings                     the settings, with the recommended values
//   POST   /api/settings                     change some settings ({"preset":2,...}); POST /api/settings/reset
//   POST   /api/convert/start                {"ids":[...],"redo":false}  (both optional: no ids = every picture)
//   POST   /api/convert/stop
//   POST   /api/selftest                     run the chip's five checks again (refused while converting)
//   GET    /api/live  /api/live.png?which=original|painted[&s=N]     the picture being painted now (asking keeps it coming)
//   GET    /api/pictures?offset=&limit=      the picture set;  GET /api/pictures/<id>.png[?s=N]
//   POST   /api/pictures  /api/pictures/file  /api/pictures/delete  /api/pictures/clear;  DELETE /api/pictures/<id>
//   GET    /api/cards?offset=&limit=         the finished pictures (newest first), with their numbers
//   GET    /api/cards/<id>/painted.png[?s=N] /api/cards/<id>/original.png[?s=N]    painted from the card's dials / the picture it came from
//   POST   /api/cards/delete {"ids":[...]}   POST /api/cards/clear;  DELETE /api/cards/<id>
//
// Everything that changes something needs the header "X-OTFM: 1" (a page on another web site cannot send it).

#include "AppCore.h"
#include "HttpServer.h"
#include "ImageUtil.h"

#include <functional>
#include <string>

namespace XboxOTFM {

struct WebApiHooks {
    // Turns a picture file (PNG, JPEG, BMP, GIF, ...) into pixels. Leave empty when the platform can't do it (then the page's own
    // in-browser conversion is the only way to add pictures).
    std::function<bool(const uint8_t* bytes, size_t size, Rgb8Image& out, std::string* error)> decodeImageFile;
};

class WebApi {
public:
    // pageBytes must stay valid for the life of the WebApi (it is the single-file web page).
    WebApi(AppCore& core, WebApiHooks hooks, const uint8_t* pageBytes, size_t pageSize);

    void Handle(const HttpRequest& req, HttpResponse& resp);
    size_t BodyLimit(const std::string& method, const std::string& path) const;

    // Largest upload for one picture sent as raw pixels, and for one picture file.
    static constexpr size_t kMaxRawPicturePixels = 1024u * 1024u;
    static constexpr size_t kMaxPictureFileBytes = 48u * 1024u * 1024u;

private:
    void ServePage(HttpResponse& resp) const;
    void ServeStatus(HttpResponse& resp);
    void ServeToasts(const HttpRequest& req, HttpResponse& resp);
    void ServeLogs(const HttpRequest& req, HttpResponse& resp);
    void ServeSettings(const HttpRequest& req, HttpResponse& resp);
    void ServeConvert(const HttpRequest& req, const std::string& action, HttpResponse& resp);
    void ServeSelfTest(HttpResponse& resp);
    void ServeLive(const HttpRequest& req, bool image, HttpResponse& resp);
    void ServePictures(const HttpRequest& req, HttpResponse& resp);
    void ServePictureImage(const HttpRequest& req, uint64_t id, HttpResponse& resp);
    void ServePictureUpload(const HttpRequest& req, HttpResponse& resp);
    void ServePictureFileUpload(const HttpRequest& req, HttpResponse& resp);
    void ServePictureDelete(const HttpRequest& req, HttpResponse& resp);
    void ServeCards(const HttpRequest& req, HttpResponse& resp);
    void ServeCardImage(const HttpRequest& req, const std::string& cardId, bool painted, HttpResponse& resp);
    void ServeCardDelete(const HttpRequest& req, HttpResponse& resp);

    static void Ok(HttpResponse& resp, JsonWriter& w);
    static void Fail(HttpResponse& resp, int status, const std::string& message);
    static bool ParseBodyObject(const HttpRequest& req, JsonValue& out, std::string* error);

    AppCore& m_core;
    WebApiHooks m_hooks;
    const uint8_t* m_page;
    size_t m_pageSize;
};

} // namespace XboxOTFM
