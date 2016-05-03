#include <new>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <opencv2/highgui/highgui.hpp>

#include "utility/premain.hpp"
#include "utility/raise.hpp"

#include "geometry/mesh.hpp"

#include "geo/coordinates.hpp"

#include "vts-libs/storage/fstreams.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/tileset/config.hpp"
#include "vts-libs/vts/metatile.hpp"
#include "vts-libs/vts/csconvertor.hpp"
#include "vts-libs/vts/math.hpp"
#include "vts-libs/vts/mesh.hpp"
#include "vts-libs/vts/opencv/navtile.hpp"

#include "../error.hpp"
#include "../support/metatile.hpp"
#include "../support/mesh.hpp"
#include "../support/srs.hpp"
#include "../support/grid.hpp"

#include "./surface-dem.hpp"
#include "./factory.hpp"

namespace fs = boost::filesystem;
namespace vr = vadstena::registry;
namespace vs = vadstena::storage;
namespace vts = vadstena::vts;

namespace generator {

namespace {

struct Factory : Generator::Factory {
    virtual Generator::pointer create(const Generator::Config &config
                                      , const Resource &resource)
    {
        return std::make_shared<SurfaceDem>(config, resource);
    }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType
        (resdef::SurfaceDem::generator, std::make_shared<Factory>());
});

} // namespace

SurfaceDem::SurfaceDem(const Config &config
                       , const Resource &resource)
    : SurfaceBase(config, resource)
    , definition_(this->resource().definition<resdef::SurfaceDem>())
    , dataset_(absoluteDataset(definition_.dataset + "/dem"))
    , datasetMin_(dataset_ + ".min")
    , datasetMax_(dataset_ + ".max")
{
    try {
        auto indexPath(filePath(vts::File::tileIndex));
        auto propertiesPath(filePath(vts::File::config));
        if (fs::exists(indexPath) && fs::exists(propertiesPath)) {
            // both paths exist -> ok
            vts::tileset::loadTileSetIndex(index_, indexPath);
            properties_ = vts::tileset::loadConfig(propertiesPath);
            makeReady();
            return;
        }
    } catch (const std::exception &e) {
        // not ready
    }
    LOG(info1) << "Generator for <" << resource.id << "> not ready.";
}

void SurfaceDem::prepare_impl()
{
    LOG(info2) << "Preparing <" << resource().id << ">.";

    const auto &r(resource());

    // try to open datasets
    auto dataset(geo::GeoDataset::open(dataset_));
    auto datasetMin(geo::GeoDataset::open(datasetMin_));
    auto datasetMax(geo::GeoDataset::open(datasetMax_));

    // build properties
    properties_ = {};
    properties_.id = r.id.fullId();
    properties_.referenceFrame = r.referenceFrame->id;
    properties_.credits = asIntSet(r.credits);
    if (definition_.textureLayerId) {
        properties_.boundLayers.insert(definition_.textureLayerId);
    }
    // position ???
    // keep driverOptions empty -> no driver
    properties_.lodRange = r.lodRange;
    properties_.tileRange = r.tileRange;

    // grab and reset tile index
    auto &ti(index_.tileIndex);
    ti = {};

    // build tile index
    // metatiles are distributed everywhere
    for (vts::Lod lod(0); lod <= r.lodRange.max; ++lod) {
        // treat whole lod as a huge metatile and process each block
        // independently; metatiles are set in all (even invalid) nodes
        for (const auto &block
                 : metatileBlocks(*resource().referenceFrame
                                  , vts::TileId(lod), lod, true))
        {
            LOG(info1) << "Generating tile index LOD <" << lod
                       << ">: ancestor: "
                       << block.commonAncestor.nodeId()
                       << "block: " << block.view << ".";

            // this is not that correct since metatiles have different spatial
            // distribution than subtrees but it is OK since we always use
            // proper metatile ID when accessing metatile
            vts::TileIndex::Flag::value_type flags
                (vts::TileIndex::Flag::meta);

            if (block.valid() && in(lod, r.lodRange)) {
                // mesh and navtile in valid area (If there are non-existent
                // tiles we'll get empty meshes and navtiles with empty masks.
                // This is lesser evil than to construct gargantuan tileindex
                // that would not fit in any imaginable memory)
                flags |= (vts::TileIndex::Flag::mesh
                          | vts::TileIndex::Flag::navtile);

                if (!block.partial()) {
                    // fully covered block -> watertight tiles
                    flags |= vts::TileIndex::Flag::watertight;
                }
            }

            // set current block to computed value
            ti.set(lod, block.view, flags);
        }
    }

    // save it all
    vts::tileset::saveConfig(filePath(vts::File::config), properties_);
    vts::tileset::saveTileSetIndex(index_, filePath(vts::File::tileIndex));
}

vts::MapConfig SurfaceDem::mapConfig_impl(ResourceRoot root)
    const
{
    auto mc(vts::mapConfig
            (properties_, vts::ExtraTileSetProperties()
             , prependRoot(fs::path(), resource(), root)));

    // look down
    mc.position.orientation = { 0.0, -90.0, 0.0 };

    // take Y size of reference frame's 3D extents extents
    mc.position.verticalExtent
        = math::size(referenceFrame().division.extents).height;

    // quite wide angle camera
    mc.position.verticalFov = 90;

    return mc;
}

namespace {

typedef vts::MetaNode::Flag MetaFlag;
typedef vts::TileIndex::Flag TiFlag;

inline MetaFlag::value_type ti2metaFlags(TiFlag::value_type ti)
{
    MetaFlag::value_type meta(MetaFlag::allChildren);
    if (ti & TiFlag::mesh) {
        meta |= MetaFlag::geometryPresent;
    }
    if (ti & TiFlag::navtile) {
        meta |= MetaFlag::navtilePresent;
    }

    return meta;
}
/** NB: Do Not Change!
 *
 * This constant has huge impact on dataset stability. Changing this value break
 * data already served to the outer world.
 */
const int metatileSamplesPerTile(8);

bool special(const vr::ReferenceFrame &referenceFrame
             , const vts::TileId &tileId)
{
    if (const auto *node
        = referenceFrame.find(vts::rfNodeId(tileId), std::nothrow))
    {
        switch (node->partitioning.mode) {
        case vr::PartitioningMode::manual:
            return true;
        default:
            return false;
        }
    }
    return false;
}

} // namespace

void SurfaceDem::generateMetatile(const vts::TileId &tileId
                                       , const Sink::pointer &sink
                                       , const SurfaceFileInfo &fi
                                       , GdalWarper&) const
{
    sink->checkAborted();

    auto blocks(metatileBlocks(resource(), tileId));

    if (blocks.empty()) {
        sink->error(utility::makeError<NotFound>
                    ("Metatile completely outside of configured range."));
        return;
    }

    const auto &rf(referenceFrame());

    vts::MetaTile metatile(tileId, rf.metaBinaryOrder);

    for (const auto &block : blocks) {
        const auto &view(block.view);
        const auto &extents(block.extents);
        const auto es(math::size(extents));
        const math::Size2 bSize(vts::tileRangesSize(view));

        const math::Size2 gridSize
            (bSize.width * metatileSamplesPerTile + 1
             , bSize.height * metatileSamplesPerTile + 1);

        LOG(info1) << "Processing metatile block ["
                   << vts::tileId(tileId.lod, block.view.ll)
                   << ", " << vts::tileId(tileId.lod, block.view.ur)
                   << "], ancestor: " << block.commonAncestor.nodeId()
                   << ", tile offset: " << block.offset;

        // grid (in grid coordinates); fill in with invalid numbers
        Grid<math::Point3> grid
            (gridSize, math::Point3(std::numeric_limits<double>::quiet_NaN()));

        // grid mask
        const ShiftMask mask(block, metatileSamplesPerTile);

        // tile size in grid and in real SDS
        math::Size2f gts
            (es.width / (metatileSamplesPerTile * bSize.width)
             , es.height / (metatileSamplesPerTile * bSize.height));
        math::Size2f ts(es.width / bSize.width
                        , es.height / bSize.height);

        auto conv(sds2phys(block.commonAncestor, definition_.geoidGrid));
        auto navConv(sds2nav(block.commonAncestor, definition_.geoidGrid));

        // fill in matrix
        for (int j(0), je(gridSize.height); j < je; ++j) {
            auto y(extents.ur(1) - j * gts.height);
            for (int i(0), ie(gridSize.width); i < ie; ++i) {
                // work only with non-masked pixels
                if (mask(i, j)) {
                    grid(i, j)
                        = conv(math::Point3
                               (extents.ll(0) + i * gts.width, y, 0.0));
                }
            }
        }

        // generate metatile content
        for (int j(0), je(bSize.height); j < je; ++j) {
            for (int i(0), ie(bSize.width); i < ie; ++i) {
                // ID of current tile
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                // build metanode
                vts::MetaNode node;
                node.flags(ti2metaFlags(index_.tileIndex.get(nodeId)));
                bool geometry(node.geometry());
                bool navtile(node.navtile());

                // compute tile extents and height range
                auto heightRange(vs::Range<double>::emptyRange());
                math::Extents3 te(math::InvalidExtents{});
                double area(0);
                int triangleCount(0);

                // process all node's vertices in grid
                for (int jj(0); jj <= metatileSamplesPerTile; ++jj) {
                    auto yy(j * metatileSamplesPerTile + jj);
                    for (int ii(0); ii <= metatileSamplesPerTile; ++ii) {
                        auto xx(i * metatileSamplesPerTile + ii);
                        const auto *p(grid(mask, xx, yy));

                        // update tile extents (if point valid)
                        if (p) { math::update(te, *p); }

                        if (geometry && ii && jj) {
                            // compute area of the quad composed of 1 or 2
                            // triangles
                            auto qa(quadArea(grid(mask, xx - 1, yy - 1)
                                             , p
                                             , grid(mask, xx - 1, yy)
                                             , grid(mask, xx, yy - 1)));
                            area += std::get<0>(qa);
                            triangleCount += std::get<1>(qa);
                        }

                        if (p && navtile) {
                            // sample height in navtile
                            auto z(navConv
                                   (math::Point3
                                    (extents.ll(0) + xx * gts.width
                                     , yy, 0.0))(2));
                            update(heightRange, z);
                        }
                    }
                }

                if (block.commonAncestor.partial() || special(rf, nodeId)) {
                    // partial node, update children flags
                    for (const auto &child : vts::children(nodeId)) {
                        node.setChildFromId
                            (child, vts::NodeInfo(rf, child).valid());
                    }
                }

                if (geometry && !area) {
                    // well, empty tile, no children
                    continue;
                }

                // set extents
                node.extents = vr::normalizedExtents(rf, te);

                // build height range
                node.heightRange.min = std::floor(heightRange.min);
                node.heightRange.max = std::ceil(heightRange.max);

                // set credits
                node.updateCredits(resource().credits);

                // mesh is (almost) flat -> use tile area
                if (geometry) {
                    node.applyTexelSize(true);

                    // calculate texture size using node mask
                    auto textureArea([&]() -> double
                    {
                        // ancestor is full -> we are full as well
                        if (!block.commonAncestor.partial()) {
                            return vr::BoundLayer::tileArea();
                        }

                        // partial node: use triangle count to calculate
                        // percentage of texture
                        math::Size2 size(metatileSamplesPerTile
                                         , metatileSamplesPerTile);

                        // return scaled coverage; NB: triangle covers hald of
                        // pixel so real area is in pixels is half of number of
                        // pixels
                        return ((triangleCount * vr::BoundLayer::tileArea())
                                / (2.0 * math::area(size)));
                    }());

                    // well, empty tile as well
                    if (!textureArea) { continue; }

                    // calculate texel size
                    node.texelSize = std::sqrt(area / textureArea);
                }

                // store metata node
                metatile.set(nodeId, node);
            }
        }
    }

    // write metatile to stream
    std::ostringstream os;
    metatile.save(os);
    sink->content(os.str(), fi.sinkFileInfo());
}

void SurfaceDem::generateMesh(const vts::TileId &tileId
                                   , const Sink::pointer &sink
                                   , const SurfaceFileInfo &fi
                                   , GdalWarper&) const
{
    // TODO: calculate tile sampling
    const int samplesPerSide(128);
    const int facesPerTile(1500);

    sink->checkAborted();

    const auto &rf(referenceFrame());

    if (!index_.tileIndex.real(tileId)) {
        sink->error(utility::makeError<NotFound>("No mesh for this tile."));
        return;
    }

    vts::NodeInfo nodeInfo(rf, tileId);
    if (!nodeInfo.valid()) {
        sink->error(utility::makeError<NotFound>
                    ("TileId outside of valid reference frame tree."));
        return;
    }

    // generate mesh
    auto meshInfo
        (meshFromNode
         (nodeInfo, math::Size2(samplesPerSide, samplesPerSide)));
    auto &lm(std::get<0>(meshInfo));

    // simplify
    simplifyMesh(lm, nodeInfo, facesPerTile);

    // and add skirt
    addSkirt(lm, nodeInfo);

    // generate VTS mesh
    vts::Mesh mesh;
    auto &sm(addSubMesh(mesh, lm, nodeInfo, definition_.geoidGrid));
    if (definition_.textureLayerId) {
        sm.textureLayer = definition_.textureLayerId;
    }

    if (fi.raw) {
        // we are returning full mesh file -> generate coverage mask
        meshCoverageMask
            (mesh.coverageMask, lm, nodeInfo, std::get<1>(meshInfo));
    }

    // write mesh (only mesh!) to stream
    std::ostringstream os;
    if (fi.raw) {
        vts::saveMesh(os, mesh);
    } else {
        vts::saveMeshProper(os, mesh);
    }

    sink->content(os.str(), fi.sinkFileInfo());
}

void SurfaceDem::generateNavtile(const vts::TileId &tileId
                                      , const Sink::pointer &sink
                                      , const SurfaceFileInfo &fi
                                      , GdalWarper&) const
{
    sink->checkAborted();

    const auto &rf(referenceFrame());

    if (!index_.tileIndex.navtile(tileId)) {
        sink->error(utility::makeError<NotFound>("No navtile for this tile."));
        return;
    }

    vts::NodeInfo nodeInfo(rf, tileId);
    if (!nodeInfo.valid()) {
        sink->error(utility::makeError<NotFound>
                    ("TileId outside of valid reference frame tree."));
        return;
    }

    const auto &extents(nodeInfo.extents());
    const auto ts(math::size(extents));

    // sds -> navigation SRS convertor
    auto navConv(sds2nav(nodeInfo, definition_.geoidGrid));

    // first, calculate height range in the same way as is done in metatile
    auto heightRange(vs::Range<double>::emptyRange());
    {
        // create node doverage
        const auto coverage(nodeInfo.coverageMask
                            (vts::NodeInfo::CoverageType::grid
                             , math::Size2(metatileSamplesPerTile + 1
                                           , metatileSamplesPerTile + 1), 1));
        // grid pixel size
        math::Size2f gpx
            (ts.width / (metatileSamplesPerTile + 1)
             , ts.height / (metatileSamplesPerTile + 1));
        for (int j(0); j <= metatileSamplesPerTile; ++j) {
            auto y(extents.ll(1) + j * gpx.height);
            for (int i(0); i <= metatileSamplesPerTile; ++i) {
                if (!coverage.get(i, j)) { continue; }
                auto z(navConv
                   (math::Point3
                    (extents.ll(0) + i * gpx.width, y, 0.0))(2));
                update(heightRange, z);
            }
        }
    }

    // calculate navtile values
    vts::opencv::NavTile nt;
    auto ntd(nt.data());
    // generate coverage mask in grid coordinates
    auto &coverage(nt.coverageMask() = nodeInfo.coverageMask
                   (vts::NodeInfo::CoverageType::grid
                    , math::Size2(ntd.cols, ntd.rows), 1));

    // set height range
    nt.heightRange(vts::NavTile::HeightRange
                   (std::floor(heightRange.min), std::ceil(heightRange.max)));
    math::Size2f npx(ts.width / ntd.cols, ts.height / ntd.rows);
    for (int j(0); j < ntd.rows; ++j) {
        auto y(extents.ll(1) + j * npx.height);
        for (int i(0); i < ntd.cols; ++i) {
            // mask with node's mask
            if (!coverage.get(i, j)) { continue; }
            auto z(navConv
                   (math::Point3
                    (extents.ll(0) + i * npx.width, y, 0.0))(2));
            // write
            ntd.at<vts::opencv::NavTile::DataType>(j, i) = z;
        }
    }

    // done
    std::ostringstream os;
    if (fi.raw) {
        // raw navtile -> serialize to on-disk format
        nt.serialize(os);
    } else {
        // just navtile itself
        nt.serializeNavtileProper(os);
    }

    sink->content(os.str(), fi.sinkFileInfo());
}

} // namespace generator