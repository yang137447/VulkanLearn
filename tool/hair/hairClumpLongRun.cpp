#include "hairClumpLongRunMath.h"
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>

namespace C3
{
using namespace C2;
using Clock = std::chrono::steady_clock;
constexpr double Step = .001;
constexpr int Planned = 13700;

std::set<int> Samples()
{
    std::set<int> ticks{1,1999,2000,2001,2999,3000,3001,3099,3100,3101,
        3199,3200,3201,3499,3500,3501,3599,3600,3601,3699,3700,3701,12700,13700};
    for (int tick = 100; tick <= Planned; tick += 100) ticks.insert(tick);
    return ticks;
}

Vector Initial(const Context& context)
{
    Vector unknown = Vector::Zero(45);
    for (int guide = 0; guide < 3; ++guide)
        for (int node = 1; node < 4; ++node)
            unknown.segment<3>(PositionIndex(guide,node,0)) = context.old[guide].positions[node] / context.length;
    return unknown;
}

double Seconds(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now()-start).count();
}

void Cost(const std::filesystem::path& directory)
{
    const Context context = MakeContext();
    const Vector unknown = Initial(context);
    const auto start = Clock::now();
    double checksum = 0;
    for (int repeat = 0; repeat < 10; ++repeat)
    {
        const Matrix first = Hessian(context,unknown,Coupling,1e-5);
        const Matrix second = Hessian(context,unknown,Coupling,5e-6);
        Eigen::SelfAdjointEigenSolver<Matrix> eig(.5*(first+first.transpose()));
        Require(eig.info()==Eigen::Success);
        checksum += eig.eigenvalues().sum() + Norm(first-second);
    }
    const double perPair = Seconds(start)/10;
    auto output = Open(directory/"cost.csv");
    output << "item,value\nH_pair_seconds," << perPair
           << "\nconservative_6_H_pairs_per_step_seconds," << perPair*6*Planned
           << "\nfinal_H_sample_count," << Samples().size()
           << "\nchecksum," << checksum << "\nadvanced_steps,0\n";
    std::cout << "C3 cost H_pair_seconds=" << perPair << " estimate_seconds=" << perPair*6*Planned << std::endl;
}

double Distance(const State& state,const State& reference,double length)
{
    double value = 0;
    for (int node=0;node<4;++node) value=std::max(value,(state.positions[node]-reference.positions[node]).norm()/length);
    return value;
}

bool Metrics(const std::filesystem::path& directory,const Context& context,
             const std::vector<std::array<State,3>>& history)
{
    const double length=std::accumulate(context.models[0].lengths.begin(),context.models[0].lengths.end(),0.0);
    auto metrics=Open(directory/"metrics.csv");
    auto summary=Open(directory/"behavior.csv");
    auto proxy=Open(directory/"sections.csv");
    auto nearest=Open(directory/"nearest.csv");
    metrics << "tick,guide,D_authored,D_snapshot,speed_over_L,static_drift,strain_min,strain_max\n";
    summary << "guide,settled,static_speed,static_drift,Dmax,Ddyn,Trec,suffix_at_least_1s,Rtail,stretch,status\n";
    proxy << "tick,section,u,p0x,p0y,p0z,p1x,p1y,p1z,p2x,p2y,p2z,centroid_x,centroid_y,centroid_z,signed_area,area_ratio,flipped,degenerate,d01,d02,d12\n";
    nearest << "tick,kind,index,guide,fixed_neighbor,fixed_distance,fixed_ratio,reselected_neighbor,reselected_distance,reselected_ratio\n";
    std::array<int,3> lastOutside{};
    bool all=true;
    for (int guide=0;guide<3;++guide)
    {
        const int off=3500+100*guide;
        lastOutside[guide]=off-1;
        double speedMax=0,driftMax=0,dynamic=0,tail=0,stretch=0;
        const double shape=Distance(history[3000][guide],history[0][guide],length);
        for (int tick=0;tick<=Planned;++tick)
        {
            const auto& state=history[tick][guide];
            const double displacement=Distance(state,history[3000][guide],length);
            double speed=0,low=0,high=0;
            for(int node=1;node<4;++node)
                if(tick) speed=std::max(speed,(state.positions[node]-history[tick-1][guide].positions[node]).norm()/Step/length);
            const double drift=Distance(state,history[2000][guide],length);
            if(tick>=2000 && tick<=3000) {speedMax=std::max(speedMax,speed);driftMax=std::max(driftMax,drift);}
            if(tick>=3000+100*guide) dynamic=std::max(dynamic,displacement);
            if(tick>=off && displacement>.005) lastOutside[guide]=tick;
            if(tick>=12700) tail=std::max(tail,displacement);
            for(int segment=0;segment<3;++segment)
            {
                const double strain=(state.positions[segment+1]-state.positions[segment]).norm()/context.models[guide].lengths[segment]-1;
                low=std::min(low,strain); high=std::max(high,strain);
            }
            stretch=std::max({stretch,-low,high});
            metrics << tick << ',' << guide << ',' << Distance(state,history[0][guide],length) << ',' << displacement
                    << ',' << speed << ',' << (tick>=2000 && tick<=3000?drift:0) << ',' << low << ',' << high << '\n';
        }
        const bool settled=speedMax<=1e-3 && driftMax<=1e-4;
        const int suffix=lastOutside[guide]+1;
        const double recovery=(suffix-off)*Step;
        const bool pass=settled && shape<=.015 && dynamic>=.02 && dynamic<=.08 &&
            suffix<=12700 && recovery<=.5 && tail<=.005 && stretch<=.01;
        all=all && pass;
        summary << guide << ',' << settled << ',' << speedMax << ',' << driftMax << ',' << shape << ',' << dynamic
                << ',' << recovery << ',' << (suffix<=12700) << ',' << tail << ',' << stretch << ',' << (pass?"PASS":"FAIL") << '\n';
    }
    auto group=Open(directory/"group.csv");
    group << "group_suffix_start_tick,recovery_from_3.7s\n"
          << std::max({3700,lastOutside[0]+1,lastOutside[1]+1,lastOutside[2]+1}) << ','
          << (std::max({3700,lastOutside[0]+1,lastOutside[1]+1,lastOutside[2]+1})-3700)*Step << '\n';
    for(int tick=0;tick<=Planned;++tick)
        for(int index=0;index<7;++index)
        {
            std::array<Vec,3> points,reference;
            const bool section=index>=4;
            const double arc=section?(index-3)*length/3:0;
            int segment=0;double fraction=0;
            if(section)
            {
                double remaining=arc;
                while(segment<2 && remaining>context.models[0].lengths[segment])
                    remaining-=context.models[0].lengths[segment++];
                fraction=std::clamp(remaining/context.models[0].lengths[segment],0.0,1.0);
            }
            for(int guide=0;guide<3;++guide)
            {
                const auto& state=history[tick][guide];
                const auto& authored=history[0][guide];
                points[guide]=section?((1-fraction)*state.positions[segment]+fraction*state.positions[segment+1]).eval():state.positions[index];
                reference[guide]=section?((1-fraction)*authored.positions[segment]+fraction*authored.positions[segment+1]).eval():authored.positions[index];
            }
            if(section)
            {
                const double area=.5*(points[1]-points[0]).cross(points[2]-points[0]).z();
                proxy << tick << ',' << index-4 << ',' << (index-3)/3.0;
                for(const auto& point:points) for(int axis=0;axis<3;++axis) proxy << ',' << point[axis];
                const Vec centroid=(points[0]+points[1]+points[2])/3;
                proxy << ',' << centroid.x() << ',' << centroid.y() << ',' << centroid.z() << ',' << area
                      << ',' << area/.00156 << ',' << (area<0) << ','
                      << (std::abs(area)<=64.0*std::numeric_limits<double>::epsilon()*.00156)
                      << ',' << (points[0]-points[1]).norm() << ',' << (points[0]-points[2]).norm()
                      << ',' << (points[1]-points[2]).norm() << '\n';
            }
            for(int guide=0;guide<3;++guide)
            {
                int fixed=-1,current=-1;
                double baselineSquare=std::numeric_limits<double>::infinity();
                double distanceSquare=std::numeric_limits<double>::infinity();
                for(int other=0;other<3;++other) if(other!=guide)
                {
                    const double original=(reference[guide]-reference[other]).squaredNorm();
                    const double actual=(points[guide]-points[other]).squaredNorm();
                    const double fixedTie=64.0*std::numeric_limits<double>::epsilon()*std::max(original,baselineSquare);
                    const double currentTie=64.0*std::numeric_limits<double>::epsilon()*std::max(actual,distanceSquare);
                    if(fixed<0 || original<baselineSquare-fixedTie) {baselineSquare=original;fixed=other;}
                    if(current<0 || actual<distanceSquare-currentTie) {distanceSquare=actual;current=other;}
                }
                const double baseline=std::sqrt(baselineSquare),distance=std::sqrt(distanceSquare);
                const double fixedDistance=(points[guide]-points[fixed]).norm();
                nearest << tick << ',' << (section?"section":"node") << ',' << (section?index-4:index) << ',' << guide
                        << ',' << fixed << ',' << fixedDistance << ',' << fixedDistance/baseline << ',' << current << ','
                        << distance << ',' << distance/(reference[guide]-reference[current]).norm() << '\n';
            }
        }
    return all;
}

int Run(const std::filesystem::path& directory)
{
    Context context=MakeContext();
    const auto samples=Samples();
    auto coverage=Open(directory/"hessian-sample-ticks.csv");
    coverage << "tick\n";for(int tick:samples) coverage << tick << '\n';coverage.flush();
    auto states=Open(directory/"states.csv");states << "kind,tick,time,guide,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    auto steps=Open(directory/"steps.csv");
    steps << "tick,time,status,iterations,chart_inf,force_scaled,torque_scaled,merit90,H_status,lambda_min,SH,H_asym,H_difference,qq_min,phi_initial,phi_final,root_x_error,root_q_error,qnorm_error,balance_scaled,Rrel_error_scaled,Urel,max_pair_error\n";
    auto directions=Open(directory/"directions.csv");directions << "tick,iteration,coordinate,z,g,p,shift\n";
    auto trials=Open(directory/"trials.csv");
    trials << "tick,iteration,backtrack,alpha,mode,base_rho,base_merit,trial_merit,full_direction_norm,base_lambda_min,base_H_asym,base_H_difference,direction_residual,stable_delta,Armijo_rhs,trial_phi,trial_chart,trial_force,trial_torque,accepted,reason,domain_valid\n";
    auto matrices=Open(directory/"hessians.csv");matrices << "case,row,column,value\n";
    auto spectra=Open(directory/"spectra.csv");spectra << "tick,kind,index,eigenvalue\n";
    auto residuals=Open(directory/"residuals.csv");residuals << "tick,coordinate,z,g,physical_residual\n";
    auto forces=Open(directory/"forces.csv");forces << "tick,guide,node,potential_fx,potential_fy,potential_fz,pair_fx,pair_fy,pair_fz,rx,ry,rz,body_tau_x,body_tau_y,body_tau_z,root_reaction_fx,root_reaction_fy,root_reaction_fz,root_reaction_tau_x,root_reaction_tau_y,root_reaction_tau_z\n";
    auto pairs=Open(directory/"pair.csv");pairs << "tick,a,b,node,ex,ey,ez,fa_x,fa_y,fa_z,fb_x,fb_y,fb_z,moment_x,moment_y,moment_z\n";
    auto energy=Open(directory/"energy.csv");
    energy << "tick,time,K_old,K_next,U_old,U_next,Urel_old,Urel_next,W0,W1,W2,damping_loss,inertial_loss,R,Rrel,Rrel_exact,Rrel_error,balance_J,balance_scale,balance_scaled\n";
    auto guideEnergy=Open(directory/"guide-energy.csv");guideEnergy << "tick,guide,U_ss,U_bt,U_G0,U_g,probe_ax\n";
    std::vector<std::array<State,3>> history{context.old};
    StateCsv(states,"initial",0,Step,context.old,context.velocity);
    Vector unknown=Initial(context);
    const auto started=Clock::now();
    int tick=1,nearrootCount=0;
    try
    {
        for(;tick<=Planned;++tick)
        {
            if(Seconds(started)>900) throw std::runtime_error("INCOMPLETE_BUDGET_WALL");
            if(tick%100==0)
            {
                std::uintmax_t bytes=0;
                for(const auto& entry:std::filesystem::directory_iterator(directory)) if(entry.is_regular_file()) bytes+=entry.file_size();
                if(bytes>6442450944ULL) throw std::runtime_error("INCOMPLETE_BUDGET_OUTPUT");
            }
            for(int guide=0;guide<3;++guide)
                context.probe[guide]=(tick>=3001+100*guide && tick<=3500+100*guide)?Vec(20,0,0):Vec::Zero();
            unknown=Initial(context);
            StateCsv(states,"old",tick-1,Step,context.old,context.velocity);
            Value last=At(context,unknown,Coupling);
            const double initial=last.phi;
            int iteration=0;
            for(;iteration<40;++iteration)
            {
                last=At(context,unknown,Coupling);
                if(Converged(last)) break;
                const Matrix raw=Hessian(context,unknown,Coupling,1e-5);
                const Matrix symmetric=.5*(raw+raw.transpose());
                Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric);
                Require(eig.info()==Eigen::Success && eig.eigenvalues().allFinite());
                const double shift=std::max(0.0,1e-6-eig.eigenvalues()[0]);
                const Matrix shifted=symmetric+shift*Matrix::Identity(45,45);
                Eigen::FullPivLU<Matrix> solve(shifted);Require(solve.isInvertible());
                const Vector direction=solve.solve(-last.gradient);Require(direction.allFinite());
                const double rho=std::max({last.gradient.lpNorm<Eigen::Infinity>(),last.forceResidual,last.torqueResidual});
                const double denominator=shifted.norm()*direction.norm()+last.gradient.norm();
                const double linear=denominator>0?(shifted*direction+last.gradient).norm()/denominator:0;
                bool nearroot=false;double asymmetry=0,difference=0;
                if(rho<=1e-8 && direction.norm()<=1e-6 && eig.eigenvalues()[0]>=0)
                {
                    const Matrix fine=Hessian(context,unknown,Coupling,5e-6);
                    const double scale=std::max(1.0,eig.eigenvalues().cwiseAbs().maxCoeff());
                    asymmetry=Norm(raw-raw.transpose())/scale;difference=Norm(raw-fine)/scale;
                    nearroot=asymmetry<=1e-6 && difference<=1e-6 && std::isfinite(linear);
                    const std::string label="base_"+std::to_string(tick)+"_"+std::to_string(iteration);
                    MatrixCsv(matrices,label+"_H1",raw);MatrixCsv(matrices,label+"_H2",fine);
                }
                for(int coordinate=0;coordinate<45;++coordinate)
                    directions << tick << ',' << iteration << ',' << coordinate << ',' << unknown[coordinate] << ','
                               << last.gradient[coordinate] << ',' << direction[coordinate] << ',' << shift << '\n';
                bool accepted=false;double alpha=1;
                for(int backtrack=0;backtrack<25;++backtrack,alpha*=.5)
                {
                    try
                    {
                        const Vector trial=unknown+alpha*direction;
                        const Value candidate=At(context,trial,Coupling);
                        const double stable=StableDelta(context,unknown,trial,last,candidate);
                        const double rhs=1e-4*alpha*last.gradient.dot(direction);
                        const bool shrink=candidate.merit<=.5*last.merit;
                        accepted=nearroot?(shrink||Converged(candidate)):stable<=rhs;
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha << ','
                               << (nearroot?"nearroot_merit":"energy_Armijo") << ',' << rho << ',' << last.merit << ','
                               << candidate.merit << ',' << direction.norm() << ',' << eig.eigenvalues()[0] << ',' << asymmetry
                               << ',' << difference << ',' << linear << ',' << stable << ',' << rhs << ',' << candidate.phi << ','
                               << candidate.gradient.lpNorm<Eigen::Infinity>() << ',' << candidate.forceResidual << ',' << candidate.torqueResidual
                               << ',' << accepted << ',' << (accepted?(nearroot?(shrink?"merit_half":"three_residuals"):"stable_Armijo"):"reject") << ",1\n";
                        if(accepted) {if(nearroot) ++nearrootCount;break;}
                    }
                    catch(const std::runtime_error&)
                    {
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha << ",domain_reject,"
                               << rho << ',' << last.merit << ",NA," << direction.norm() << ',' << eig.eigenvalues()[0]
                               << ',' << asymmetry << ',' << difference << ',' << linear << ",NA,NA,NA,NA,NA,NA,0,domain_reject,0\n";
                    }
                }
                if(!accepted) throw std::runtime_error("ARMIJO_EXHAUSTED");
                unknown+=alpha*direction;
            }
            last=At(context,unknown,Coupling);
            if(!Converged(last)) throw std::runtime_error("ITERATION_LIMIT40");
            const bool sampled=samples.count(tick)>0;
            double minimum=0,hscale=0,asymmetry=0,difference=0,qqMinimum=0;
            bool hpass=true;
            if(sampled)
            {
                const Matrix first=Hessian(context,unknown,Coupling,1e-5),second=Hessian(context,unknown,Coupling,5e-6);
                const Matrix symmetric=.5*(first+first.transpose()),qq=GatherQq(symmetric);
                MatrixCsv(matrices,"accepted_"+std::to_string(tick)+"_H1",first);
                MatrixCsv(matrices,"accepted_"+std::to_string(tick)+"_H2",second);
                MatrixCsv(matrices,"accepted_"+std::to_string(tick)+"_qq",qq);
                Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric),qeig(qq);
                Require(eig.info()==Eigen::Success && qeig.info()==Eigen::Success);
                minimum=eig.eigenvalues()[0];hscale=std::max(1.0,eig.eigenvalues().cwiseAbs().maxCoeff());qqMinimum=qeig.eigenvalues()[0];
                asymmetry=Norm(first-first.transpose())/hscale;difference=Norm(first-second)/hscale;
                hpass=minimum/hscale>1e-6 && asymmetry<=1e-6 && difference<=1e-6;
                for(int index=0;index<45;++index) spectra << tick << ",full," << index << ',' << eig.eigenvalues()[index] << '\n';
                for(int index=0;index<18;++index) spectra << tick << ",qq_gather," << index << ',' << qeig.eigenvalues()[index] << '\n';
            }
            const PairValue oldPair=PairAt(context,context.old,Coupling);
            double oldU=oldPair.energy,nextU=last.pair.energy,oldK=0,nextK=0,damping=0,inertial=0,forceWork=0,pairWork=0;
            double rootX=0,rootQ=0,qnorm=0,pairError=0,exactRrel=0;
            std::array<double,3> work{};
            std::array<State,3> next;
            std::array<std::vector<Vec>,3> velocities;
            for(int guide=0;guide<3;++guide)
            {
                const auto& single=last.guides[guide];
                next[guide]=single.state;velocities[guide].assign(4,Vec::Zero());
                oldU+=Evaluate(context.models[guide],context.old[guide],context.models[guide].authored.positions,Vec(0,-9.8,0)).Energy();
                nextU+=single.potential.Energy();
                rootX=std::max(rootX,(next[guide].positions[0]-context.models[guide].authored.positions[0]).norm());
                rootQ=std::max(rootQ,(next[guide].frames[0].coeffs()-context.models[guide].authored.frames[0].coeffs()).norm());
                for(const auto& frame:next[guide].frames) qnorm=std::max(qnorm,std::abs(frame.norm()-1));
                for(int node=1;node<4;++node)
                {
                    const Vec delta=next[guide].positions[node]-context.old[guide].positions[node];
                    velocities[guide][node]=delta/Step;
                    oldK+=.5*.001*context.velocity[guide][node].squaredNorm();
                    nextK+=.5*.001*velocities[guide][node].squaredNorm();
                    damping+=Step*Damping*velocities[guide][node].squaredNorm();
                    inertial+=.5*.001*(velocities[guide][node]-context.velocity[guide][node]).squaredNorm();
                    forceWork+=(single.potential.force[node]+last.pair.force[guide][node]).dot(delta);
                    pairWork+=last.pair.force[guide][node].dot(delta);
                    work[guide]+=.001*context.probe[guide].dot(delta);
                }
                guideEnergy << tick << ',' << guide << ',' << single.potential.shearStretch << ',' << single.potential.bendTwist << ','
                            << single.potential.ghost << ',' << single.potential.gravity << ',' << context.probe[guide].x() << '\n';
                for(int local=0;local<15;++local)
                {
                    const int coordinate=15*guide+local;
                    const double physical=local<9?single.residual[1+local/3][local%3]:single.potential.bodyTorque[1+(local-9)/3][local%3];
                    residuals << tick << ',' << coordinate << ',' << unknown[coordinate] << ',' << last.gradient[coordinate] << ',' << physical << '\n';
                }
                for(int node=0;node<4;++node)
                {
                    forces << tick << ',' << guide << ',' << node;
                    for(const Vec& vector:{single.potential.force[node],last.pair.force[guide][node],single.residual[node]})
                        for(int axis=0;axis<3;++axis) forces << ',' << vector[axis];
                    if(node<3) for(int axis=0;axis<3;++axis) forces << ',' << single.potential.bodyTorque[node][axis];
                    else forces << ",NA,NA,NA";
                    if(node==0)
                    {
                        for(int axis=0;axis<3;++axis) forces << ',' << -single.potential.force[0][axis];
                        for(int axis=0;axis<3;++axis) forces << ',' << -single.potential.bodyTorque[0][axis];
                    }
                    else forces << ",NA,NA,NA,NA,NA,NA";
                    forces << '\n';
                }
            }
            for(int first=0;first<3;++first) for(int second=first+1;second<3;++second) for(int node=1;node<4;++node)
            {
                const Vec relative=next[first].positions[node]-next[second].positions[node];
                const Vec rest=context.models[first].authored.positions[node]-context.models[second].authored.positions[node];
                const Vec error=relative-rest,forceA=-Coupling*error,forceB=-forceA;
                const Vec moment=next[first].positions[node].cross(forceA)+next[second].positions[node].cross(forceB);
                pairError=std::max(pairError,error.norm());
                exactRrel-=.5*Coupling*((next[first].positions[node]-context.old[first].positions[node])-
                    (next[second].positions[node]-context.old[second].positions[node])).squaredNorm();
                pairs << tick << ',' << first << ',' << second << ',' << node;
                for(const Vec& vector:{error,forceA,forceB,moment}) for(int axis=0;axis<3;++axis) pairs << ',' << vector[axis];
                pairs << '\n';
            }
            const double remainder=nextU-oldU+forceWork,rrel=last.pair.energy-oldPair.energy+pairWork;
            const double probeWork=work[0]+work[1]+work[2];
            const double balance=nextK+nextU-oldK-oldU-probeWork+damping+inertial-remainder;
            const double scale=context.energyScale+std::abs(nextK+nextU)+std::abs(oldK+oldU)+std::abs(probeWork)+damping+inertial+std::abs(remainder);
            const bool pass=hpass && last.phi<=initial+1e-12 && rootX==0 && rootQ==0 && qnorm<=1e-12 &&
                std::abs(balance)/scale<=1e-10 && std::abs(rrel-exactRrel)/.00036<=1e-10;
            steps << tick << ',' << tick*Step << ',' << (pass?"PASS":"FAIL") << ',' << iteration << ','
                  << last.gradient.lpNorm<Eigen::Infinity>() << ',' << last.forceResidual << ',' << last.torqueResidual << ',' << last.merit
                  << ',' << (sampled?"SAMPLED":"H_NOT_SAMPLED");
            if(sampled) steps << ',' << minimum << ',' << hscale << ',' << asymmetry << ',' << difference << ',' << qqMinimum;
            else steps << ",NA,NA,NA,NA,NA";
            steps << ',' << initial << ',' << last.phi << ',' << rootX << ',' << rootQ << ',' << qnorm << ','
                  << std::abs(balance)/scale << ',' << std::abs(rrel-exactRrel)/.00036 << ',' << last.pair.energy << ',' << pairError << '\n';
            energy << tick << ',' << tick*Step << ',' << oldK << ',' << nextK << ',' << oldU << ',' << nextU << ','
                   << oldPair.energy << ',' << last.pair.energy << ',' << work[0] << ',' << work[1] << ',' << work[2] << ','
                   << damping << ',' << inertial << ',' << remainder << ',' << rrel << ',' << exactRrel << ',' << rrel-exactRrel
                   << ',' << balance << ',' << scale << ',' << std::abs(balance)/scale << '\n';
            if(!pass) throw std::runtime_error("NUMERICAL_STEP_GATE_FAILURE");
            context.old=next;context.velocity=velocities;history.push_back(next);
            StateCsv(states,"accepted",tick,Step,context.old,context.velocity);
            if(tick%100==0 || tick==1)
            {
                steps.flush();energy.flush();matrices.flush();trials.flush();directions.flush();
                std::cout << "C3 accepted=" << tick << " seconds=" << Seconds(started) << std::endl;
            }
        }
    }
    catch(const std::exception& error)
    {
        auto failure=Open(directory/"failure.txt");failure << "tick=" << tick << "\nreason=" << error.what() << '\n';
        StateCsv(states,"failed_old",tick-1,Step,context.old,context.velocity);
        for(int coordinate=0;coordinate<45;++coordinate) failure << "z" << coordinate << '=' << unknown[coordinate] << '\n';
        MatrixCsv(matrices,"failed_"+std::to_string(tick)+"_H1",Hessian(context,unknown,Coupling,1e-5));
        MatrixCsv(matrices,"failed_"+std::to_string(tick)+"_H2",Hessian(context,unknown,Coupling,5e-6));
        auto summary=Open(directory/"summary.csv");
        summary << "item,status,value\nC3_numerical,FAIL," << tick << "\naccepted_steps,INFO," << history.size()-1
                << "\nindependentAudit,PENDING,0\n";
        throw;
    }
    const bool behavior=Metrics(directory,context,history);
    auto summary=Open(directory/"summary.csv");
    summary << "item,status,value\nC3_numerical,PASS,13700\nbehavior," << (behavior?"PASS":"FAIL")
            << ",3\nwall_seconds,INFO," << Seconds(started) << "\nnearroot_acceptances,INFO," << nearrootCount
            << "\naccepted_final_H_samples,INFO," << samples.size() << "\nindependentAudit,PENDING,0\nvolume,UNSUPPORTED,0\n";
    return behavior?0:2;
}
}

int main(int argc,char** argv)
{
    try
    {
        if(argc!=3 || (std::string(argv[1])!="--cost" && std::string(argv[1])!="--run"))
            throw std::runtime_error("usage: vl_hair_clump_long_run --cost|--run NEW_DIRECTORY");
        if(!std::filesystem::create_directories(argv[2])) throw std::runtime_error("OUTPUT_EXISTS");
        if(std::string(argv[1])=="--cost") {C3::Cost(argv[2]);return 0;}
        return C3::Run(argv[2]);
    }
    catch(const std::exception& error) {std::cerr << "C3 FAIL: " << error.what() << '\n';return 2;}
}
