#pragma once
#include "LineSearch.hpp"

namespace polysolve::nonlinear::line_search
{
    class NoLineSearch : public LineSearch
    {
    public:
        using Superclass = LineSearch;
        using typename Superclass::Scalar;
        using typename Superclass::TVector;

        NoLineSearch(spdlog::logger &logger);

    protected:
        double compute_descent_step_size(
            const TVector &x,
            const TVector &delta_x,
            Problem &objFunc,
            const bool,
            const double,
            const double starting_step_size) override;
    };
} // namespace polysolve::nonlinear::line_search