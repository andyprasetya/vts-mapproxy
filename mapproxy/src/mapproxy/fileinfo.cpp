/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/range/iterator.hpp>

#include "utility/streams.hpp"

#include "vts-libs/registry.hpp"
#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/support.hpp"
#include "vts-libs/vts/mapconfig.hpp"
#include "vts-libs/vts/service.hpp"

#include "./error.hpp"
#include "./fileinfo.hpp"
#include "./browser2d.hpp"

namespace ba = boost::algorithm;
namespace vr = vtslibs::registry;

namespace constants {
    const std::string Config("mapConfig.json");
    const std::string BoundLayerDefinition("boundlayer.json");
    const std::string FreeLayerDefinition("freelayer.json");
    const std::string DebugConfig("debug.json");
    const std::string Self("");
    const std::string Index("index.html");
    const std::string Dems("dems.html");
    const std::string Geo("geo");
    const std::string Style("style.json");

    namespace tileset {
        const std::string Config("tileset.conf");
        const std::string Index("tileset.index");
        const std::string Registry("tileset.registry");
    } // namespace tileset

    const std::string DisableBrowserHeader("X-Mapproxy-Disable-Browser");
} // namesapce constants

namespace {

template <typename E>
bool asEnum(const std::string &str, E &value)
{
    try {
        value = boost::lexical_cast<E>(str);
    } catch (boost::bad_lexical_cast) {
        return false;
    }
    return true;
}

template <typename E, typename Error>
void asEnumChecked(const std::string &str, E &value, const std::string message)
{
    if (!asEnum(str, value)) {
        LOGTHROW(err1, NotFound)
            << "Invalid value for enum <" << str << ">: " << message;
    }
}

const std::string& checkReferenceFrame(const std::string &referenceFrame)
{
    if (vr::system.referenceFrames(referenceFrame, std::nothrow)) {
        return referenceFrame;
    }

    LOGTHROW(err1, NotFound)
        << "<" << referenceFrame << "> is not known reference frame.";
    throw;
}

} // namespace

FileInfo::FileInfo(const http::Request &request, int f)
    : url(request.uri), path(request.path), query(request.query)
    , flags(f), type(Type::resourceFile)
{
    if (flags & FileFlags::browserEnabled) {
        // browsing enabled, check for disable header
        if (request.hasHeader(constants::DisableBrowserHeader)) {
            flags &= ~FileFlags::browserEnabled;
        }
    }

    auto end(path.end());

    std::vector<std::string> components;
    {
        auto range(boost::make_iterator_range(path.begin(), end));
        ba::split(components, range, ba::is_any_of("/")
                  , ba::token_compress_on);
    }

    switch (components.size() - 1) {
    case 1:
        filename = components[1];

        if ((filename == constants::Index)
            || filename == constants::Self)
        {
            // /rf/index.html or /rf/ -> list types
            type = Type::referenceFrameListing;
            return;
        }
        // just /rf -> redir to /rf/
        type = Type::dirRedir;
        return;

    case 2:
        resourceId.referenceFrame = checkReferenceFrame(components[1]);
        filename = components[2];

        if (filename == constants::Index) {
            // /rf/index.html -> browser
            type = Type::referenceFrameBrowser;
            return;
        } else if (filename == constants::Dems) {
            // /rf/dems.html -> dems
            type = Type::referenceFrameDems;
            return;
        } else if (filename == constants::Self) {
            // /rf/ -> list types
            type = Type::typeListing;
            return;
        }

        // just /rf/type -> redir to /rf/type/
        type = Type::dirRedir;
        return;

    case 3:
        // only reference frame -> allow only map config
        resourceId.referenceFrame = checkReferenceFrame(components[1]);
        asEnumChecked<Resource::Generator::Type, NotFound>
            (components[2], generatorType, "Unknown generator type.");
        filename = components[3];

        if (filename == constants::Index) {
            // /rf/type/index.html -> browser
            type = Type::typeBrowser;
            return;
        } else if (filename == constants::Self) {
            // /rf/type/ -> list types
            type = Type::groupListing;
            return;
        }

        // just /rf/type/group -> redir to /rf/type/group/
        type = Type::dirRedir;
        return;

    case 4:
        // only reference frame -> allow only map config
        resourceId.referenceFrame = checkReferenceFrame(components[1]);
        asEnumChecked<Resource::Generator::Type, NotFound>
            (components[2], generatorType, "Unknown generator type.");
        resourceId.group = components[3];
        filename = components[4];

        if (filename == constants::Index) {
            // /rf/type/group/index.html -> browser
            type = Type::groupBrowser;
            return;
        } else if (filename == constants::Self) {
            // /rf/type/group/ -> list ids
            type = Type::idListing;
            return;
        }

        // just /rf/type/group/id -> redir to /rf/type/group/id/
        type = Type::dirRedir;
        return;

    case 5:
        // full resource file path
        resourceId.referenceFrame = checkReferenceFrame(components[1]);
        asEnumChecked<Resource::Generator::Type, NotFound>
            (components[2], generatorType, "Unknown generator type.");
        resourceId.group = components[3];
        resourceId.id = components[4];
        filename = components[5];
        return;

    default:
        if (components.size() != 6) {
            LOGTHROW(err1, NotFound)
                << "URL <" << url << "> not found: invalid number "
                "of path components.";
        }
    }

}

namespace {

inline bool isDigit(char c) { return (c >= '0') && (c <= '9'); }

inline char positive(char c) { return c - '0'; }

template <unsigned int minWidth, char(*getter)(char), typename T>
inline const char* parsePartImpl(const char *p, T &value)
{
    bool prefix = false;
    char c(p[0]);
    switch (c) {
    case '-': case '+': return nullptr;
    case '0': prefix = true;
    }

    value = 0;

    const char *e(p);
    while (isDigit(c)) {
        value *= 10;
        value += getter(c);
        c = *++e;
    }

    auto dist(e - p);
    if (dist < minWidth) { return nullptr; }
    if (prefix && (dist > minWidth)) { return nullptr; }
    return e;
}

template <unsigned int minWidth, typename T>
inline const char* parsePart(const char *p, T &value)
{
    // only positive numbers are allowed
    return parsePartImpl<minWidth, positive>(p, value);
}

} // namespace

TmsFileInfo::TmsFileInfo(const FileInfo &fi)
    : fileInfo(fi), type(Type::unknown), support()
{
    if ([&]() -> bool
    {
        const char *p(fi.filename.c_str());

        if (!(p = parsePart<1>(p, tileId.lod))) { return false; }
        if (*p++ != '-') { return false; }

        if (!(p = parsePart<1>(p, tileId.x))) { return false; }
        if (*p++ != '-') { return false; }

        if (!(p = parsePart<1>(p, tileId.y))) { return false; }
        if (*p++ != '.') { return false; }

        std::string ext(p);
        if (ext == "mask") {
            // mask file
            type = Type::mask;
        } else if (ext == "meta") {
            // mask file
            type = Type::metatile;
        } else {
            // another file -> parse as format
            type = Type::image;
            if (!asEnum<RasterFormat>(ext, format)) { return false; }
        }

        return true;
    }()) {
        return;
    }

    if (constants::Config == fi.filename) {
        type = Type::config;
        return;
    }

    if (constants::BoundLayerDefinition == fi.filename) {
        type = Type::definition;
        return;
    }

    if (fi.flags & FileFlags::browserEnabled) {
        LOG(debug) << "Browser enabled, checking browser files.";

        auto path(fi.filename);
        if (constants::Self == path) { path = constants::Index; }

        // support files
        auto fsupport(browser2d::supportFiles.find(path));
        if (fsupport != browser2d::supportFiles.end()) {
            type = Type::support;
            support = &fsupport->second;
            return;
        }
    } else {
        LOG(debug) << "Browser disabled, skipping browser files.";
    }
}

Sink::FileInfo TmsFileInfo::sinkFileInfo(std::time_t lastModified) const
{
    switch (type) {
    case Type::config:
        return Sink::FileInfo(vts::MapConfig::contentType, lastModified)
            .setFileClass(FileClass::config);

    case Type::image:
        return Sink::FileInfo(contentType(format), lastModified)
            .setFileClass(FileClass::data);

    case Type::mask:
        return Sink::FileInfo(contentType(MaskFormat), lastModified)
            .setFileClass(FileClass::data);

    case Type::metatile:
        return Sink::FileInfo(contentType(RasterMetatileFormat), lastModified)
            .setFileClass(FileClass::data);

    case Type::support:
        return Sink::FileInfo(support->contentType, support->lastModified)
        .setFileClass(FileClass::support);

    case Type::definition:
        return Sink::FileInfo("application/json; charset=utf-8", lastModified)
            .setFileClass(FileClass::config);

    case Type::unknown:
        return {};
    }

    return {};
}

SurfaceFileInfo::SurfaceFileInfo(const FileInfo &fi)
    : fileInfo(fi), type(Type::unknown), fileType(vs::File::config)
    , tileType(vts::TileFile::meta), flavor(vts::FileFlavor::regular)
    , support(), registry(), serviceFile()
{
    if (vts::fromFilename
        (tileId, tileType, subTileIndex, fi.filename, 0, &flavor))
    {
        type = Type::tile;
        return;
    }

    // non-tile files
    if (constants::Config == fi.filename) {
        type = Type::file;
        fileType = vs::File::config;
        return;
    }

    if (constants::FreeLayerDefinition == fi.filename) {
        type = Type::definition;
        return;
    }

    if (constants::tileset::Config == fi.filename) {
        type = Type::file;
        fileType = vs::File::config;
        // this is raw file
        flavor = vts::FileFlavor::raw;
        return;
    }

    if (constants::tileset::Index == fi.filename) {
        type = Type::file;
        fileType = vs::File::tileIndex;
        return;
    }

    if (constants::tileset::Registry == fi.filename) {
        type = Type::file;
        fileType = vs::File::registry;
        return;
    }

    if (fi.flags & FileFlags::browserEnabled) {
        LOG(debug) << "Browser enabled, checking browser files.";

        auto path(fi.filename);
        if (constants::Self == path) { path = constants::Index; }

        // support files
        auto fsupport(vts::supportFiles.find(path));
        if (fsupport != vts::supportFiles.end()) {
            type = Type::support;
            support = &fsupport->second;
            return;
        }
    } else {
        LOG(debug) << "Browser disabled, skipping browser files.";
    }

    // extra files, unknown to common machinery
    registry = vr::dataFile
        (fi.filename, vr::DataFile::Key::filename, std::nothrow);
    if (registry) {
        type = Type::registry;
        return;
    }

    serviceFile = vts::service::match(fi.filename);
    if (serviceFile) {
        type = Type::service;
        return;
    }

    if (constants::DebugConfig == fi.filename) {
        type = Type::file;
        fileType = vs::File::config;
        // this is debug file
        flavor = vts::FileFlavor::debug;
        return;
    }
}

std::string contentType(vts::TileFile tileType, vts::FileFlavor flavor)
{
    switch (tileType) {
    case vts::TileFile::meta:
        if (flavor == vts::FileFlavor::debug) {
            // debug node
            return "application/json; charset=utf-8";
        }
        return vs::contentType(tileType);

    default:
        return vs::contentType(tileType);
    }
}

Sink::FileInfo SurfaceFileInfo::sinkFileInfo(std::time_t lastModified) const
{
    switch (type) {
    case Type::file:
        return Sink::FileInfo(vs::contentType(fileType), lastModified)
            .setFileClass(FileClass::config);

    case Type::tile:
        return Sink::FileInfo(contentType(tileType, flavor), lastModified)
            .setFileClass(FileClass::data);

    case Type::support:
        return Sink::FileInfo(support->contentType, support->lastModified)
            .setFileClass(FileClass::support);

    case Type::registry:
        return Sink::FileInfo(registry->contentType, lastModified)
            .setFileClass(FileClass::registry);

    case Type::service:
        // service privides its own file info
        return {};

    case Type::definition:
        return Sink::FileInfo("application/json", lastModified)
            .setFileClass(FileClass::config);

    case Type::unknown:
        return {};
    }

    return {};
}

GeodataFileInfo::GeodataFileInfo(const FileInfo &fi, bool tiled
                                 , geo::VectorFormat format)
    : fileInfo(fi), type(Type::unknown), support()
    , format(format)
{
    if (tiled && [&]() -> bool
    {
        const char *p(fi.filename.c_str());

        if (!(p = parsePart<1>(p, tileId.lod))) { return false; }
        if (*p++ != '-') { return false; }

        if (!(p = parsePart<1>(p, tileId.x))) { return false; }
        if (*p++ != '-') { return false; }

        if (!(p = parsePart<1>(p, tileId.y))) { return false; }
        if (*p++ != '.') { return false; }

        std::string ext(p);
        if (ext == "geo") {
            // mask file
            type = Type::geo;
        } else if (ext == "meta") {
            // mask file
            type = Type::metatile;
        }

        return true;
    }()) {
        return;
    }

    if (constants::Config == fi.filename) {
        type = Type::config;
        return;
    }

    if (!tiled && constants::Geo == fi.filename) {
        type = Type::geo;
        return;
    }

    if (constants::FreeLayerDefinition == fi.filename) {
        type = Type::definition;
        return;
    }

    if (constants::Style == fi.filename) {
        type = Type::style;
        return;
    }

    if (fi.flags & FileFlags::browserEnabled) {
        LOG(debug) << "Browser enabled, checking browser files.";

        auto path(fi.filename);
        if (constants::Self == path) { path = constants::Index; }

        // support files
        auto fsupport(vts::supportFiles.find(path));
        if (fsupport != vts::supportFiles.end()) {
            type = Type::support;
            support = &fsupport->second;
            return;
        }
    } else {
        LOG(debug) << "Browser disabled, skipping browser files.";
    }

    // extra files, unknown to common machinery
    registry = vr::dataFile
        (fi.filename, vr::DataFile::Key::filename, std::nothrow);
    if (registry) {
        type = Type::registry;
        return;
    }
}

Sink::FileInfo GeodataFileInfo::sinkFileInfo(std::time_t lastModified) const
{
    switch (type) {
    case Type::geo:
        return Sink::FileInfo(contentType(format), lastModified)
            .setFileClass(FileClass::data);

    case Type::metatile:
        return Sink::FileInfo(vs::contentType(vs::TileFile::meta)
                              , lastModified)
            .setFileClass(FileClass::data);

    case Type::support:
        return Sink::FileInfo(support->contentType, support->lastModified)
            .setFileClass(FileClass::support);

    case Type::registry:
        return Sink::FileInfo(registry->contentType, lastModified)
            .setFileClass(FileClass::registry);

    case Type::config:
        return Sink::FileInfo(vts::MapConfig::contentType, lastModified)
            .setFileClass(FileClass::config);

    case Type::definition:
        return Sink::FileInfo("application/json; charset=utf-8", lastModified)
            .setFileClass(FileClass::config);

    case Type::style:
        return Sink::FileInfo("application/json; charset=utf-8", lastModified)
            .setFileClass(FileClass::config);

    case Type::unknown:
        return {};
    }

    return {};
}
