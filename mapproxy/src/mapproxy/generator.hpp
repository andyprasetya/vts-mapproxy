#ifndef mapproxy_generator_hpp_included_
#define mapproxy_generator_hpp_included_

#include <memory>
#include <string>
#include <iostream>
#include <atomic>

#include <boost/noncopyable.hpp>
#include <boost/any.hpp>
#include <boost/filesystem/path.hpp>

#include "vts-libs/vts/mapconfig.hpp"

#include "./resource.hpp"
#include "./resourcebackend.hpp"
#include "./fileinfo.hpp"
#include "./contentgenerator.hpp"
#include "./gdalsupport.hpp"

namespace vts = vadstena::vts;

/** Dataset generator.
 */
class Generator : boost::noncopyable {
public:
    typedef std::shared_ptr<Generator> pointer;
    typedef std::vector<pointer> list;
    typedef std::map<Resource::Id, pointer> map;
    typedef std::function<void(GdalWarper&)> Task;

    /** Configuration
     */
    struct Config {
        boost::filesystem::path root;
        boost::filesystem::path resourceRoot;
        int fileFlags;

        Config() : fileFlags() {}
    };

    virtual ~Generator() {}

    static Generator::pointer create(const Config &config
                                     , const Resource::Generator &type
                                     , const Resource &resource);

    struct Factory;
    static void registerType(const Resource::Generator &type
                             , const std::shared_ptr<Factory> &factory);

    /** Generator is ready when all data needed to serve are prepared.
     */
    bool ready() const { return ready_; }

    /** Throws Unavailable if generator is not ready yet.
     */
    void checkReady() const;

    /** Prepares generator for serving.
     */
    void prepare();

    const Resource& resource() const { return resource_; }
    const Resource::Id& id() const { return resource_.id; }
    const std::string& group() const { return resource_.id.group; }
    Resource::Generator::Type type() const { return resource_.generator.type; }
    const std::string& referenceFrameId() const {
        return resource_.id.referenceFrame;
    }
    const vr::ReferenceFrame& referenceFrame() const {
        return *resource_.referenceFrame;
    }

    const Config& config() const { return config_; }
    const boost::filesystem::path& root() const { return config_.root; }
    const boost::filesystem::path& resourceRoot() const {
        return config_.resourceRoot;
    }

    bool check(const Resource &resource) const;

    vts::MapConfig mapConfig(ResourceRoot root) const;

    Task generateFile(const FileInfo &fileInfo
                      , const Sink::pointer &sink) const;

protected:
    Generator(const Config &config, const Resource &resource);

    void makeReady();
    bool fresh() const { return fresh_; }

    void mapConfig(std::ostream &os, ResourceRoot root) const;

    std::string absoluteDataset(const std::string &path) const;
    boost::optional<std::string>
    absoluteDataset(const boost::optional<std::string> &path) const;

private:
    virtual void prepare_impl() = 0;
    virtual vts::MapConfig mapConfig_impl(ResourceRoot root) const = 0;

    virtual Task generateFile_impl(const FileInfo &fileInfo
                                   , const Sink::pointer &sink) const = 0;

    Config config_;
    Resource resource_;
    Resource savedResource_;
    bool fresh_;
    std::atomic<bool> ready_;
};

/** Set of dataset generators.
 */
class Generators {
public:
    struct Config : Generator::Config {
        int resourceUpdatePeriod;

        Config() : resourceUpdatePeriod(100) {}
    };

    /** Creates generator set.
     */
    Generators(const Config &config
               , const ResourceBackend::pointer &resourceBackend);

    ~Generators();

    const Config& config() const;

    /** Returns generator for requested file.
     */
    Generator::pointer generator(const FileInfo &fileInfo) const;

    /** Returns list of all generators for given referenceFrame.
     */
    Generator::list referenceFrame(const std::string &referenceFrame) const;

    std::vector<std::string> listGroups(const std::string &referenceFrame
                                        , Resource::Generator::Type type)
        const;

    std::vector<std::string> listIds(const std::string &referenceFrame
                                     , Resource::Generator::Type type
                                     , const std::string &group) const;

    // internals
    struct Detail;

private:
    std::shared_ptr<Detail> detail_;
    Detail& detail() { return *detail_; }
    const Detail& detail() const { return *detail_; }
};

// inlines

inline void Generator::prepare()
{
    prepare_impl();
}

inline vts::MapConfig Generator::mapConfig(ResourceRoot root) const
{
    return mapConfig_impl(root);
}

inline Generator::Task Generator::generateFile(const FileInfo &fileInfo
                                               , const Sink::pointer &sink)
    const
{
    return generateFile_impl(fileInfo, sink);
}

#endif // mapproxy_generator_hpp_included_
