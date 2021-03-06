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

#include <algorithm>

#include <boost/python.hpp>
#include <boost/python/stl_iterator.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "dbglog/dbglog.hpp"

#include "utility/uri.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "vts-libs/storage/fstreams.hpp"

#include "../support/python.hpp"
#include "../support/serialization.hpp"

#include "./geodatavectorbase.hpp"
#include "./files.hpp"

namespace ba = boost::algorithm;
namespace vf = geo::vectorformat;

namespace generator {

namespace {

void parseLayers(boost::optional<geo::heightcoding::Config::LayerNames> &result
                 , const std::string &name, const Json::Value &value)
{
    if (!value.isMember(name)) { return; }

    const auto layers(value[name]);
    if (!layers.isArray()) {
        LOGTHROW(err1, FormatError)
            << "Geodata definition[layers] is not an array.";
    }

    result = boost::in_place();
    for (const auto &layer : layers) {
        result->push_back(layer.asString());
    }
    std::sort(result->begin(), result->end());
}

void parse(vf::GeodataConfig &config, const Json::Value &value)
{
    Json::getOpt(config.resolution, value, "resolution");
}

void parseDefinition(GeodataVectorBase::Definition &def
                     , const Json::Value &value)
{
    std::string s;

    Json::get(def.dataset, value, "dataset");
    Json::get(def.dem.dataset, value, "demDataset");

    if (value.isMember("geoidGrid")) {
        def.dem.geoidGrid = boost::in_place();
        Json::get(*def.dem.geoidGrid, value, "geoidGrid");
    }

    parseLayers(def.layers, "layers", value);
    parseLayers(def.clipLayers, "clipLayers", value);

    if (value.isMember("format")) {
        Json::get(s, value, "format");
        try {
            def.format  = boost::lexical_cast<geo::VectorFormat>(s);
        } catch (boost::bad_lexical_cast) {
            utility::raise<FormatError>
                ("Value stored in format is not a valid height"
                 " coded data format.");
        }
    }

    if (value.isMember("formatConfig")) {
        const auto formatConfig(value["formatConfig"]);
        if (!formatConfig.isObject()) {
            LOGTHROW(err1, FormatError)
                << "Geodata definition[formatConfig] is not an object.";
        }

        switch (def.format) {
        case geo::VectorFormat::geodataJson:
            parse(createGeodataConfig(def.formatConfig), formatConfig);
            break;

        default:
            // ignore
            break;
        }
    }

    Json::getOpt(def.styleUrl, value, "styleUrl");
    Json::get(def.displaySize, value, "displaySize");
    Json::getOpt(def.mode, value, "mode");

    if (value.isMember("enhance")) {
        const auto enhance(value["enhance"]);
        if (!enhance.isObject()) {
            LOGTHROW(err1, FormatError)
                << "Geodata definition[enhance] is not an object.";
        }

        for (const auto &layerName : enhance.getMemberNames()) {
            const auto &layer(enhance[layerName]);
            auto &lh(def.layerEnhancers[layerName]);
            Json::get(lh.key, layer, "key");
            Json::get(lh.databasePath, layer, "db");
            Json::get(lh.table, layer, "table");
        }
    }

    def.heightFunction = HeightFunction::parse(value, "heightFunction");

    if (value.isMember("introspection")) {
        const auto &jintrospection(value["introspection"]);

        def.introspection.surface
            = introspectionIdFrom(jintrospection, "surface");

        if (jintrospection.isMember("browserOptions")) {
            def.introspection.browserOptions
                = Json::check(jintrospection["browserOptions"]
                              , Json::objectValue);
        }
    }
}

void buildLayers(const boost::optional
                 <geo::heightcoding::Config::LayerNames> &layers
                 , const std::string &name, Json::Value &value)
{
    if (!layers) { return; }
    auto &players(value[name] = Json::arrayValue);
    for (const auto &layer : *layers) {
        players.append(layer);
    }
}

void build(Json::Value &value, const vf::GeodataConfig *config)
{
    if (!config) { return; }
    value["resolution"] = config->resolution;
}

void buildDefinition(Json::Value &value
                     , const GeodataVectorBase::Definition &def)
{
    value["dataset"] = def.dataset;
    value["demDataset"] = def.dem.dataset;

    if (def.dem.geoidGrid) {
        value["geoidGrid"] = *def.dem.geoidGrid;
    }

    buildLayers(def.layers, "layers", value);
    buildLayers(def.clipLayers, "clipLayers", value);

    value["format"] = boost::lexical_cast<std::string>(def.format);
    auto &formatConfig(value["formatConfig"] = Json::objectValue);
    switch (def.format) {

    case geo::VectorFormat::geodataJson:
        build(formatConfig, boost::get<vf::GeodataConfig>(&def.formatConfig));
        break;

    default:
        // ignore
        break;
    }

    value["displaySize"] = def.displaySize;
    value["styleUrl"] = def.styleUrl;
    value["mode"] = boost::lexical_cast<std::string>(def.mode);

    if (!def.layerEnhancers.empty()) {
        auto &layerEnhancers(value["enhance"] = Json::objectValue);
        for (const auto &item : def.layerEnhancers) {
            auto &layer(layerEnhancers[item.first] = Json::objectValue);
            layer["key"] = item.second.key;
            layer["db"] = item.second.databasePath;
            layer["table"] = item.second.table;
        }
    }

    if (def.heightFunction) {
        boost::any tmp(Json::Value(Json::objectValue));
        def.heightFunction->build(tmp);
        value["heightFunction"] = boost::any_cast<const Json::Value&>(tmp);
    }

    if (!def.introspection.empty()) {
        auto &jintrospection(value["introspection"] = Json::objectValue);
        introspectionIdTo(jintrospection, "surface"
                          , def.introspection.surface);

        if (!def.introspection.browserOptions.empty()) {
            jintrospection["browserOptions"]
                = boost::any_cast<const Json::Value&>
                (def.introspection.browserOptions);
        }
    }
}

void parseLayers(boost::optional<geo::heightcoding::Config::LayerNames> &result
                 , const std::string &name, const boost::python::dict &value)
{
    if (!value.has_key(name)) { return; }

    result = boost::in_place();
    auto pylayers(value[name]);
    for (boost::python::stl_input_iterator<boost::python::object>
             ipylayers(pylayers), epylayers;
         ipylayers != epylayers; ++ipylayers)
    {
        result->push_back(py2utf8(*ipylayers));
    }
    std::sort(result->begin(), result->end());
}

void parse(vf::GeodataConfig &config, const boost::python::dict &value)
{
    if (value.has_key("resolution")) {
        config.resolution
            = boost::python::extract<int>(value["resolution"]);
    }
}

void parseDefinition(GeodataVectorBase::Definition &def
                     , const boost::python::dict &value)
{
    def.dataset = py2utf8(value["dataset"]);
    def.dem.dataset = py2utf8(value["demDataset"]);

    if (value.has_key("geoidGrid")) {
        def.dem.geoidGrid = py2utf8(value["geoidGrid"]);
    }

    parseLayers(def.layers, "layers", value);
    parseLayers(def.clipLayers, "clipLayers", value);

    if (value.has_key("format")) {
        try {
            def.format = boost::lexical_cast<geo::VectorFormat>
                (py2utf8(value["format"]));
        } catch (boost::bad_lexical_cast) {
            utility::raise<Error>
                ("Value stored in format is not a valid height"
                 " coded data format.");
        }
    }

    if (value.has_key("formatConfig")) {
        boost::python::dict formatConfig(value["formatConfig"]);
        switch (def.format) {
        case geo::VectorFormat::geodataJson:
            parse(createGeodataConfig(def.formatConfig), formatConfig);
            break;

        default:
            // ignore
            break;
        }
    }

    def.displaySize = boost::python::extract<int>(value["displaySize"]);
    def.styleUrl = py2utf8(value["styleUrl"]);

    if (value.has_key("mode")) {
        try {
            def.mode = boost::lexical_cast<geo::heightcoding::Mode>
                (py2utf8(value["mode"]));
        } catch (boost::bad_lexical_cast) {
            utility::raise<Error>
                ("Value stored in mode is not a valid height coding mode.");
        }
    }

    if (value.has_key("enhance")) {
        const auto enhance(value["enhance"]);
        for (boost::python::stl_input_iterator<boost::python::str>
                 ilayer(enhance), elayer; ilayer != elayer; ++ilayer)
        {
            const auto name(py2utf8(*ilayer));
            const auto content(enhance[*ilayer]);

            auto &lh(def.layerEnhancers[name]);
            lh.key = py2utf8(content["key"]);
            lh.databasePath = py2utf8(content["db"]);
            lh.table = py2utf8(content["table"]);
        }
    }

    def.heightFunction = HeightFunction::parse(value, "heightFunction");

    if (value.has_key("introspection")) {
        boost::python::dict pintrospection(value["introspection"]);
        def.introspection.surface
            = introspectionIdFrom(pintrospection, "surface");
    }
}

} // namespace

bool GeodataVectorBase::Introspection::empty() const
{
    return (!surface && browserOptions.empty());
}

bool GeodataVectorBase::Introspection::operator!=(const Introspection &other)
    const
{
    // introspection can safely change
    if (surface != other.surface) { return true; }

    if (browserOptions.empty() != other.browserOptions.empty()) {
        return true;
    }
    if (!browserOptions.empty()
        && (boost::any_cast<const Json::Value&>(browserOptions)
            != boost::any_cast<const Json::Value&>(other.browserOptions)))
    {
        return true;
    }

    return false;
}

void GeodataVectorBase::Definition::from_impl(const boost::any &value)
{
    if (const auto *json = boost::any_cast<Json::Value>(&value)) {
        parseDefinition(*this, *json);
    } else if (const auto *py
               = boost::any_cast<boost::python::dict>(&value))
    {
        parseDefinition(*this, *py);
    } else {
        LOGTHROW(err1, Error)
            << "GeodataVectorBase: Unsupported configuration from: <"
            << value.type().name() << ">.";
    }
}

void GeodataVectorBase::Definition::to_impl(boost::any &value) const
{
    if (auto *json = boost::any_cast<Json::Value>(&value)) {
        buildDefinition(*json, *this);
    } else {
        LOGTHROW(err1, Error)
            << "GeodataVectorBase:: Unsupported serialization into: <"
            << value.type().name() << ">.";
    }
}

bool differ(const vf::GeodataConfig &l, const vf::GeodataConfig *r)
{
    // different type?
    if (!r) { return true; }
    // same types
    if (l.resolution != r->resolution) { return true; }
    return false;
}

bool differ(const vf::Config &l, const vf::Config &r)
{
    if (const auto *cl = boost::get<vf::GeodataConfig>(&l)) {
        return differ(*cl, boost::get<vf::GeodataConfig>(&r));
    }
    // add more config types here
    return true;
}

Changed
GeodataVectorBase::Definition::changed_impl(const DefinitionBase &o) const
{
    const auto &other(o.as<Definition>());

    // accumulate revision bump/safe changes
    bool bump(false);
    bool safe(false);

    // changing these bumps version
    if (dem != other.dem) { bump = true; }
    if (dataset != other.dataset) { bump = true; }
    if (layers != other.layers) { bump = true; }
    if (clipLayers != other.clipLayers) { bump = true; }
    if (mode != other.mode) { bump = true; }
    if (layerEnhancers != other.layerEnhancers) { bump = true; }
    if (HeightFunction::changed(heightFunction, other.heightFunction)) {
        bump = true;
    }

    // different format leads to version bump!
    if (format != other.format) { bump = true; }
    // different format config leads to version bump!
    if (differ(formatConfig, other.formatConfig)) { bump = true; }
    // displaySize can change
    if (displaySize != other.displaySize) { safe = true; }
    // styleUrl can change
    if (styleUrl != other.styleUrl) { safe = true; }

    // introspection can change
    if (introspection != other.introspection) { safe = true; }

    // interpret change type
    if (bump) { return Changed::withRevisionBump; }
    if (safe) { return Changed::safely; }
    return Changed::no;
}

GeodataVectorBase::GeodataVectorBase(const Params &params, bool tiled)
    : Generator(params)
    , definition_(this->resource().definition<Definition>())
    , layerEnhancers_(definition_.layerEnhancers)
    , tiled_(tiled)
    , styleUrl_(definition_.styleUrl)
{
    if (styleUrl_.empty()) {
        styleUrl_ = "style.json";
    } else if (ba::istarts_with(styleUrl_, "file:")) {
        // pseudo file URL
        stylePath_ = absoluteDataset(styleUrl_.substr(5));
        styleUrl_ = "style.json";
    }

    for (auto &item : layerEnhancers_) {
        item.second.databasePath = absoluteDataset(item.second.databasePath);
    }
}

Generator::Task GeodataVectorBase::generateFile_impl(const FileInfo &fileInfo
                                                     , Sink &sink) const
{
    GeodataFileInfo fi(fileInfo, tiled_, definition_.format);

    // check for valid tileId
    switch (fi.type) {
    case GeodataFileInfo::Type::geo:
        return[=](Sink &sink, Arsenal &arsenal) {
            generateGeodata(sink, fi, arsenal);
        };
        break;

    case GeodataFileInfo::Type::metatile:
        if (tiled_) {
            return[=](Sink &sink, Arsenal &arsenal) {
                generateMetatile(sink, fi, arsenal);
            };
        }
        sink.error(utility::makeError<NotFound>
                   ("Metatiles not supported by non-tiled driver."));
        break;

    case GeodataFileInfo::Type::config: {
        std::ostringstream os;
        mapConfig(os, ResourceRoot::none);
        sink.content(os.str(), fi.sinkFileInfo());
        break;
    }

    case GeodataFileInfo::Type::definition: {
        std::ostringstream os;
        vr::saveFreeLayer(os, freeLayer_impl(ResourceRoot::none));
        sink.content(os.str(), fi.sinkFileInfo());
        break;
    }

    case GeodataFileInfo::Type::support:
        supportFile(*fi.support, sink, fi.sinkFileInfo());
        break;

    case GeodataFileInfo::Type::registry:
        sink.content(vs::fileIStream
                      (fi.registry->contentType, fi.registry->path)
                     , FileClass::registry);
        break;

    case GeodataFileInfo::Type::style:
        if (stylePath_.empty()) {
            // return internal file
            supportFile(files::defaultStyle, sink, fi.sinkFileInfo());
        } else {
            // return external file
            sink.content(vs::fileIStream
                         (files::defaultStyle.contentType, stylePath_)
                         , FileClass::config);
        }
        break;

    default:
        sink.error(utility::makeError<NotFound>("Not Found."));
        break;
    }

    return {};
}

namespace {

typedef boost::iterator_range<std::string::const_iterator> SubString;
typedef std::vector<SubString> Args;
typedef std::pair<SubString, SubString> KeyValue;

KeyValue splitArgument(const SubString &arg)
{
    auto b(std::begin(arg));
    auto e(std::end(arg));
    for (auto i(b); i != e; ++i) {
        if (*i == '=') {
            return KeyValue(SubString(b, i), SubString(std::next(i), e));
        }
    }
    return KeyValue(SubString(b, e), SubString());
}

} // namespace

std::pair<DemDataset::list, bool>
GeodataVectorBase::viewspec2datasets(const std::string &query
                                     , const DemDataset &fallback)
    const
{
    auto empty([&]() -> std::pair<DemDataset::list, bool>
    {
        return { DemDataset::list{fallback}, true };
    });

    if (query.empty()) {
        LOG(info1) << "No query -. no viewspec.";
        return empty();
    };

    Args args;
    ba::split(args, query, ba::is_any_of("&"), ba::token_compress_on);

    for (auto iargs(args.begin()), eargs(args.end()); iargs != eargs; ++iargs)
    {
        auto kv(splitArgument(*iargs));
        if (ba::equals(kv.first, "viewspec")) {
            std::vector<std::string> ids;
            ba::split(ids, kv.second, ba::is_any_of(",")
                      , ba::token_compress_on);

            // url decode values
            for (auto &id : ids) { id = utility::urlDecode(id); }

            if ((ids.size() == 1)
                && ((ids.front() == "{viewspec}")
                    || (ids.front() == "viewspec")))
            {
                // viewspec not expanded, treate as empty
                LOG(info1) << "Viewspec not expanded, ignoring.";
                return empty();
            }

            auto result(demRegistry().find(referenceFrameId(), ids));
            result.first.emplace_back(fallback);
            return result;
        }
    }

    // nothing appropriate
    LOG(info1) << "No query -> no viewspec.";
    return empty();
}

} // namespace generator
