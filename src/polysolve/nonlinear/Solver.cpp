
#include "Solver.hpp"

#include "BFGS.hpp"
#include "DenseNewton.hpp"
#include "GradientDescent.hpp"
#include "LBFGS.hpp"
#include "SparseNewton.hpp"

#include <polysolve/Utils.hpp>

#include <jse/jse.h>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bundled/color.h>
#include <spdlog/fmt/ostr.h>

#include <iomanip>
#include <fstream>

namespace polysolve::nonlinear
{

    // Static constructor
    std::unique_ptr<Solver> Solver::create(
        const json &solver_params_in,
        const json &linear_solver_params,
        const double characteristic_length,
        spdlog::logger &logger,
        const bool strict_validation)
    {
        json solver_params = solver_params_in; // mutable copy

        json rules;
        jse::JSE jse;

        jse.strict = strict_validation;
        const std::string input_spec = POLYSOLVE_NON_LINEAR_SPEC;
        std::ifstream file(input_spec);

        if (file.is_open())
            file >> rules;
        else
            log_and_throw_error(logger, "unable to open {} rules", input_spec);

        const bool valid_input = jse.verify_json(solver_params, rules);

        if (!valid_input)
            log_and_throw_error(logger, "invalid input json:\n{}", jse.log2str());

        solver_params = jse.inject_defaults(solver_params, rules);

        const std::string solver = solver_params["solver"];

        if (solver == "BFGS")
        {
            return std::make_unique<BFGS>(solver_params, linear_solver_params, characteristic_length, logger);
        }
        else if (solver == "DenseNewton" || solver == "dense_newton")
        {
            return std::make_unique<DenseNewton>(solver_params, linear_solver_params, characteristic_length, logger);
        }
        else if (solver == "Newton" || solver == "SparseNewton" || solver == "sparse_newton")
        {
            return std::make_unique<SparseNewton>(solver_params, linear_solver_params, characteristic_length, logger);
        }
        else if (solver == "GradientDescent" || solver == "gradient_descent")
        {
            return std::make_unique<GradientDescent>(solver_params, characteristic_length, logger);
        }
        else if (solver == "LBFGS" || solver == "L-BFGS")
        {
            return std::make_unique<LBFGS>(solver_params, characteristic_length, logger);
        }
        throw std::runtime_error("Unrecognized solver type: " + solver);
    }

    std::vector<std::string> Solver::available_solvers()
    {
        return {{"BFGS",
                 "DenseNewton",
                 "Newton",
                 "GradientDescent",
                 "L-BFGS"}};
    }

    Solver::Solver(const json &solver_params,
                   const double characteristic_length,
                   spdlog::logger &logger)
        : m_logger(logger), characteristic_length(characteristic_length)
    {
        TCriteria criteria = TCriteria::defaults();
        criteria.xDelta = solver_params["x_delta"];
        criteria.fDelta = solver_params["f_delta"];
        criteria.gradNorm = solver_params["grad_norm"];

        criteria.xDelta *= characteristic_length;
        criteria.fDelta *= characteristic_length;
        criteria.gradNorm *= characteristic_length;

        criteria.iterations = solver_params["max_iterations"];
        // criteria.condition = solver_params["condition"];
        this->setStopCriteria(criteria);

        use_grad_norm_tol = solver_params["line_search"]["use_grad_norm_tol"];

        first_grad_norm_tol = solver_params["first_grad_norm_tol"];

        use_grad_norm_tol *= characteristic_length;
        first_grad_norm_tol *= characteristic_length;

        set_line_search(solver_params);
    }

    double Solver::compute_grad_norm(const Eigen::VectorXd &x, const Eigen::VectorXd &grad) const
    {
        return grad.norm();
    }

    void Solver::set_line_search(const json &params)
    {
        m_line_search = line_search::LineSearch::create(params, m_logger);
        solver_info["line_search"] = params["line_search"]["method"];
    }

    void Solver::minimize(Problem &objFunc, TVector &x)
    {
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        // ---------------------------
        // Initialize the minimization
        // ---------------------------
        reset(x.size()); // place for children to initialize their fields

        m_line_search->use_grad_norm_tol = use_grad_norm_tol;

        TVector grad = TVector::Zero(x.rows());
        TVector delta_x = TVector::Zero(x.rows());

        // Set these to nan to indicate they have not been computed yet
        double old_energy = NaN;
        {
            POLYSOLVE_SCOPED_STOPWATCH("constraint set update", constraint_set_update_time, m_logger);
            objFunc.solution_changed(x);
        }

        const auto g_norm_tol = this->m_stop.gradNorm;
        this->m_stop.gradNorm = first_grad_norm_tol;

        StopWatch stop_watch("non-linear solver", this->total_time, m_logger);
        stop_watch.start();

        objFunc.save_to_file(x);

        m_logger.debug(
            "Starting {} solve f₀={:g} ‖∇f₀‖={:g} "
            "(stopping criteria: max_iters={:d} Δf={:g} ‖∇f‖={:g} ‖Δx‖={:g})",
            name(), objFunc.value(x), this->m_current.gradNorm, this->m_stop.iterations,
            this->m_stop.fDelta, this->m_stop.gradNorm, this->m_stop.xDelta);

        update_solver_info(objFunc.value(x));

        do
        {
            this->m_current.xDelta = NaN;
            this->m_current.fDelta = NaN;
            this->m_current.gradNorm = NaN;

            //////////// Energy
            double energy;
            {
                POLYSOLVE_SCOPED_STOPWATCH("compute objective function", obj_fun_time, m_logger);
                energy = objFunc.value(x);
            }

            if (!std::isfinite(energy))
            {
                this->m_status = cppoptlib::Status::UserDefined;
                m_error_code = ErrorCode::NAN_ENCOUNTERED;
                log_and_throw_error(m_logger, "[{}] f(x) is nan or inf; stopping", name());
                break;
            }

            this->m_current.fDelta = std::abs(old_energy - energy); // / std::abs(old_energy);
            old_energy = energy;
            this->m_status = checkConvergence(this->m_stop, this->m_current);
            if (this->m_status != cppoptlib::Status::Continue)
                break;

            ///////////// gradient
            {
                POLYSOLVE_SCOPED_STOPWATCH("compute gradient", grad_time, m_logger);
                objFunc.gradient(x, grad);
            }

            const double grad_norm = compute_grad_norm(x, grad);
            if (std::isnan(grad_norm))
            {
                this->m_status = cppoptlib::Status::UserDefined;
                m_error_code = ErrorCode::NAN_ENCOUNTERED;
                log_and_throw_error(m_logger, "[{}] Gradient is nan; stopping", name());
                break;
            }
            this->m_current.gradNorm = grad_norm;
            this->m_status = checkConvergence(this->m_stop, this->m_current);
            if (this->m_status != cppoptlib::Status::Continue)
                break;

            // ------------------------
            // Compute update direction
            // ------------------------
            // Compute a Δx to update the variable
            //
            compute_update_direction(objFunc, x, grad, delta_x);

            if (is_direction_descent() && grad_norm != 0 && delta_x.dot(grad) >= 0)
            {
                increase_descent_strategy();
                m_logger.debug(
                    "[{}] direction is not a descent direction (‖Δx‖={:g}; ‖g‖={:g}; Δx⋅g={:g}≥0); reverting to {}",
                    name(), delta_x.norm(), grad.norm(), delta_x.dot(grad), descent_strategy_name());
                this->m_status = cppoptlib::Status::Continue;
                continue;
            }

            const double delta_x_norm = delta_x.norm();
            if (std::isnan(delta_x_norm))
            {
                increase_descent_strategy();
                this->m_status = cppoptlib::Status::UserDefined;
                m_logger.debug("[{}] Δx is nan; reverting to {}", name(), descent_strategy_name());
                this->m_status = cppoptlib::Status::Continue;
                continue;
            }

            // Use the maximum absolute displacement value divided by the timestep,
            this->m_current.xDelta = descent_strategy == 2 ? NaN : delta_x_norm;
            this->m_status = checkConvergence(this->m_stop, this->m_current);
            if (this->m_status != cppoptlib::Status::Continue)
                break;

            // ---------------
            // Variable update
            // ---------------

            // Perform a line_search to compute step scale
            double rate = m_line_search->line_search(x, delta_x, objFunc);
            if (std::isnan(rate))
            {
                assert(this->m_status == cppoptlib::Status::Continue);

                if (descent_strategy < 2) // 2 is the max, grad descent
                {
                    increase_descent_strategy();
                    m_logger.warn(
                        "[{}] Line search failed; reverting to {}", name(), descent_strategy_name());
                    continue;
                }
                else
                {
                    assert(descent_strategy == 2);                   // failed on gradient descent
                    this->m_status = cppoptlib::Status::UserDefined; // Line search failed on gradient descent, so quit!
                    log_and_throw_error(m_logger, "[{}] Line search failed on gradient descent; stopping", name());
                }
            }

            x += rate * delta_x;

            // -----------
            // Post update
            // -----------

            set_default_descent_strategy(); // Reset this for the next iterations

            const double step = (rate * delta_x).norm();

            if (objFunc.stop(x))
            {
                this->m_status = cppoptlib::Status::UserDefined;
                m_error_code = ErrorCode::SUCCESS;
                m_logger.debug("[{}] Objective decided to stop", name());
            }

            objFunc.post_step(this->m_current.iterations, x);

            m_logger.debug(
                "[{}] iter={:d} f={:g} Δf={:g} ‖∇f‖={:g} ‖Δx‖={:g} Δx⋅∇f(x)={:g} rate={:g} ‖step‖={:g}",
                name(), this->m_current.iterations, energy, this->m_current.fDelta,
                this->m_current.gradNorm, this->m_current.xDelta, delta_x.dot(grad), rate, step);

            if (++this->m_current.iterations >= this->m_stop.iterations)
                this->m_status = cppoptlib::Status::IterationLimit;

            update_solver_info(energy);

            objFunc.save_to_file(x);

            // reset the tolerance, since in the first iter it might be smaller
            this->m_stop.gradNorm = g_norm_tol;

        } while (objFunc.callback(this->m_current, x) && (this->m_status == cppoptlib::Status::Continue));

        stop_watch.stop();

        // -----------
        // Log results
        // -----------

        if (!allow_out_of_iterations && this->m_status == cppoptlib::Status::IterationLimit)
            log_and_throw_error(m_logger, "[{}] Reached iteration limit (limit={})", name(), this->m_stop.iterations);
        if (this->m_status == cppoptlib::Status::UserDefined && m_error_code != ErrorCode::SUCCESS)
            log_and_throw_error(m_logger, "[{}] Failed to find minimizer", name());

        double tot_time = stop_watch.getElapsedTimeInSec();
        m_logger.info(
            "[{}] Finished: {} Took {:g}s (niters={:d} f={:g} Δf={:g} ‖∇f‖={:g} ‖Δx‖={:g} ftol={})",
            name(), this->m_status, tot_time, this->m_current.iterations,
            old_energy, this->m_current.fDelta, this->m_current.gradNorm, this->m_current.xDelta, this->m_stop.fDelta);

        log_times();
        update_solver_info(objFunc.value(x));
    }

    void Solver::reset(const int ndof)
    {
        this->m_current.reset();
        set_default_descent_strategy();
        m_error_code = ErrorCode::SUCCESS;

        const std::string line_search_name = solver_info["line_search"];
        solver_info = json();
        solver_info["line_search"] = line_search_name;
        solver_info["iterations"] = 0;

        reset_times();
    }

    void Solver::reset_times()
    {
        total_time = 0;
        grad_time = 0;
        assembly_time = 0;
        inverting_time = 0;
        line_search_time = 0;
        obj_fun_time = 0;
        constraint_set_update_time = 0;
        if (m_line_search)
        {
            m_line_search->reset_times();
        }
    }

    void Solver::update_solver_info(const double energy)
    {
        solver_info["status"] = this->status();
        solver_info["error_code"] = m_error_code;
        solver_info["energy"] = energy;
        const auto &crit = this->criteria();
        solver_info["iterations"] = crit.iterations;
        solver_info["xDelta"] = crit.xDelta;
        solver_info["fDelta"] = crit.fDelta;
        solver_info["gradNorm"] = crit.gradNorm;
        solver_info["condition"] = crit.condition;

        double per_iteration = crit.iterations ? crit.iterations : 1;

        solver_info["total_time"] = total_time;
        solver_info["time_grad"] = grad_time / per_iteration;
        solver_info["time_assembly"] = assembly_time / per_iteration;
        solver_info["time_inverting"] = inverting_time / per_iteration;
        solver_info["time_line_search"] = line_search_time / per_iteration;
        solver_info["time_constraint_set_update"] = constraint_set_update_time / per_iteration;
        solver_info["time_obj_fun"] = obj_fun_time / per_iteration;

        if (m_line_search)
        {
            solver_info["line_search_iterations"] = m_line_search->iterations;

            solver_info["time_checking_for_nan_inf"] =
                m_line_search->checking_for_nan_inf_time / per_iteration;
            solver_info["time_broad_phase_ccd"] =
                m_line_search->broad_phase_ccd_time / per_iteration;
            solver_info["time_ccd"] = m_line_search->ccd_time / per_iteration;
            // Remove double counting
            solver_info["time_classical_line_search"] =
                (m_line_search->classical_line_search_time
                 - m_line_search->constraint_set_update_time)
                / per_iteration;
            solver_info["time_line_search_constraint_set_update"] =
                m_line_search->constraint_set_update_time / per_iteration;
        }
    }

    void Solver::log_times()
    {
        m_logger.debug(
            "[{}] grad {:.3g}s, assembly {:.3g}s, inverting {:.3g}s, "
            "line_search {:.3g}s, constraint_set_update {:.3g}s, "
            "obj_fun {:.3g}s, checking_for_nan_inf {:.3g}s, "
            "broad_phase_ccd {:.3g}s, ccd {:.3g}s, "
            "classical_line_search {:.3g}s",
            fmt::format(fmt::fg(fmt::terminal_color::magenta), "timing"),
            grad_time, assembly_time, inverting_time, line_search_time,
            constraint_set_update_time + (m_line_search ? m_line_search->constraint_set_update_time : 0),
            obj_fun_time, m_line_search ? m_line_search->checking_for_nan_inf_time : 0,
            m_line_search ? m_line_search->broad_phase_ccd_time : 0, m_line_search ? m_line_search->ccd_time : 0,
            m_line_search ? m_line_search->classical_line_search_time : 0);
    }
} // namespace polysolve::nonlinear
