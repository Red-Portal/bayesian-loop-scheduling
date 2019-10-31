
#include <iostream>

#include "loop_state.hpp"
#include "metrics.hpp"
#include "state_io.hpp"
#include "tls.hpp"
#include "utils.hpp"
#include "performance.hpp"
#include "profile.hpp"
#include "param.hpp"

#include <atomic>
#include <blaze/Blaze.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>
#include <unordered_map>

extern char const* __progname;

bool _show_loop_stat = false;
bool _is_debug       = false;
bool _is_bo_schedule = false;
bool _is_new_file    = false;
bool _profile_loop   = false;
bool _is_eval        = false;
std::mt19937 _rng __attribute__((init_priority(101)));
std::unordered_map<size_t, bosched::loop_state_t> _loop_states __attribute__((init_priority(101)));
nlohmann::json _stats   __attribute__((init_priority(101)));
nlohmann::json _profile __attribute__((init_priority(101)));
std::unordered_map<size_t, bosched::workload_params> _params __attribute__((init_priority(101)));
long _procs;
size_t _profile_count = 0;

namespace bosched
{
    inline double
    warmup_next_param()
    {
        static std::uniform_real_distribution<double> dist(0.0, 1.0);
        double next = dist(_rng);
        return next;
    }

    inline double
    generate_parameter(std::vector<double> const& weight,
		       std::vector<double> const& mu,
		       std::vector<double> const& sigma)
    {
	auto mixdist = std::discrete_distribution<size_t>(weight.begin(),
							  weight.end());
	auto mixidx  = mixdist(_rng);
	auto dist    = std::normal_distribution(mu[mixidx], sigma[mixidx]);
	return dist(_rng);
    }
}

extern "C"
{
    void __attribute__ ((constructor(65535)))
    bo_load_data()
    {
        std::ios_base::Init();

        using namespace std::literals::string_literals;
        auto progname = std::string(__progname);

        std::random_device seed;
        _rng = std::mt19937(seed());

        auto file_name = ".bostate."s + progname;
        std::ifstream stream(file_name + ".json"s);

        if(getenv("DEBUG"))
        {
            _is_debug = true;
        }
        if(getenv("LOOPSTAT"))
        {
            _show_loop_stat = true;
            auto stat_file_name = ".stat."s + progname;
            auto stat_stream = std::ifstream(stat_file_name + ".json"s);
            if(stat_stream)
                stat_stream >> _stats; 
            stat_stream.close();
        }
        if(getenv("PROFILE"))
        {
            _profile_loop = true;
            auto prof_stream = std::ifstream(".workload.json"s);
            if(prof_stream)
                prof_stream >> _profile; 
            prof_stream.close();
        }

        if(stream)
        {
            _is_new_file = false;
            auto data = nlohmann::json();
            stream >> data;
            _loop_states = bosched::read_loops(data);
        }
        else
        {
            _is_new_file = true;
        }
        stream.close();

        {
            nlohmann::json raw_params;
            auto param_stream = std::ifstream(".params.json"s);
            if(param_stream)
            {
                param_stream >> raw_params;
                _params = bosched::load_workload_params(raw_params);
            }
            param_stream.close();
        }

        if(getenv("EVAL"))
        {
            _is_eval = true;
        }
    }

    void __attribute__ ((destructor))
    bo_save_data()
    {
        using namespace std::literals::string_literals;

        auto progname = std::string(__progname);
        if(_show_loop_stat)
        {
            auto stat_file_name = ".stat."s + progname;
            auto stat_stream = std::ofstream(stat_file_name + ".json"s);
            stat_stream << _stats.dump(2); 
            stat_stream.close();
        }

        if(_is_eval)
            return;

        if(_profile_loop)
        {
            auto prof_stream = std::ofstream(".workload.json"s);
            prof_stream << _profile.dump(2); 
            prof_stream.close();
        }

        auto next = bosched::write_loops(std::move(_loop_states));

        auto file_name = ".bostate."s + progname;
        auto stream = std::ofstream(file_name + ".json"s);
        stream << next.dump(2); 
        stream.close();
    }

    void bo_workload_profile_start(long iteration)
    {
        if(_profile_loop && _profile_count < 4)
            prof::iteration_profile_start(iteration);
    }

    void bo_workload_profile_stop()
    {
        if(_profile_loop && _profile_count < 4)
            prof::iteration_profile_stop();
    }

    void bo_record_iteration_start()
    {
        if(__builtin_expect (_show_loop_stat == false, 1))
            return;
        stat::iteration_start_record();
    }

    void bo_record_iteration_stop()
    {
        if(__builtin_expect (_show_loop_stat == false, 1))
            return;
        stat::iteration_stop_record();
    }

    void bo_binlpt_load_loop(unsigned long long region_id,
                             unsigned** task_map)
    {
        auto& profile = _params[region_id].binlpt;
        *task_map = profile.data();
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested workload binlpt profile"
                      << std::endl;
        }
    }

    void bo_hss_load_loop(unsigned long long region_id,
                          unsigned** task_map)
    {
        auto& profile = _params[region_id].hss;
        *task_map = profile.data();
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested workload hss profile"
                      << std::endl;
        }
    }

    double bo_fss_parameter(unsigned long long region_id)
    {
        double param = _params[region_id].fss;
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested fss schedule parameter " << param
                      << std::endl;
        }
        return param;
    }

    double bo_css_parameter(unsigned long long region_id)
    {
        double param = _params[region_id].css;
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested css schedule parameter " << param
                      << std::endl;
        }
        return param;
    }

    double bo_tss_parameter(unsigned long long region_id)
    {
        double param = _params[region_id].tss.value();
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested tss schedule parameter " << param
                      << std::endl;
        }
        return param;
    }

    double bo_tape_parameter(unsigned long long region_id)
    {
        double param = _params[region_id].tape.value();
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested schedule parameter " << param
                      << std::endl;
        }
        return param;
    }

    double
    bo_schedule_parameter(unsigned long long region_id,
                          int is_bo_schedule)
    {
        auto& loop_state = _loop_states[region_id];
        _is_bo_schedule = static_cast<bool>(is_bo_schedule);

        if(_is_new_file)
            loop_state.warming_up = true;
        
	if(is_bo_schedule)
	{
	    if(_is_eval)
	    {
		loop_state.param = loop_state.eval_param;
	    }
	    else if(loop_state.warming_up)
	    {
		loop_state.param = bosched::warmup_next_param();
	    }
	    else
	    {
		loop_state.param = bosched::generate_parameter(
		    loop_state.gmm_weight,
		    loop_state.gmm_mean,
		    loop_state.gmm_stddev);
	    }
	}
	double param = loop_state.param;

        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id
                      << " requested schedule parameter " << param
                      << std::endl;
        }
        return param;
    }
    
    void bo_schedule_begin(unsigned long long region_id,
                           unsigned long long N,
                           long procs)
    {
        if(_is_bo_schedule || _show_loop_stat)
        {
            _loop_states[region_id].loop_start();
            _loop_states[region_id].num_tasks = N;
            _procs = procs;
        }

        if(__builtin_expect(_profile_loop, false))
        {
            if(_profile_count < 4)
                prof::profiling_init(N);
        }

        if(__builtin_expect (_show_loop_stat, false))
        {
            stat::init_tls();
        }
        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id << " starting execution."
                      << " iterations: " << N
                      << std::endl;
        }
    }

    void bo_schedule_end(unsigned long long region_id)
    {
        if(_is_bo_schedule || _show_loop_stat)
        {
            using time_scale_t = bosched::microsecond;
            auto& loop_state = _loop_states[region_id];
            auto duration = loop_state.loop_stop<time_scale_t>();
            if(_is_bo_schedule)
            {
		loop_state.obs_x.push_back(loop_state.param);
		loop_state.obs_y.push_back(duration.count());
	    }

	    if(__builtin_expect (_show_loop_stat, false))
            {
                auto work_time = std::chrono::duration_cast<time_scale_t>(
                    stat::total_work()).count();

                auto parallel_time = duration.count();

                auto work_per_processor = stat::work_per_processor();
                auto performance        = work_time / parallel_time;
                auto cost               = parallel_time * _procs;
                auto effectiveness      = performance / cost;
                auto cov                = bosched::coeff_of_variation(work_per_processor);
                auto slowdown           = bosched::slowdown(work_per_processor);

                auto& log = _stats[std::to_string(region_id)];
                log["num_tasks"    ].push_back(loop_state.num_tasks);
                log["work_time"    ].push_back(work_time);
                log["parallel_time"].push_back(parallel_time);
                log["effectiveness"].push_back(effectiveness);
                log["performance"  ].push_back(performance);
                log["task_mean"    ].push_back(work_time / loop_state.num_tasks);
                log["slowdown"     ].push_back(slowdown);
                log["cov"          ].push_back(cov);
                log["cost"         ].push_back(cost);
                if(_is_debug)
                {
                    std::cout << "-- loop " << region_id << " stats \n"
                              << log.dump(2) << '\n' << std::endl;
                }
            }
        }

        if(__builtin_expect(_profile_loop, false))
        {
            if(_profile_count <= 4)
            {
                auto workload_profile = prof::load_profile();
                _profile[std::to_string(region_id)].emplace_back(workload_profile);
                ++_profile_count;
            }
        }

        if(__builtin_expect (_is_debug, false))
        {
            std::cout << "-- loop " << region_id << " ending execution" << std::endl;
        }
    }
}


