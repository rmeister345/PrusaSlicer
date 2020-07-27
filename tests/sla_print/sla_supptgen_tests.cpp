#include <catch2/catch.hpp>

#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/SLA/AGGRaster.hpp>

#include "sla_test_utils.hpp"

namespace Slic3r { namespace sla {

using FloatRaster = AGGRaster<agg::pixfmt_alpha_blend_gray<agg::blender_gray32, agg::row_accessor<float>>,
                              agg::renderer_scanline_aa_solid,
                              agg::rasterizer_scanline_aa<>,
                              agg::scanline_p8>;

struct PPMFloatRasterEncoder {
    EncodedRaster operator()(const void *ptr,
                             size_t      w,
                             size_t      h,
                             size_t      num_components)
    {
        std::vector<uint8_t> buf;

        auto header = std::string("P5 ") +
                      std::to_string(w) + " " +
                      std::to_string(h) + " " + "255 ";

        auto sz = w * h * num_components;
        size_t s = sz + header.size();

        buf.reserve(s);

        std::copy(header.begin(), header.end(), std::back_inserter(buf));

        for (size_t i = 0; i < sz; ++i)
            buf.emplace_back(
                uint8_t(std::max(0., std::min(255., 255. * static_cast<const float *>(ptr)[i])))
            );

        return EncodedRaster(std::move(buf), "ppm");
    }
};

class PressureRaster: public FloatRaster
{

    template<class Op>
    PressureRaster & apply_op(const PressureRaster &other, Op &&op)
        noexcept(noexcept(op(float{}, float{})))
    {
        assert(resolution() == other.resolution());

        for (size_t i = 0; i < m_buf.size(); ++i)
            m_buf[i] = op(m_buf[i], other.m_buf[i]);

        return *this;
    }

public:
    PressureRaster(const RasterBase::Resolution &res,
                   const RasterBase::PixelDim &  pxdim)
        : FloatRaster{res,
               pxdim,
               RasterBase::Trafo{},
               FloatRaster::TColor{0.f},
               FloatRaster::TColor{1.f},
               agg::gamma_power{1.}}
    {}

    inline const float& operator()(size_t col, size_t row) const noexcept
    {
        return m_buf[row * resolution().width_px + col];
    }

    inline float& operator()(size_t col, size_t row) noexcept
    {
        return m_buf[row * resolution().width_px + col];
    }

    inline PressureRaster & operator+=(float val) noexcept
    {
        for (size_t i = 0; i < m_buf.size(); ++i) m_buf[i] += val;
        return *this;
    }

    inline PressureRaster & operator+=(const PressureRaster &other) noexcept
    {
        return apply_op(other, std::plus<float>{});
    }

    inline PressureRaster & operator*=(const PressureRaster &other) noexcept
    {
        return apply_op(other, std::multiplies<float>{});
    }

    const std::vector<float> & data() { return m_buf; }

};

double bb_width(const BoundingBoxf &bb) { return bb.max.x() - bb.min.x(); }
double bb_height(const BoundingBoxf &bb) { return bb.max.y() - bb.min.y(); }

using PressureMatrix = Eigen::MatrixXf;

class PressureModel {
    const std::vector<ExPolygons> &m_slices;
    const std::vector<float>      &m_heights;

    RasterBase::Resolution m_res;
    RasterBase::PixelDim   m_pxdim;
    Point m_bedcenter = {0, 0};

    float m_tear_foce = -2.f;

    void init_grid(PressureMatrix &rst) const
    {
        rst.setOnes();
        rst *= 10.f; // TODO: substitue grip force of the print bed
    }

    void calc_grid(size_t n, PressureMatrix &pmat) const
    {
        RasterGrayscaleAAGammaPower rst(m_res, m_pxdim,
                                        RasterBase::Trafo{}.set_center(m_bedcenter));

        for (auto &expoly : m_slices[n]) rst.draw(expoly);

//        PressureMatrix pmat_prev(m_res.width_px, m_res.height_px);
//        pmat_prev.setZero();
//        if (n > 0) {
//            RasterGrayscaleAAGammaPower rst_prev(m_res, m_pxdim,
//                                            RasterBase::Trafo{}.set_center(m_bedcenter));

//            for (auto &expoly : m_slices[n - 1]) rst_prev.draw(expoly);

//            for (int r = 0; r < int(m_res.height_px); ++r) {
//                for (int c = 0; c < int(m_res.width_px); ++c)
//                    pmat_prev(c, r) = area(m_pxdim) * -m_tear_foce *
//                                      rst_prev.read_pixel(c, r) / 255.;
//            }
//        }

        std::fstream{std::string("layer") + std::to_string(n) + ".png", std::fstream::out} << rst.encode(PNGRasterEncoder{});

//        float h = n == 0 || m_heights.empty() ? 0. : m_heights[n] - m_heights[n - 1];

        for (int r = 0; r < int(m_res.height_px); ++r) {
            for (int c = 0; c < int(m_res.width_px); ++c) {
                float mask = rst.read_pixel(c, r) / 255.;
                pmat(c, r) = mask * pmat(c, r)  /*+ pmat_prev(c, r)*/ + area(m_pxdim) * m_tear_foce * mask;
            }
        }
    }

public:

    PressureModel(const std::vector<ExPolygons> &slices,
                  const std::vector<float> &     heights,
                  const BoundingBoxf &            bed,
                  const Vec<2, size_t> & gridsize)
        : m_slices{slices}
        , m_heights{heights}
        , m_res{gridsize.x(), gridsize.y()}
        , m_pxdim{bb_width(bed) / gridsize.x(), bb_height(bed) / gridsize.y()}
        , m_bedcenter{scaled(bed.center())} {};

    PressureMatrix operator() (size_t n) const;
};

PressureMatrix PressureModel::operator() (size_t n) const
{
    PressureMatrix grid{m_res.width_px, m_res.height_px};

    init_grid(grid);

    for (size_t i = 0; i < n && i < m_slices.size(); ++i)
        calc_grid(i, grid);

    return grid;
}

//static ExPolygon square(double a, Point center = {0, 0})
//{
//    ExPolygon poly;
//    coord_t V = scaled(a / 2.);

//    poly.contour.points = {{-V, -V}, {V, -V}, {V, V}, {-V, V}};
//    poly.translate(center.x(), center.y());

//    return poly;
//}

//TEST_CASE("Float rasterizer", "[SupGen]") {
//    _RasterGrayscaleAA rst({500, 500}, {1., 1.},
//                    RasterBase::Trafo{}.set_center(scaled(Vec2d(250., 250.))),
//                    _RasterGrayscaleAA::TColor{255}, _RasterGrayscaleAA::TColor{0},
//                    agg::gamma_power{1.});

//    ExPolygon sqh = square_with_hole(50.);

//    rst.draw(sqh);

//    std::fstream{"floatrast.ppm", std::fstream::out} << rst.encode(PPMRasterEncoder{});
//}

EncodedRaster to_ppm(const PressureMatrix &pmat)
{
    std::vector<uint8_t> buf;

    auto header = std::string("P5 ") +
                  std::to_string(pmat.rows()) + " " +
                  std::to_string(pmat.cols()) + " " + "255 ";

    auto sz = size_t(pmat.cols() * pmat.rows());
    size_t s = sz + header.size();

    buf.reserve(s);

    std::copy(header.begin(), header.end(), std::back_inserter(buf));

    float cmax = pmat.maxCoeff();
    float cmin = pmat.minCoeff();
    for (size_t c = 0; c < size_t(pmat.cols()); ++c)
        for (size_t r = 0; r < size_t(pmat.rows()); ++r)
            buf.emplace_back(
                uint8_t(std::max(0., std::min(255., 255. * (pmat(r, c) - cmin) / (cmax - cmin) )))
                );

    return EncodedRaster(std::move(buf), "ppm");
}

TEST_CASE("PressureMatrix calc", "[SupGen]")
{

    TriangleMesh mesh = make_pyramid(50., 5.);
//    mesh.rotate_y(PI);
    mesh.translate(0.f, 0.f, 1.f);
    mesh.require_shared_vertices();

    // Prepare the slice grid and the slices
    std::vector<ExPolygons> slices;
    auto                    bb      = cast<float>(mesh.bounding_box());
    std::vector<float>      heights = grid(bb.min.z() + 0.1f - 1.f, bb.max.z(), 0.1f);
    slice_mesh(mesh, heights, slices, CLOSING_RADIUS, [] {});

    PressureModel pm{slices, heights, BoundingBoxf{{0., 0.}, {120.f, 68.f}}, {120, 68}};

    PressureMatrix pmat = pm(13/*slices.size()*/);

    std::fstream{"floatrast.ppm", std::fstream::out} << to_ppm(pmat);
}

}} // namespace Slic3r::sla
