
normal_pdf(μ, σ²) = 1/√(2π*σ²) * exp(-μ^2/(2*σ²))
normal_cdf(μ, σ²) = 1/2 * (1 + erf(μ/√(2σ²)))

function sample_ystar(η::Float64,
                      num_samples::Int64,
                      num_grid_points::Int64,
                      gp::GaussianProcesses.GPBase) 
    xgrid = rand(gp.dim, num_grid_points)
    μ, σ² = predict_y(gp, xgrid)
    σe    = exp.(gp.logNoise.value)
    σ     = sqrt.(σ²)
    σ    .= max.(σ, σe)
    ystar = Vector{Float64}(undef, num_samples)
    left  = η

    probf(x) = prod(cdf.(Normal.(μ, sqrt.(σ²)), x))
    if(probf(left) < 0.25)
        right = maximum(μ .+ 5*σ)
        while(probf(right) < 0.75)
            right += right - left
        end
        md = find_zero(x->(probf(x) - 0.5), (left, right), atol=0.01, order=0)
        q1 = find_zero(x->(probf(x) - 0.25), (left, md), atol=0.01, order=0)
        q2 = find_zero(x->(probf(x) - 0.75), (md, right), atol=0.01, order=0)
        Gθ = (q1 - q2) / (log(log(4/3)) - log(log(4)))
        Gμ = md + Gθ*log(log(2))
        @assert Gθ > 0
        ystar .= rand(Gumbel(Gμ, Gθ), num_samples)
        ystar .= max.(ystar, left .+ 5*σe)
    else
        ystar .= left + 5*σe
    end
    return ystar
end

function acquisition(x, gp::AbstractParticleGP)
    P  = length(gp.weights)
    N  = size(x, 2)
    w  = gp.weights
    η  = gp.particles
    α_res = Matrix{eltype(x)}(undef, N, P)
    Threads.@threads for i = 1:P
        μ, σ²       = predict_y(gp.gp[i], [x])
        α_res[:,i] .= α.(μ, σ², η[:,i])
    end
    res = α_res * w 
    if(N == 1)
        res = res[1]
    end
    return res
end

function acquisition(x, gp::TimeMarginalizedGP)
    nmgp  = gp.non_marg_gp
    P     = length(nmgp.weights)
    w     = nmgp.weights
    η     = nmgp.η
    α_res = zeros(P)
    t     = gp.time_idx
    Threads.@threads for i = 1:P
        xt       = zeros(2, length(t))
        xt[1,:]  = t
        xt[2,:] .= x

        μ, σ² = predict_y(nmgp.gp[i], xt)
        μ     = dot(μ, gp.time_w)
        σ²    = dot(σ², gp.time_w)

        α_res[i] = α(μ, σ², η[:,i])
    end
    res = α_res'w 
    return res
end

# @inline function α(μ, σ², η)
# """
#  "Upper Confidence Bound"
#  Niranjan Srinivas, Andreas Krause, Sham Kakade, and Matthias Seeger. 2010.
#  Gaussian process optimization in the bandit setting: no regret and
#  experimental design. ICML'10
#  Srinivas, Niranjan, et al. "Information-theoretic regret bounds for
#  gaussian process optimization in the bandit setting." IEEE Transactions
#  on Information Theory 58.5 (2012)
# """
# t = 30
#     δ = 0.1 # Tunable parameter refer to original paper
#     β = 2 * log((t * π)^2 / (6 * δ))
#     return μ + sqrt(β * σ²)
# end

@inline function α(μ, σ², η)
"""
 "MES Acquisition Function"
 Wang, Zi, and Stefanie Jegelka. "Max-value entropy search for efficient 
 Bayesian optimization." Proceedings of the 34th International Conference 
 on Machine Learning-Volume 70. JMLR. org, 2017.
"""
    dist = Normal()
    z = (η .- μ) / sqrt.(σ²)
    ϕ = pdf.(dist, z)
    Φ = cdf.(dist, z) .+ 1e-20
    return mean(z .* ϕ ./ (2 * Φ) - log.(Φ))
end
