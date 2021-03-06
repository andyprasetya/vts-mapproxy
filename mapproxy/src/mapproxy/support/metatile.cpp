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

#include <deque>
#include <set>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "utility/raise.hpp"

#include "imgproc/fillrect.hpp"

#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/tileop.hpp"

#include "../error.hpp"
#include "./metatile.hpp"

// Uncomment to get debug information for metatile block generation
//#define DEBUG_META_BLOCKS 1

MetatileBlock::MetatileBlock(vts::Lod lod
                             , const vr::ReferenceFrame &referenceFrame
                             , const std::string &srs
                             , const vts::TileRange &view
                             , const math::Extents2 &extents)
    : srs(srs), view(view), extents(extents)
    , commonAncestor(referenceFrame, vts::commonAncestor(lod, view))
    , offset(vts::local(commonAncestor.nodeId().lod
                        , vts::tileId(lod, view.ll)))
{}

MetatileBlock::list metatileBlocksImpl(const vr::ReferenceFrame &referenceFrame
                                       , const vts::TileId &tileId
                                       , unsigned int metaBinaryOrder
                                       , bool includeInvalid
                                       , const vts::TileRange &tileRange)
{
    if (!metaBinaryOrder) {
        // no override, use order from reference frame
        metaBinaryOrder = referenceFrame.metaBinaryOrder;
    }

    const math::Size2 metaSize(1 << metaBinaryOrder, 1 << metaBinaryOrder);
    const unsigned int metaMask(~(metaSize.width - 1));

    if (((tileId.x & metaMask) != tileId.x)
        || ((tileId.y & metaMask) != tileId.y))
    {
        throw utility::makeError<http::NotFound>
            ("TileId doesn't point to metatile origin.");
    }

    // generate tile range (inclusive!)
    vts::TileRange tr(tileId.x, tileId.y
                      , tileId.x + metaSize.width - 1
                      , tileId.y + metaSize.height - 1);

    // get maximum tile index at this lod
    auto maxIndex(vts::tileCount(tileId.lod) - 1);

    // and clip
    if (tr.ur(0) > maxIndex) { tr.ur(0) = maxIndex; }
    if (tr.ur(1) > maxIndex) { tr.ur(1) = maxIndex; }

    // calculate overlap
    auto view(vts::tileRangesIntersect(tileRange, tr, std::nothrow));

    // no overlap -> bail out
    if (!math::valid(view)) { return {}; }

    auto llId(vts::tileId(tileId.lod, view.ll));
    auto urId(vts::tileId(tileId.lod, view.ur));

    // grab nodes at opposite sides
    vts::NodeInfo llNode(referenceFrame, llId);
    vts::NodeInfo urNode(referenceFrame, urId);

    if (llNode.subtree() == urNode.subtree()) {
        // whole range resides under same subtree
        // compose extents
        math::Extents2 extents
            (llNode.extents().ll(0), urNode.extents().ll(1)
             , urNode.extents().ur(0), llNode.extents().ur(1));

        // done
        return {
            MetatileBlock
                (tileId.lod, referenceFrame, llNode.srs(), view, extents)
        };
    }

    MetatileBlock::list blocks;

    // seed queue of nodes to inspect with ll node
    std::deque<vts::NodeInfo> queue(1, llNode);

    // tile index
    typedef math::Point2_<unsigned int> Index;

    // set of already seen nodes
    std::set<Index> seen;

    // push tile
    auto push([&](unsigned int x, unsigned int y)
    {
        // check if this tile has been already seen
        if (!seen.insert(Index(x, y)).second) { return; }

        if ((x > view.ur(0)) || (y > view.ur(1))) { return; }

#if DEBUG_META_BLOCKS
        LOG(info4) << "push(" << x << ", " << y << ")";
#endif

        // push node, masked node is not invalidated
        queue.push_back(vts::NodeInfo
                        (referenceFrame, vts::TileId(tileId.lod, x, y)
                         , false));
    });

    // process nodes in the queue until empty
    while (!queue.empty()) {
        // pop
        const auto node(queue.front());
        queue.pop_front();

#if DEBUG_META_BLOCKS
        LOG(info4) << "Processing " << node.nodeId() << ".";
#endif

        // grab stuff
        const auto &rootId(node.subtree().id());

#if DEBUG_META_BLOCKS
        LOG(info4) << "rootId " << rootId << ".";
        LOG(info4) << "rootId.lod: " << rootId.lod;
        LOG(info4) << "tileId.lod: " << tileId.lod;
#endif

        // compute tile range covered by root at current lod
        auto blockRange(vts::childRange
                        (vts::TileRange(rootId.x, rootId.y, rootId.x, rootId.y)
                         , tileId.lod - rootId.lod));

#if DEBUG_META_BLOCKS
        LOG(info4) << "blockRange: " << blockRange;
#endif

        // now, clip it by view
        auto blockView(vts::tileRangesIntersect(view, blockRange));

        auto blockUrId(vts::tileId(tileId.lod, blockView.ur));
        vts::NodeInfo blockUrNode(referenceFrame, blockUrId
                                  , false);

        // compose extents
        math::Extents2 blockExtents
            (node.extents().ll(0), blockUrNode.extents().ll(1)
             , blockUrNode.extents().ur(0), node.extents().ur(1));

        // new block
        if (node.valid() || includeInvalid) {
            // remember block
            blocks.emplace_back(tileId.lod, referenceFrame, node.srs()
                                , blockView, blockExtents);
        }

        // remember 2 new nodes to check
        push(blockView.ll(0), blockView.ur(1) + 1); // left/bottom
        push(blockView.ur(0) + 1, blockView.ll(1)); // right/top
    }

    // done
    return blocks;
}

MetatileBlock::list metatileBlocks(const Resource &resource
                                   , const vts::TileId &tileId
                                   , unsigned int metaBinaryOrder
                                   , bool includeInvalid)
{
    return metatileBlocksImpl
        (*resource.referenceFrame, tileId, metaBinaryOrder, includeInvalid
         , vts::shiftRange
         (resource.lodRange.min, resource.tileRange, tileId.lod));
}

cv::Mat boundlayerMetatileFromMaskTree(const vts::TileId &tileId
                                       , const MaskTree &maskTree
                                       , const MetatileBlock::list &blocks)
{
    typedef vr::BoundLayer BL;

    cv::Mat metatile(BL::rasterMetatileHeight, BL::rasterMetatileWidth
                     , CV_8U, cv::Scalar(0));

    std::vector<cv::Rect> boundsList;
    for (const auto &block : blocks) {
        if (!block.valid()) { continue; }
        const auto &v(block.view);
        boundsList.emplace_back(v.ll(0) - tileId.x, v.ll(1) - tileId.y
                                , 1 + v.ur(0) - v.ll(0)
                                , 1 + v.ur(1) - v.ll(1));
    }
    if (boundsList.empty()) { return metatile; }

    // clip sampling depth
    int depth(std::min(int(tileId.lod), int(maskTree.depth())));

    // bit shift
    int shift(maskTree.depth() - depth);

    // setup constraints
    MaskTree::Constraints con(depth);
    con.extents.ll(0) = applyShift(tileId.x, shift);
    con.extents.ll(1) = applyShift(tileId.y, shift);
    con.extents.ur(0) = applyShift((tileId.x + metatile.cols), shift);
    con.extents.ur(1) = applyShift((tileId.y + metatile.rows), shift);

    const cv::Scalar available(BL::MetaFlags::available);
    const cv::Scalar watertight(BL::MetaFlags::available
                                | BL::MetaFlags::watertight);

    auto draw([&](MaskTree::Node node, boost::tribool value)
    {
        // black -> nothing
        if (!value) { return; }

        // update to match level grid
        node.shift(shift);

        node.x -= tileId.x;
        node.y -= tileId.y;

        // FIXME: is this correct?

        // construct rectangle and intersect it with all bounds
        cv::Rect r(node.x, node.y, node.size, node.size);
        for (const auto &bounds : boundsList) {
            r = r & bounds;
            if (!r.width || !r.height) { return; }
        }

        imgproc::fillRectangle(metatile, r, (value ? watertight : available));
    });

    maskTree.forEachQuad(draw, con);

    return metatile;
}
