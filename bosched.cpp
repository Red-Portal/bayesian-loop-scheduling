
#include <iostream>

#include "LPBO/LPBO.hpp"
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

double const _epsilon = 1e-7;

bool _show_loop_stat = false;
bool _is_debug       = false;
bool _is_bo_schedule = false;
bool _is_new_file    = false;
bool _profile_loop   = false;
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
        static std::uniform_real_distribution<double> dist(_epsilon, 1.0);
        double next = dist(_rng);
        return next;
    }

    inline void
    update_param_warmup(loop_state_t& loop_state)
    {
        if(loop_state.obs_x.size() > 20)
        {
            size_t particle_num = 10;
            double survival_rate = 0.8;

            try
            {
                loop_state.gp.emplace(loop_state.obs_x,
                                      loop_state.obs_y,
                                      particle_num,
                                      survival_rate);
            }
            catch(...)
            {
                std::cout << "-- covariance matrix singularity detected.. skipping"
                          << std::endl;
                return;
            }

            loop_state.iteration = 1;
            loop_state.warming_up = false;

            auto [next, mean, var, acq] =
                lpbo::bayesian_optimization(*loop_state.gp,
                                            _epsilon,
                                            loop_state.iteration,
                                            1000);
            loop_state.param = next;
            loop_state.pred_mean.push_back(mean);
            loop_state.pred_var.push_back(var);
            loop_state.pred_acq.push_back(acq);
        }
    }

    inline void
    update_param_non_warmup(loop_state_t& loop_state)
    {
        if(loop_state.obs_y.size() < 1)
            return;

        auto sum = std::accumulate(loop_state.obs_y.begin(),
                                   loop_state.obs_y.end(), 0.0);
        auto y_avg = sum / loop_state.obs_y.size();
        try
        { loop_state.gp->update(loop_state.param, y_avg); }
        catch(...)
        {
            std::cout << "-- covariance matrix singularity detected.. skipping"
                      << std::endl;
            return;
        }

        auto [next, mean, var, acq] =
            lpbo::bayesian_optimization(*loop_state.gp,
                                        _epsilon,
                                        loop_state.iteration,
                                        1000);
        ++loop_state.iteration;
        loop_state.pred_mean.push_back(mean);
        loop_state.pred_var.push_back(var);
        loop_state.pred_acq.push_back(acq);
        loop_state.param = next;
    }

    inline std::unordered_map<size_t, loop_state_t>
    update_loop_parameters(std::unordered_map<size_t, loop_state_t>&& loop_states)
    {
        for(auto& l : loop_states)
        {
            auto loop_id = l.first;
            auto& loop_state = l.second;

            if(loop_state.warming_up)
            {
                update_param_warmup(loop_state);

                if(_is_debug)
                {
                    std::cout << "-- warming up loop " << loop_id 
                              << " current observations: " << loop_state.obs_x.size() 
                              << std::endl;
                }
            }
            else
            {
                update_param_non_warmup(loop_state);

                if(_is_debug)
                {
                    std::cout << "-- updating GP of loop " << loop_id
                              << " next point: " << loop_state.param
                              << std::endl;
                }
            }
        }
        return std::move(loop_states);
    }

    inline void
    eval_loop_parameters(std::unordered_map<size_t, loop_state_t>& loop_states)
    {
        bool use_opt = getenv("USE_OPT") ? true : false;
        for(auto& l : loop_states)
        {
            auto loop_id = l.first;
            auto& loop_state = l.second;

            if(loop_state.warming_up)
                continue;

            auto [param, mean, var] = lpbo::find_best_mean(*loop_state.gp, _epsilon, 5000);

            auto y = loop_state.gp->data_y();
            auto best_y = std::min_element(y.begin(), y.end());
            auto best_idx = std::distance(y.begin(), best_y);
            auto best_x = loop_state.gp->data_x()[best_idx];

            loop_state.param = use_opt ? param : best_x;
                
            if(_is_debug)
            {
                auto [hist_mean, hist_var] = loop_state.gp->predict(best_x);

                std::cout << "-- evaluating mean GP of loop " << loop_id
                          << " best param: " << param
                          << " opt best param: " << param
                          << " mean: " << mean
                          << " var: " << var
                          << " hist best param: " << best_x
                          << " mean: " << hist_mean
                          << " var: " << hist_var
                          << std::endl;
            }
        }
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
            bosched::eval_loop_parameters(_loop_states);
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

        if(getenv("EVAL"))
            return;

        if(_profile_loop)
        {
            auto prof_stream = std::ofstream(".workload.json"s);
            prof_stream << _profile.dump(2); 
            prof_stream.close();
        }

        auto updated_states = _is_bo_schedule ?
            update_loop_parameters(std::move(_loop_states)) 
            : std::move(_loop_states);

        auto next = bosched::write_loops(updated_states);

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
        double param = 0.5;
        auto& loop_state = _loop_states[region_id];
        _is_bo_schedule = static_cast<bool>(is_bo_schedule);

        if(_is_new_file)
        {
            loop_state.warming_up = true;
            loop_state.iteration = 0;
            loop_state.param = param;
        }
        
        if(loop_state.warming_up && is_bo_schedule)
        {
            param = bosched::warmup_next_param();
            loop_state.param = param;
        }
        else
        {
            param = loop_state.param;
        }

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
                if( loop_state.warming_up && loop_state.obs_x.size() < 30 )
                {
                    loop_state.obs_x.push_back(loop_state.param);
                    loop_state.obs_y.push_back(duration.count());
                }
                else if(!loop_state.warming_up)
                {
                    loop_state.obs_y.push_back(duration.count());
                }
            }

            if(__builtin_expect (_show_loop_stat, false))
            {
                auto work_time = std::chrono::duration_cast<time_scale_t>(
                    stat::total_work()).count();

                auto parallel_time = duration.count();

                auto work_per_processor = stat::work_per_processor();
                auto performance        =  work_time / parallel_time;
                auto cost               = parallel_time * _procs;
                auto effectiveness      =  performance / cost;
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


