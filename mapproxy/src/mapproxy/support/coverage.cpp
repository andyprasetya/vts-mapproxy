#include <opencv2/highgui/highgui.hpp>

#include "./coverage.hpp"

namespace {

template <typename T>
T applyShift(T value, int shift)
{
    if (shift >= 0) {
        return value << shift;
    }
    return value >> -shift;
}

} // namespace

vts::NodeInfo::CoverageMask
generateCoverage(const int size, const vts::NodeInfo &nodeInfo
                 , const MaskTree &maskTree
                 , vts::NodeInfo::CoverageType type)
{
    // get coverage mask from rf node
    // size is in pixels, when generating in grid: +1
    // dilate by one pixel to make mask sane
    const math::Size2 gridSize
        (size + (type == vts::NodeInfo::CoverageType::grid)
         , size + (type == vts::NodeInfo::CoverageType::grid));
    // dilate by 1
    auto coverage(nodeInfo.coverageMask(type, gridSize, 1));

    if (!maskTree) {
        // no mask to apply
        return coverage;
    }

    // number of bits of detail derived from maximum dimension
    const int detail(int(std::ceil(std::log2(size))));

    // tile size in pixels
    const int ws(1 << detail);

    // margin added around mask (2 by default, reset to 0 if
    const int margin([&]() -> int
    {
        if ((type == vts::NodeInfo::CoverageType::pixel) && (size == ws)) {
            // pixel to pixel match -> no need to inflate by margin
            return 0;
        }
        // 2 pixel margin to handle scaling
        return 2;
    }());

    const int ts(ws + 2 * margin);

    // rasterize mask tree that it covers current tile and half of neighbours
    // get get tile ID
    auto tileId(nodeInfo.nodeId());

    // make detailed tile ID
    tileId.lod += detail;
    tileId.x <<= detail;
    tileId.y <<= detail;

    // apply margin
    tileId.x -= margin;
    tileId.y -= margin;

    // clip sampling depth
    int depth(std::min(int(tileId.lod), int(maskTree.depth())));

    // scale in x and y coordinates
    const double scale(double(ws) / double(size));

    cv::Mat tile(ts, ts, CV_8UC1, cv::Scalar(0x00));
    {
        // bit shift; NB: can be negative!
        int shift(maskTree.depth() - depth);

        // setup constraints
        MaskTree::Constraints con(depth);
        con.extents.ll(0) = applyShift(tileId.x, shift);
        con.extents.ll(1) = applyShift(tileId.y, shift);
        con.extents.ur(0) = applyShift(tileId.x + ts, shift);
        con.extents.ur(1) = applyShift(tileId.y + ts, shift);

        cv::Scalar white(0xff);
        cv::Rect tileBounds(0, 0, ts, ts);
        auto draw([&](MaskTree::Node node, boost::tribool value)
        {
            // black -> nothing
            if (!value) { return; }

            // update to match level grid
            node.shift(shift);

            node.x -= tileId.x;
            node.y -= tileId.y;

            // construct rectangle and intersect it with bounds
            //
            // NB: dilate by 1 destination pixel (i.e. scale source pixels)
            int x1(std::round(node.x - scale));
            int y1(std::round(node.y - scale));
            int x2(std::round(node.x + node.size + scale));
            int y2(std::round(node.y + node.size + scale));
            cv::Rect r(x1, y1, x2 - x1, y2 - y1);
            auto rr(r & tileBounds);
            cv::rectangle(tile, rr, white, CV_FILLED, 4);
        });

        maskTree.forEachQuad(draw, con);
    }

    // no margin -> pixel-to-pixel match: intersect and return
    if (!margin) {
        // process rendered mask and unset all invalid points
        for (int j(0); j < gridSize.height; ++j) {
            for (int i(0); i < gridSize.width; ++i) {
                if (!tile.at<std::uint8_t>(j, i)) {
                    coverage.set(i, j, false);
                }
            }
        }
        return coverage;
    }

    // shift in x and y coordinates; shift by half of src pixel to convert from
    // grid position to pixel index
    const double shift((type == vts::NodeInfo::CoverageType::pixel)
                       ? margin : (-0.5 * scale + margin));

    // clamp to tile limits
    const int clampMax(ts - 1);
    auto clamp([clampMax](int i) -> int
    {
        if (i < 0) { return 0; }
        if (i > clampMax) { return clampMax; }
        return i;
    });

    // kernel radius is a half of scale
    const double kr(scale / 2.0);

    // scan pixels around sample; both ends of kernel are inclusive
    auto scan([&](double px, double py) -> bool
    {
        for (int y(clamp(std::floor(py - kr)))
                 , ey(clamp(std::ceil(py + kr)));
             y <= ey; ++y)
        {
            for (int x(clamp(std::floor(px - kr)))
                     , ex(clamp(std::ceil(px + kr)));
                 x <= ex; ++x)
            {
                if (!tile.at<std::uint8_t>(y, x)) {
                    return false;
                }
            }
        }
        return true;
    });

    // transform point from destination raster to input raster
    // NB: (0, 1) and (1, 0) are considered to be zero
    auto trans([&](int p)
    {
       return scale * p + shift;
    });

    // process whole output space and unset invalid pixels
    for (int j(0); j < gridSize.height; ++j) {
        auto jj(trans(j));
        for (int i(0); i < gridSize.width; ++i) {
            if (!scan(trans(i), jj)) {
                coverage.set(i, j, false);
            }
        }
    }

    return coverage;
}
