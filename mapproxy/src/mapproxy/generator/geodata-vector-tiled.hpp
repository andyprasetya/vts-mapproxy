#ifndef mapproxy_generator_geodata_vector_tiled_hpp_included_
#define mapproxy_generator_geodata_vector_tiled_hpp_included_

#include "vts-libs/vts/urltemplate.hpp"
#include "vts-libs/vts/tileset/tilesetindex.hpp"

#include "./geodatavectorbase.hpp"

namespace generator {

class GeodataVectorTiled : public GeodataVectorBase {
public:
    GeodataVectorTiled(const Config &config, const Resource &resource);

private:
    virtual void prepare_impl();
    virtual vts::MapConfig mapConfig_impl(ResourceRoot root) const;

    vr::FreeLayer freeLayer(ResourceRoot root) const;

    virtual void generateMetatile(Sink &sink
                                  , const GeodataFileInfo &fileInfo
                                  , Arsenal &arsenal) const;

    virtual void generateGeodata(Sink &sink
                                 , const GeodataFileInfo &fileInfo
                                 , Arsenal &arsenal) const;

    /** Path to /dem dataset
     */
    const std::string demDataset_;

    vts::UrlTemplate tileUrl_;

    const vr::Srs& physicalSrs_;

    vts::tileset::Index index_;
};

} // namespace generator

#endif // mapproxy_generator_geodata_vector_tiled_hpp_included_
