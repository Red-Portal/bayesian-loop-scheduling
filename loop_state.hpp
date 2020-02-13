
#ifndef _BOSCHED_LOOP_STATE_HPP_
#define _BOSCHED_LOOP_STATE_HPP_

#include "metrics.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <vector>
#include <optional>

namespace bosched
{
    struct loop_state_t
    {
        double param;
        size_t num_tasks;
        double eval_param;
        std::vector<double> obs_x;
        std::vector<double> obs_y;
        //std::atomic<double> mean_us;
        bool warming_up;
        bosched::time_point_t start;
        size_t iteration;
	nlohmann::json loop_json;

        inline bosched::time_point_t
        loop_start() noexcept
        {
            start = bosched::clock::now();
            return start;
        }

        template<typename Duration>
        inline Duration
        loop_stop() const noexcept
        {
            return std::chrono::duration_cast<Duration>
                (bosched::clock::now() - start);
        }
    };
}

#endif
