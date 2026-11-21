#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"
#include "hair/rod/rodXpbd.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodSectionInertia.h"
#include "hair/rod/rodAirDrag.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <utility>

namespace
{
using namespace VL::Hair::Rod;
using Json = nlohmann::json;
constexpr std::array<int,8> Ids{584,33486,39210,8924,4006,5651,5660,120};
using Clock = std::chrono::steady_clock;

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if(!output) throw std::runtime_error("OUTPUT_FAILURE");
    output.exceptions(std::ios::failbit|std::ios::badbit);
    output << std::setprecision(17);
    return output;
}

std::uintmax_t OutputBytes(std::initializer_list<std::ostream*> streams)
{
    std::uintmax_t bytes=0;
    for(auto* stream:streams)
    {
        const auto position=stream->tellp();
        if(position<0) throw std::runtime_error("OUTPUT_POSITION_FAILURE");
        bytes+=static_cast<std::uintmax_t>(position);
    }
    return bytes;
}

struct Fixture
{
    int id;
    int count;
    double sourceLength;
    RestState rest;
    unsigned kineticMathVersion=1;
};

std::vector<Fixture> Load(const std::filesystem::path& input,bool sectionInertia=false)
{
    std::ifstream stream(input);
    const Json data=Json::parse(stream);
    if(data.at("source_sha256")!="73fa1f3bce4a060f4876c3286a5ec29da5f985c793c9e03f81f5f16340dc39c7")
        throw std::runtime_error("INVENTORY_SOURCE_IDENTITY");
    std::vector<Fixture> fixtures;
    for(int count:{8,16,32}) for(int id:Ids)
    {
        auto found=std::find_if(data.at("samples").begin(),data.at("samples").end(),
            [id](const Json& sample){return sample.at("guide_id").get<int>()==id;});
        if(found==data.at("samples").end()) throw std::runtime_error("MISSING_FIXED_ID");
        std::vector<Vec3> original;
        std::vector<double> arcs{0};
        for(const auto& point:found->at("positions_xyzw"))
        {
            original.emplace_back(.01*point[0].get<double>(),.01*point[1].get<double>(),.01*point[2].get<double>());
            if(original.size()>1) arcs.push_back(arcs.back()+(original.back()-original[original.size()-2]).norm());
        }
        if(original.size()!=32) throw std::runtime_error("SOURCE_POINT_COUNT");
        if(std::abs(arcs.back()-.01*found->at("length_cm").get<double>())>1e-14)
            throw std::runtime_error("UNIT_LENGTH_ORACLE");
        std::vector<Vec3> points;
        if(count==32) points=original;
        else
        {
            points={original[0],original[1]};
            for(int node=2;node<count;++node)
            {
                if(node==count-1) {points.push_back(original.back());continue;}
                const double target=arcs[1]+(arcs.back()-arcs[1])*(node-1)/(count-2);
                const auto upper=std::upper_bound(arcs.begin(),arcs.end(),target);
                const size_t segment=static_cast<size_t>(upper-arcs.begin()-1);
                const double fraction=(target-arcs[segment])/(arcs[segment+1]-arcs[segment]);
                points.push_back((1-fraction)*original[segment]+fraction*original[segment+1]);
            }
        }
        std::vector<double> markers(count,1);
        markers[0]=markers[1]=0;
        const RestState rest=sectionInertia?BuildSectionInertiaRestState(points,Material{}, {},markers):
            BuildRestState(points,Material{}, {},markers);
        fixtures.push_back({id,count,arcs.back(),rest,sectionInertia?SectionInertiaKineticMathVersion:1});
        if(points.front()!=original.front() || points[1]!=original[1] || points.back()!=original.back())
            throw std::runtime_error("ENDPOINT_CHANGED");
    }
    return fixtures;
}

void StateRows(std::ostream& output,const Fixture& fixture,const std::string& kind,const State& state)
{
    for(size_t node=0;node<state.positions.size();++node)
    {
        output << fixture.id << ',' << fixture.count << ',' << kind << ',' << node;
        for(int axis=0;axis<3;++axis) output << ',' << state.positions[node][axis];
        if(node<state.orientations.size())
        {
            const Quat& orientation=state.orientations[node];
            output << ',' << orientation.w() << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z();
        }
        else output << ",NA,NA,NA,NA";
        output << '\n';
    }
}

void Group(std::ostream& output,const std::vector<Fixture>& fixtures,const std::string& kind,
           const std::vector<State>& states)
{
    for(int count:{8,16,32})
    {
        Vec3 minimum=Vec3::Constant(1e99),maximum=Vec3::Constant(-1e99),centroid=Vec3::Zero();
        size_t points=0;
        double nearestMinimum=1e99,nearestMaximum=0;
        for(size_t index=0;index<fixtures.size();++index) if(fixtures[index].count==count)
            for(size_t node=0;node<states[index].positions.size();++node)
            {
                const Vec3& position=states[index].positions[node];
                minimum=minimum.cwiseMin(position);maximum=maximum.cwiseMax(position);centroid+=position;++points;
                double nearest=1e99;
                for(size_t other=0;other<fixtures.size();++other)
                    if(fixtures[other].count==count && other!=index)
                        nearest=std::min(nearest,(position-states[other].positions[node]).norm());
                nearestMinimum=std::min(nearestMinimum,nearest);nearestMaximum=std::max(nearestMaximum,nearest);
            }
        centroid/=static_cast<double>(points);
        const Vec3 extent=maximum-minimum;
        output << kind << ',' << count;
        for(const Vec3& vector:{minimum,maximum,centroid,extent}) for(int axis=0;axis<3;++axis) output << ',' << vector[axis];
        output << ',' << extent.prod() << ',' << nearestMinimum << ',' << nearestMaximum << '\n';
    }
}

SupportEvaluation Support(const RestState& rest,const State& state,const RootFrame& root,const ProgressiveSupport* profile)
{
    return profile?EvaluateProgressiveSupport(rest,state,root,*profile):EvaluateRootSupport(rest,state,root);
}

ImplicitDiagnostics Advance(const RestState& rest,State& state,const StepInput& input,const ProgressiveSupport* profile,bool continuation=false,
                            ResidualWorkCertificate* certificate=nullptr)
{
    if(profile && continuation && certificate)
        return StepImplicitMomentumProgressiveContinuationWorkGuard(rest,state,input,*profile,*certificate);
    if(profile && continuation) return StepImplicitMomentumProgressiveContinuation(rest,state,input,*profile);
    return profile?StepImplicitMomentumProgressive(rest,state,input,*profile):StepImplicitMomentum(rest,state,input);
}

std::vector<Vector6> Physical(const RestState& rest,const State& old,const State& state,const StepInput& input,
                             const ProgressiveSupport* profile)
{
    return profile?EvaluateProgressiveImplicitResidual(rest,old,state,input,*profile):EvaluateImplicitResidual(rest,old,state,input);
}

Vec3 Tangent(const State& state,size_t node)
{
    Vec3 tangent;
    if(node==0) tangent=state.positions[1]-state.positions[0];
    else if(node+1==state.positions.size()) tangent=state.positions[node]-state.positions[node-1];
    else tangent=(state.positions[node]-state.positions[node-1]).normalized()+
        (state.positions[node+1]-state.positions[node]).normalized();
    return tangent.normalized();
}

int Prepare(const std::vector<Fixture>& fixtures,const std::filesystem::path& directory,bool progressive=false)
{
    auto checks=Open(directory/"checks.csv");checks << "guide,points,check,value,tolerance,status\n";
    auto states=Open(directory/"states.csv");states << "guide,points,kind,node,x,y,z,qw,qx,qy,qz\n";
    auto restRows=Open(directory/"rest.csv");
    restRows << "guide,points,node,mass,inverse_mass,support_k,support_c,rx,ry,rz,l,nu0x,nu0y,nu0z,phi0x,phi0y,phi0z,Sx,Sy,Sz,Bx,By,Bz,Jx,Jy,Jz,invJx,invJy,invJz\n";
    auto fd=Open(directory/"fd.csv");fd << "guide,points,kind,node,axis,epsilon,analytic,finite_difference,normalized_error\n";
    auto inputs=Open(directory/"inputs.csv");
    inputs << "guide,points,source_length_m,resampled_length_m,length_ratio,total_mass_kg,radius_m,density_kg_m3,E_Pa,G_Pa,delta,zeta,kineticMathVersion\n";
    auto evaluations=Open(directory/"evaluations.csv");evaluations << "guide,points,kind,node,fx,fy,fz,tx,ty,tz\n";
    auto objectivityRows=Open(directory/"objectivity.csv");
    objectivityRows << "guide,points,kind,index,axis,original,transformed,expected_transformed,absolute_error,normalization,normalized_error\n";
    auto profileRows=Open(directory/"progressive-profile.csv");
    profileRows << "guide,points,node,k1_N_m,k2_N_m3,design_force_N,design_air_speed_mps,response_limit_ratio,drag_acceleration_per_speed\n";
    auto inertiaRows=Open(directory/"inertia-elements.csv");
    inertiaRows << "guide,points,segment,length,mass,Jx,Jy,Jz,expected_section_Jx,expected_section_Jy,expected_section_Jz\n";
    auto massBudget=Open(directory/"rigid-mass-budget.csv");
    massBudget << "guide,points,lumped_centerline_K,consistent_centerline_K,section_rotary_K,continuum_cylinder_K,expected_lumping_excess,actual_lumping_excess,consistent_plus_section_relative_error,lumping_budget_relative_error\n";
    auto groups=Open(directory/"group-proxy.csv");
    groups << "kind,points,minx,miny,minz,maxx,maxy,maxz,cx,cy,cz,extentx,extenty,extentz,bbox_volume_proxy_m3,NN_min_m,NN_max_m\n";
    std::vector<State> authoredStates,perturbedStates;
    int failures=0;
    bool sectionInertiaValid=true;
    auto check=[&](const Fixture& fixture,const char* name,double value,double tolerance)
    {
        const bool pass=std::isfinite(value) && value<=tolerance;
        checks << fixture.id << ',' << fixture.count << ',' << name << ',' << value << ',' << tolerance << ',' << (pass?"PASS":"FAIL") << '\n';
        failures+=!pass;
    };
    const Material material;
    const auto started=Clock::now();
    for(const Fixture& fixture:fixtures)
    {
        if(std::chrono::duration<double>(Clock::now()-started).count()>120)
            throw std::runtime_error("INCOMPLETE_PREPARE_BUDGET");
        const RestState& rest=fixture.rest;
        ProgressiveSupport profile;
        if(progressive)
        {
            profile=BuildProgressiveSupport(rest);
            for(size_t node=0;node<rest.positions.size();++node)
                profileRows << fixture.id << ',' << fixture.count << ',' << node << ',' << rest.supportStiffness[node] << ','
                            << profile.cubicStiffness[node] << ',' << profile.designForces[node] << ',' << profile.designAirSpeed << ','
                            << profile.responseLimitRatio << ',' << profile.dragAccelerationPerSpeed << '\n';
        }
        const ProgressiveSupport* profilePointer=progressive?&profile:nullptr;
        State authored=MakeAuthoredState(rest),general=authored;
        authoredStates.push_back(authored);
        StateRows(states,fixture,"authored",authored);
        const double forceScale=material.youngModulus*Pi*material.radius*material.radius;
        const double torqueScale=rest.bendTwist.front().maxCoeff()/rest.totalLength;
        const double energyScale=forceScale*rest.totalLength;
        const double expectedMass=material.density*Pi*material.radius*material.radius*rest.totalLength;
        const double mass=std::accumulate(rest.masses.begin(),rest.masses.end(),0.0);
        inputs << fixture.id << ',' << fixture.count << ',' << fixture.sourceLength << ',' << rest.totalLength << ','
               << rest.totalLength/fixture.sourceLength << ',' << mass << ',' << material.radius << ',' << material.density << ','
               << material.youngModulus << ',' << material.shearModulus << ',' << material.supportOffsetRatio << ',' << material.supportDampingRatio
               << ',' << fixture.kineticMathVersion << '\n';
        check(fixture,"total_mass_relative",std::abs(mass-expectedMass)/expectedMass,1e-12);
        double inverseError=0,qerror=0;
        for(size_t node=0;node<rest.positions.size();++node)
        {
            if(!(rest.masses[node]>0)) throw std::runtime_error("NONPOSITIVE_MASS");
            const double expected=node<2?0:1/rest.masses[node];
            inverseError=std::max(inverseError,std::abs(rest.inverseMasses[node]-expected)/std::max(1.0,expected));
            restRows << fixture.id << ',' << fixture.count << ',' << node << ',' << rest.masses[node] << ','
                     << rest.inverseMasses[node] << ',' << rest.supportStiffness[node] << ',' << rest.supportDamping[node];
            for(int axis=0;axis<3;++axis) restRows << ',' << rest.rootLocalPositions[node][axis];
            if(node<rest.lengths.size())
            {
                qerror=std::max(qerror,std::abs(rest.orientations[node].norm()-1));
                restRows << ',' << rest.lengths[node];
                const Vec3 angle=node<rest.restJointAngles.size()?rest.restJointAngles[node]:Vec3::Zero();
                for(const Vec3& vector:{rest.restStrains[node],angle,rest.stretchShear[node],rest.bendTwist[node],
                    rest.inertias[node],rest.inverseInertias[node]})
                    for(int axis=0;axis<3;++axis) restRows << ',' << vector[axis];
                if((rest.inertias[node].array()<=0).any()) throw std::runtime_error("NONPOSITIVE_J");
            }
            else restRows << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA";
            restRows << '\n';
        }
        check(fixture,"inverse_mass",inverseError,1e-12);check(fixture,"qnorm",qerror,1e-12);
        check(fixture,"root_orientation_inverse_J",rest.inverseInertias.front().norm(),0);
        double inertiaError=0;
        double lumpedK=0,consistentK=0,sectionK=0,cylinderK=0,lumpingExcess=0;
        const Vec3 worldOmega(.7,-.4,.3),translationVelocity(.02,-.01,.03);
        for(size_t node=0;node<rest.positions.size();++node)
        {
            const Vec3 velocity=translationVelocity+worldOmega.cross(rest.positions[node]-rest.positions.front());
            lumpedK+=.5*rest.masses[node]*velocity.squaredNorm();
        }
        for(size_t segment=0;segment<rest.lengths.size();++segment)
        {
            const double length=rest.lengths[segment];
            const double segmentMass=material.density*Pi*material.radius*material.radius*length;
            const double radiusSquared=material.radius*material.radius;
            const Vec3 expectedJ=segmentMass*Vec3(radiusSquared/4,radiusSquared/4,radiusSquared/2);
            inertiaError=std::max(inertiaError,((rest.inertias[segment]-expectedJ).cwiseAbs().cwiseQuotient(expectedJ)).maxCoeff());
            inertiaRows << fixture.id << ',' << fixture.count << ',' << segment << ',' << length << ',' << segmentMass;
            for(const Vec3& value:{rest.inertias[segment],expectedJ})
                for(int axis=0;axis<3;++axis) inertiaRows << ',' << value[axis];
            inertiaRows << '\n';
            const Vec3 firstVelocity=translationVelocity+worldOmega.cross(rest.positions[segment]-rest.positions.front());
            const Vec3 secondVelocity=translationVelocity+worldOmega.cross(rest.positions[segment+1]-rest.positions.front());
            const Vec3 meanVelocity=.5*(firstVelocity+secondVelocity);
            const Vec3 bodyOmega=rest.orientations[segment].conjugate()*worldOmega;
            consistentK+=segmentMass*(firstVelocity.squaredNorm()+firstVelocity.dot(secondVelocity)+secondVelocity.squaredNorm())/6;
            sectionK+=.5*bodyOmega.dot(rest.inertias[segment].cwiseProduct(bodyOmega));
            const Vec3 cylinderJ=segmentMass*Vec3(length*length/12+radiusSquared/4,length*length/12+radiusSquared/4,radiusSquared/2);
            cylinderK+=.5*segmentMass*meanVelocity.squaredNorm()+.5*bodyOmega.dot(cylinderJ.cwiseProduct(bodyOmega));
            lumpingExcess+=segmentMass*(secondVelocity-firstVelocity).squaredNorm()/12;
        }
        const double kineticScale=std::max(1e-30,cylinderK);
        const double rigidError=std::abs(consistentK+sectionK-cylinderK)/kineticScale;
        const double quadratureError=std::abs(lumpedK-consistentK-lumpingExcess)/kineticScale;
        sectionInertiaValid=sectionInertiaValid && inertiaError<=1e-12 && rigidError<=1e-12 && quadratureError<=1e-12;
        check(fixture,"section_rotary_inertia",inertiaError,1e-12);
        check(fixture,"rigid_consistent_mass_plus_section",rigidError,1e-12);
        check(fixture,"lumped_mass_quadrature_budget",quadratureError,1e-12);
        massBudget << fixture.id << ',' << fixture.count << ',' << lumpedK << ',' << consistentK << ',' << sectionK << ','
                   << cylinderK << ',' << lumpingExcess << ',' << lumpedK-consistentK << ',' << rigidError << ',' << quadratureError << '\n';
        double angleMargin=0;
        for(const auto& angle:rest.restJointAngles) angleMargin=std::max(angleMargin,angle.norm());
        check(fixture,"principal_rest_log",angleMargin>=RotationLimit?1:0,0);
        const auto restEnergy=EvaluateEnergy(rest,authored);
        double force=0,torque=0;
        for(const auto& value:restEnergy.forces) force=std::max(force,value.norm()/forceScale);
        for(const auto& value:restEnergy.bodyTorques) torque=std::max(torque,value.norm()/torqueScale);
        check(fixture,"rest_energy",restEnergy.Total()/energyScale,1e-20);
        check(fixture,"rest_force",force,1e-10);check(fixture,"rest_torque",torque,1e-10);
        for(size_t node=2;node<general.positions.size();++node)
            general.positions[node]+=rest.totalLength*.001*Vec3(std::sin(static_cast<double>(node)),std::cos(static_cast<double>(node)),std::sin(2.0*node));
        for(size_t segment=1;segment<general.orientations.size();++segment)
            general.orientations[segment]=ApplyBodyIncrement(general.orientations[segment],Vec3(.003,-.002,.004));
        StateRows(states,fixture,"general",general);perturbedStates.push_back(general);
        const auto energy=EvaluateEnergy(rest,general);
        double fdError=0;
        for(size_t node=0;node<general.positions.size();++node)
        {
            evaluations << fixture.id << ',' << fixture.count << ",general," << node;
            for(int axis=0;axis<3;++axis) evaluations << ',' << energy.forces[node][axis];
            if(node<energy.bodyTorques.size()) for(int axis=0;axis<3;++axis) evaluations << ',' << energy.bodyTorques[node][axis];
            else evaluations << ",NA,NA,NA";
            evaluations << '\n';
            for(int kind=0;kind<2;++kind)
            {
                if(kind==1 && node>=general.orientations.size()) continue;
                const double epsilon=kind==0?1e-6*rest.totalLength:1e-6;
                for(int axis=0;axis<3;++axis)
                {
                    State plus=general,minus=general;
                    if(kind==0) {plus.positions[node][axis]+=epsilon;minus.positions[node][axis]-=epsilon;}
                    else
                    {
                        const Vec3 delta=epsilon*Vec3::Unit(axis);
                        plus.orientations[node]=ApplyBodyIncrement(general.orientations[node],delta);
                        minus.orientations[node]=ApplyBodyIncrement(general.orientations[node],-delta);
                    }
                    const double numeric=-(EvaluateEnergy(rest,plus).Total()-EvaluateEnergy(rest,minus).Total())/(2*epsilon);
                    const double analytic=kind==0?energy.forces[node][axis]:energy.bodyTorques[node][axis];
                    const double error=std::abs(numeric-analytic)/(kind==0?forceScale:torqueScale);
                    fdError=std::max(fdError,error);
                    fd << fixture.id << ',' << fixture.count << ',' << (kind==0?"force":"torque") << ',' << node << ','
                       << axis << ',' << epsilon << ',' << analytic << ',' << numeric << ',' << error << '\n';
                }
            }
        }
        check(fixture,"general_energy_FD",fdError,2e-6);
        RootFrame root;root.position=rest.positions.front();root.orientation=rest.orientations.front();
        const auto support=Support(rest,general,root,profilePointer);
        const Quat rotation=Exp(Vec3(.7,-.4,.3));
        const Vec3 translation(.12,-.07,.09);
        State transformed=general;
        for(auto& position:transformed.positions) position=translation+rotation*position;
        for(auto& orientation:transformed.orientations) orientation=rotation*orientation;
        StateRows(states,fixture,"transformed_general",transformed);
        root.position=translation+rotation*root.position;root.orientation=rotation*root.orientation;
        const auto rotated=EvaluateEnergy(rest,transformed);
        const auto supportRotated=Support(rest,transformed,root,profilePointer);
        for(size_t node=0;node<transformed.positions.size();++node)
        {
            evaluations << fixture.id << ',' << fixture.count << ",transformed_general," << node;
            for(int axis=0;axis<3;++axis) evaluations << ',' << rotated.forces[node][axis];
            if(node<rotated.bodyTorques.size()) for(int axis=0;axis<3;++axis) evaluations << ',' << rotated.bodyTorques[node][axis];
            else evaluations << ",NA,NA,NA";
            evaluations << '\n';
        }
        auto compareObjectivity=[&](const char* kind,size_t index,int axis,double original,
            double actual,double expected,double normalization)
        {
            objectivityRows << fixture.id << ',' << fixture.count << ',' << kind << ',' << index << ',' << axis << ','
                            << original << ',' << actual << ',' << expected << ',' << std::abs(actual-expected) << ','
                            << normalization << ',' << std::abs(actual-expected)/normalization << '\n';
        };
        compareObjectivity("rod_energy",0,0,energy.Total(),rotated.Total(),energy.Total(),energyScale);
        compareObjectivity("support_energy",0,0,support.potential,supportRotated.potential,support.potential,energyScale);
        double objectivity=std::max(std::abs(energy.Total()-rotated.Total()),std::abs(support.potential-supportRotated.potential))/energyScale;
        for(size_t node=0;node<general.positions.size();++node)
        {
            const Vec3 expectedForce=rotation*energy.forces[node];
            const Vec3 expectedSupport=rotation*support.forces[node];
            for(int axis=0;axis<3;++axis)
            {
                compareObjectivity("rod_force",node,axis,energy.forces[node][axis],rotated.forces[node][axis],expectedForce[axis],forceScale);
                compareObjectivity("support_force",node,axis,support.forces[node][axis],supportRotated.forces[node][axis],expectedSupport[axis],forceScale);
            }
            objectivity=std::max(objectivity,(rotation*energy.forces[node]-rotated.forces[node]).norm()/forceScale);
            objectivity=std::max(objectivity,(rotation*support.forces[node]-supportRotated.forces[node]).norm()/forceScale);
        }
        for(size_t segment=0;segment<general.orientations.size();++segment)
        {
            objectivity=std::max(objectivity,(energy.bodyTorques[segment]-rotated.bodyTorques[segment]).norm()/torqueScale);
            for(int axis=0;axis<3;++axis)
                compareObjectivity("body_torque",segment,axis,energy.bodyTorques[segment][axis],rotated.bodyTorques[segment][axis],
                    energy.bodyTorques[segment][axis],torqueScale);
        }
        check(fixture,"rigid_objectivity",objectivity,1e-10);
    }
    for(int count:{8,16,32})
    {
        std::vector<Vec3> points;
        for(int node=0;node<count;++node) points.emplace_back(0,0,.3*node/(count-1));
        const bool sectionInertia=fixtures.front().kineticMathVersion==SectionInertiaKineticMathVersion;
        const RestState rest=sectionInertia?BuildSectionInertiaRestState(points):BuildRestState(points);
        State twist=MakeAuthoredState(rest);
        for(size_t segment=0;segment<twist.orientations.size();++segment)
            twist.orientations[segment]=Exp(Vec3(0,0,.2*segment/(count-2)));
        const auto energy=EvaluateEnergy(rest,twist);
        double expected=0;
        for(size_t joint=0;joint<rest.restJointAngles.size();++joint)
            expected+=.5*rest.bendTwist[joint].z()*std::pow(.2/(count-2),2)/(.5*(rest.lengths[joint]+rest.lengths[joint+1]));
        const Fixture fixture{-1,count,.3,rest,sectionInertia?SectionInertiaKineticMathVersion:1};
        StateRows(states,fixture,"uniform_twist",twist);
        check(fixture,"twist_energy_relative",std::abs(energy.bendTwist-expected)/expected,1e-10);
        check(fixture,"twist_zero_shear",energy.stretchShear/(rest.stretchShear.front().z()*.3),1e-20);
        check(fixture,"twist_nonzero",energy.bendTwist>0 && energy.bodyTorques.back().norm()>0?0:1,0);
    }
    Group(groups,fixtures,"authored",authoredStates);Group(groups,fixtures,"general",perturbedStates);
    auto summary=Open(directory/"summary.json");
    summary << Json{{"stage","prepare"},{"status",failures?"FAIL":"PASS"},{"failures",failures},{"groomFixtures",24},
        {"twistFixtures",3},{"advancedSteps",0},{"volumePhysics","UNSUPPORTED_NO_GUIDE_COUPLING"},
        {"profile",progressive?"effectiveBundleProgressiveV3":"effectiveBundleV1"},
        {"kineticMathVersion",fixtures.front().kineticMathVersion},
        {"rotary_inertia",sectionInertiaValid?"SECTION_ONLY_VALIDATED":"PHYSICAL_INERTIA_CHECK_FAILED"},
        {"groupProxy","8_SELECTED_GUIDES_ONLY_NOT_FULL_GROOM"},{"independentAudit","PENDING"}}.dump(2);
    return failures?2:0;
}

int Short(const std::vector<Fixture>& fixtures,const std::filesystem::path& directory,bool perf,bool implicit,bool reference=false,int referenceLevel=3,bool progressive=false,bool wind=false,bool continuation=false,bool endpointAir=false)
{
    auto steps=Open(directory/"steps.csv");
    steps << "guide,points,h,sweeps,tick,status,converged,rx_scaled,rq_scaled,ss_defect,bt_defect,support_over_L,predictor_residual,energy,linear_K,angular_K\n";
    auto states=Open(directory/"states.csv");states << "guide,points,kind,node,x,y,z,qw,qx,qy,qz\n";
    auto endpoints=Open(directory/"refinement.csv");
    endpoints << "guide,points,direction,fixed_level,x_coarse_difference,x_fine_difference,SO3_coarse_difference,SO3_fine_difference,status\n";
    auto dynamics=Open(directory/"dynamics.csv");
    dynamics << "guide,points,h,sweeps,tick,node,x,y,z,vx,vy,vz,qw,qx,qy,qz,wx,wy,wz\n";
    auto supportEnergy=Open(directory/"support-energy.csv");
    supportEnergy << "guide,points,h,sweeps,tick,Uss,Ubt,Ut,Ugravity,support_drive_power,support_dissipation\n";
    auto shape=Open(directory/"shape.csv");
    shape << "guide,points,h,sweeps,Dshape_over_L,centroid_shift_m,eps_Dmax_m,max_tangent_angle_rad,max_turn_change_rad,status\n";
    auto groups=Open(directory/"group-proxy.csv");
    groups << "kind,points,minx,miny,minz,maxx,maxy,maxz,cx,cy,cz,extentx,extenty,extentz,bbox_volume_proxy_m3,NN_min_m,NN_max_m\n";
    std::array<std::vector<State>,9> groupFinal;
    auto solverRows=Open(directory/"solver.csv");
    solverRows << "guide,points,h,tick,solver,newton_iterations,backtracks,normalized_residual,linear_solve_residual,XPBD_defects\n";
    auto physicalRows=Open(directory/"physical-residuals.csv");
    physicalRows << "guide,points,h,sweeps,tick,node,pinned,rx,ry,rz,rqx,rqy,rqz\n";
    auto rawConstraints=Open(directory/"raw-constraints.csv");
    rawConstraints << "guide,points,h,sweeps,tick,kind,index,cx,cy,cz\n";
    auto stateGates=Open(directory/"state-gates.csv");
    stateGates << "guide,points,h,sweeps,tick,pin_error,root_q_error,qnorm_error,max_adjacent_log,max_step_log,finite,status\n";
    auto external=Open(directory/"external-forces.csv");
    external << "guide,points,h,sweeps,tick,node,probe_acceleration_x,air_speed_x,fx,fy,fz\n";
    auto reactions=Open(directory/"root-reactions.csv");
    reactions << "guide,points,h,sweeps,tick,valid,fx,fy,fz,body_tx,body_ty,body_tz\n";
    auto airRows=Open(directory/"endpoint-air.csv");
    airRows << "guide,points,h,tick,node,old_tx,old_ty,old_tz,Ux,Uy,Uz,other_fx,other_fy,other_fz,air_fx,air_fy,air_fz,air_work,other_work\n";
    auto certificates=Open(directory/"work-certificates.csv");
    certificates << "guide,points,h,tick,free_work,free_work_upper,root_work,Eref,balance,scale,work_bound_scaled,balance_scaled,identity_scaled,finite,accepted\n";
    const auto started=Clock::now();
    int failures=0,completed=0,unqualified=0,unresolved=0;
    double solverSeconds=0;
    const size_t planned=perf?2000:fixtures.size();
    for(size_t index=0;index<planned;++index)
    {
        const Fixture& fixture=perf?fixtures[16+index%8]:fixtures[index];
        ProgressiveSupport profile;
        if(progressive) profile=BuildProgressiveSupport(fixture.rest);
        const ProgressiveSupport* profilePointer=progressive?&profile:nullptr;
        std::array<std::array<State,5>,3> finals;
        int sweepIndex=0;
        for(unsigned sweeps:implicit?std::vector<unsigned>{0}:perf?std::vector<unsigned>{32}:std::vector<unsigned>{16,32,64})
        {
            for(int refinement=reference?referenceLevel:0;refinement<(reference?referenceLevel+1:perf?1:3);++refinement)
            {
                State state=MakeAuthoredState(fixture.rest);
                StepInput input;input.sweeps=implicit?16:sweeps;input.stepSeconds=1.0/(240*(1<<refinement));
                input.root.position=fixture.rest.positions.front();input.root.orientation=fixture.rest.orientations.front();
                input.externalForces.resize(fixture.rest.masses.size());
                for(size_t node=0;node<fixture.rest.masses.size();++node)
                    input.externalForces[node]=fixture.rest.masses[node]*Vec3(endpointAir?0:20,0,0);
                const double forceScale=std::accumulate(fixture.rest.masses.begin(),fixture.rest.masses.end(),0.0)*Material{}.referenceGravity+
                    *std::max_element(fixture.rest.supportStiffness.begin(),fixture.rest.supportStiffness.end())*fixture.rest.totalLength;
                const int ticks=perf?1:6*(1<<refinement);
                StateRows(states,fixture,"initial_h"+std::to_string(refinement)+"_s"+std::to_string(sweeps),state);
                for(int tick=1;tick<=ticks;++tick)
                {
                    if(std::chrono::duration<double>(Clock::now()-started).count()>(reference?60:implicit && !perf?600:300))
                        throw std::runtime_error("INCOMPLETE_BUDGET");
                    if(completed%100==0)
                    {
                        const auto bytes=OutputBytes({&steps,&states,&endpoints,&dynamics,&supportEnergy,&shape,
                            &groups,&solverRows,&physicalRows,&rawConstraints,&stateGates,&external,&reactions,&airRows,&certificates});
                        if(bytes>(reference?67108864ULL:536870912ULL)) throw std::runtime_error("INCOMPLETE_OUTPUT_BUDGET");
                    }
                    if(wind && !endpointAir)
                        for(size_t node=0;node<state.positions.size();++node)
                            input.externalForces[node]=ProgressiveAirDragForce(profile,fixture.rest.masses[node],
                                Vec3(20,0,0)-state.velocities[node],Tangent(state,node));
                    if(!endpointAir) for(size_t node=0;node<state.positions.size();++node)
                    {
                        external << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ','
                                 << node << ',' << (wind?0:20) << ',' << (wind?20:0);
                        for(int axis=0;axis<3;++axis) external << ',' << input.externalForces[node][axis];
                        external << '\n';
                    }
                    const State old=state;
                    StepDiagnostics diagnostic;
                    const auto solverStarted=Clock::now();
                    if(implicit)
                    {
                        ResidualWorkCertificate certificate;
                        const auto result=endpointAir?
                            StepImplicitMomentumProgressiveAirContinuation(fixture.rest,state,input,profile,Vec3(20,0,0),certificate):
                            Advance(fixture.rest,state,input,profilePointer,continuation);
                        if(endpointAir)
                        {
                            certificates << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << tick
                                << ',' << certificate.freeResidualWork << ',' << certificate.freeResidualWorkUpper
                                << ',' << certificate.rootConstraintWork << ',' << certificate.energyReference
                                << ',' << certificate.frozenTargetBalance << ',' << certificate.ledgerScale
                                << ',' << certificate.workBoundScaled << ',' << certificate.balanceScaled << ',' << certificate.identityScaled
                                << ',' << certificate.finite << ',' << certificate.accepted << '\n';
                            if(result.status==Status::Success && (!certificate.accepted || result.normalizedResidual>1e-8)) ++unqualified;
                            for(size_t node=0;node<state.positions.size();++node)
                            {
                                const auto air=EvaluateFrozenTangentAirDrag(old,state,node,fixture.rest.masses[node],
                                    input.stepSeconds,profile,Vec3(20,0,0));
                                const Vec3 delta=state.positions[node]-old.positions[node];
                                external << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ','
                                    << node << ",0,20";
                                for(int axis=0;axis<3;++axis) external << ',' << air.force[axis];
                                external << '\n';
                                airRows << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << tick << ',' << node;
                                for(const Vec3& value:{air.tangent,Vec3(20,0,0),input.externalForces[node],air.force})
                                    for(int axis=0;axis<3;++axis) airRows << ',' << value[axis];
                                airRows << ',' << air.force.dot(delta) << ',' << input.externalForces[node].dot(delta) << '\n';
                            }
                        }
                        solverSeconds+=std::chrono::duration<double>(Clock::now()-solverStarted).count();
                        diagnostic=result;
                        solverRows << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << tick
                                   << ',' << (endpointAir?"implicitMomentumProgressiveAirContinuation":continuation?"implicitMomentumProgressiveContinuation9":progressive?"implicitMomentumProgressiveV3":"implicitMomentumV2") << ',' << result.newtonIterations << ',' << result.backtracks << ','
                                   << result.normalizedResidual << ',' << result.linearSolveResidual << ",NA\n";
                    }
                    else
                    {
                        diagnostic=Step(fixture.rest,state,input);
                        solverSeconds+=std::chrono::duration<double>(Clock::now()-solverStarted).count();
                    }
                    reactions << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick
                              << ',' << (diagnostic.status==Status::Success);
                    for(const Vec3& value:{diagnostic.rootForce,diagnostic.rootBodyTorque})
                        for(int axis=0;axis<3;++axis) reactions << ',' << value[axis];
                    reactions << '\n';
                    const auto actualEnergy=EvaluateEnergy(fixture.rest,state);
                    const auto supportSample=Support(fixture.rest,state,input.root,profilePointer);
                    double gravityEnergy=0;
                    for(size_t node=0;node<state.positions.size();++node)
                        gravityEnergy-=fixture.rest.masses[node]*input.gravity.dot(state.positions[node]);
                    double pinError=0,rootQError=(state.orientations.front().coeffs()-input.root.orientation.coeffs()).norm();
                    double qerror=0,adjacentAngle=0,stepAngle=0;
                    bool finite=true;
                    for(size_t node=0;node<state.positions.size();++node)
                    {
                        finite=finite && state.positions[node].allFinite() && state.velocities[node].allFinite();
                        if(fixture.rest.inverseMasses[node]==0)
                            pinError=std::max(pinError,(state.positions[node]-SampleRootSupport(input.root,fixture.rest.rootLocalPositions[node]).position).norm());
                    }
                    for(size_t segment=0;segment<state.orientations.size();++segment)
                    {
                        finite=finite && state.orientations[segment].coeffs().allFinite() && state.angularVelocities[segment].allFinite();
                        qerror=std::max(qerror,std::abs(state.orientations[segment].norm()-1));
                        stepAngle=std::max(stepAngle,Log(old.orientations[segment].conjugate()*state.orientations[segment]).norm());
                        if(segment+1<state.orientations.size())
                            adjacentAngle=std::max(adjacentAngle,Log(state.orientations[segment].conjugate()*state.orientations[segment+1]).norm());
                        const auto shear=EvaluateShearBlock(fixture.rest,state,segment);
                        rawConstraints << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ",ss," << segment;
                        for(int axis=0;axis<3;++axis) rawConstraints << ',' << shear.constraint[axis];
                        rawConstraints << '\n';
                        if(segment+1<state.orientations.size())
                        {
                            const auto bend=EvaluateBendBlock(fixture.rest,state,segment);
                            rawConstraints << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ",bt," << segment;
                            for(int axis=0;axis<3;++axis) rawConstraints << ',' << bend.constraint[axis];
                            rawConstraints << '\n';
                        }
                    }
                    const bool statePass=finite && pinError==0 && rootQError==0 && qerror<=1e-12 &&
                        adjacentAngle<RotationLimit && stepAngle<RotationLimit;
                    stateGates << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ','
                               << pinError << ',' << rootQError << ',' << qerror << ',' << adjacentAngle << ',' << stepAngle << ',' << finite
                               << ',' << (statePass?"PASS":"FAIL") << '\n';
                    if(!statePass) ++unqualified;
                    if(implicit)
                    {
                        const auto physical=endpointAir?
                            EvaluateProgressiveAirResidual(fixture.rest,old,state,input,profile,FrozenTangentAir{Vec3(20,0,0),1}):
                            Physical(fixture.rest,old,state,input,profilePointer);
                        for(size_t node=0;node<physical.size();++node)
                        {
                            physicalRows << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ',' << node
                                         << ',' << (fixture.rest.inverseMasses[node]==0);
                            for(int axis=0;axis<6;++axis) physicalRows << ',' << physical[node][axis];
                            physicalRows << '\n';
                        }
                    }
                    supportEnergy << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick
                                  << ',' << actualEnergy.stretchShear << ',' << actualEnergy.bendTwist << ',' << supportSample.potential
                                  << ',' << gravityEnergy << ',' << supportSample.drivePower << ',' << supportSample.dissipation << '\n';
                    const double rx=diagnostic.maximumLinearResidual/forceScale;
                    const double rq=diagnostic.maximumAngularResidual/(forceScale*fixture.rest.totalLength);
                    const double support=diagnostic.maximumSupportDefect/fixture.rest.totalLength;
                    if(rx>1e-2 || rq>1e-2 || (!implicit && (diagnostic.maximumShearDefect>1e-4 ||
                        diagnostic.maximumBendDefect>1e-4 || support>1e-4))) ++unqualified;
                    steps << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ','
                          << static_cast<int>(diagnostic.status) << ',' << diagnostic.converged << ','
                          << diagnostic.maximumLinearResidual/forceScale << ',' << diagnostic.maximumAngularResidual/(forceScale*fixture.rest.totalLength)
                          << ',';
                    if(implicit) steps << "NA,NA,NA";
                    else steps << diagnostic.maximumShearDefect << ',' << diagnostic.maximumBendDefect << ',' << support;
                    steps << ',' << diagnostic.predictorResidual << ','
                          << diagnostic.energy << ',' << diagnostic.linearKineticEnergy << ',' << diagnostic.angularKineticEnergy << '\n';
                    ++completed;
                    StateRows(states,fixture,"h"+std::to_string(refinement)+"_s"+std::to_string(sweeps)+"_tick"+std::to_string(tick),state);
                    for(size_t node=0;node<state.positions.size();++node)
                    {
                        dynamics << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << tick << ',' << node;
                        for(const Vec3& value:{state.positions[node],state.velocities[node]})
                            for(int axis=0;axis<3;++axis) dynamics << ',' << value[axis];
                        if(node<state.orientations.size())
                        {
                            const auto& orientation=state.orientations[node];
                            dynamics << ',' << orientation.w() << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z();
                            for(int axis=0;axis<3;++axis) dynamics << ',' << state.angularVelocities[node][axis];
                        }
                        else dynamics << ",NA,NA,NA,NA,NA,NA,NA";
                        dynamics << '\n';
                    }
                    if(diagnostic.status!=Status::Success)
                    {
                        ++failures;
                        auto failure=Open(directory/"failure.json");
                        failure << Json{{"guide",fixture.id},{"points",fixture.count},{"h",input.stepSeconds},{"tick",tick},
                            {"status",static_cast<int>(diagnostic.status)},{"attemptedSteps",completed},
                            {"acceptedSteps",completed-1},{"statePolicy","ROLLED_BACK; failed internal candidate requires solver trace"},
                            {"physicalDiagnostics","NOT_VALID_ON_EARLY_FAILURE"}}.dump(2);
                        auto summary=Open(directory/"summary.json");
                        summary << Json{{"numerical","FAIL"},{"attemptedSteps",completed},{"acceptedSteps",completed-1},
                            {"behavior","NOT_RUN"},{"independentAudit","PENDING"}}.dump(2);
                        throw std::runtime_error("STEP_STATUS_FAILURE_PRESERVED");
                    }
                }
                finals[sweepIndex][refinement]=state;
                if(!perf && !reference) groupFinal[3*sweepIndex+refinement].push_back(state);
                double displacement=0,tangentDifference=0,turnDifference=0;
                Vec3 centroidShift=Vec3::Zero();
                for(size_t node=0;node<state.positions.size();++node)
                {
                    const Vec3 delta=state.positions[node]-fixture.rest.positions[node];
                    displacement=std::max(displacement,delta.norm()/fixture.rest.totalLength);
                    centroidShift+=delta/static_cast<double>(state.positions.size());
                }
                for(size_t segment=0;segment<fixture.rest.lengths.size();++segment)
                {
                    const Vec3 authored=(fixture.rest.positions[segment+1]-fixture.rest.positions[segment]).normalized();
                    const Vec3 current=(state.positions[segment+1]-state.positions[segment]).normalized();
                    tangentDifference=std::max(tangentDifference,std::acos(std::clamp(authored.dot(current),-1.0,1.0)));
                    if(segment+1<fixture.rest.lengths.size())
                    {
                        const Vec3 authoredNext=(fixture.rest.positions[segment+2]-fixture.rest.positions[segment+1]).normalized();
                        const Vec3 currentNext=(state.positions[segment+2]-state.positions[segment+1]).normalized();
                        turnDifference=std::max(turnDifference,std::abs(std::acos(std::clamp(authored.dot(authoredNext),-1.0,1.0))-
                            std::acos(std::clamp(current.dot(currentNext),-1.0,1.0))));
                    }
                }
                shape << fixture.id << ',' << fixture.count << ',' << input.stepSeconds << ',' << sweeps << ',' << displacement << ','
                      << centroidShift.norm() << ',' << .015*fixture.rest.totalLength << ',' << tangentDifference << ',' << turnDifference
                      << ",SHORT_NOT_SETTLED_DIAGNOSTIC\n";
            }
            ++sweepIndex;
        }
        if(!perf && !reference)
        {
            auto difference=[&](const State& first,const State& second)
            {
                double position=0,angle=0;
                for(size_t node=0;node<first.positions.size();++node)
                    position+=(first.positions[node]-second.positions[node]).squaredNorm();
                for(size_t segment=0;segment<first.orientations.size();++segment)
                    angle=std::max(angle,Log(first.orientations[segment].conjugate()*second.orientations[segment]).norm());
                return std::array<double,2>{std::sqrt(position)/fixture.rest.totalLength,angle};
            };
            for(int direction=0;direction<(implicit?1:2);++direction) for(int level=0;level<(implicit?1:3);++level)
            {
                const auto coarse=direction==0?difference(finals[level][0],finals[level][1]):difference(finals[0][level],finals[1][level]);
                const auto fine=direction==0?difference(finals[level][1],finals[level][2]):difference(finals[1][level],finals[2][level]);
                const bool resolved=coarse[0]>1e-10 && fine[0]>1e-10 && coarse[1]>1e-9 && fine[1]>1e-9;
                const bool decreasing=coarse[0]>fine[0] && coarse[1]>fine[1];
                const char* status=!resolved?"ORDER_UNRESOLVED":decreasing?"DECREASING":"NONDECREASING";
                if(!resolved) ++unresolved;else if(!decreasing) ++unqualified;
                endpoints << fixture.id << ',' << fixture.count << ',' << (direction==0?"h":"sweeps") << ',' << level << ','
                          << coarse[0] << ',' << fine[0] << ',' << coarse[1] << ',' << fine[1] << ',' << status << '\n';
            }
        }
    }
    if(!perf && !reference) for(size_t cell=0;cell<(implicit?3:groupFinal.size());++cell)
        Group(groups,fixtures,"short_endpoint_cell"+std::to_string(cell),groupFinal[cell]);
    auto summary=Open(directory/"summary.json");
    summary << Json{{"stage",reference?"fixed_finer_reference":perf?"CPU_perf_diagnostic":"short_step_diagnostic"},{"status",perf?"CPU_DIAGNOSTIC_ONLY":unqualified?"NUMERICAL_NOT_QUALIFIED":unresolved?"ORDER_UNRESOLVED":"CANDIDATE_GATES_MET"},
        {"steps",completed},{"failures",failures},{"wallSeconds",std::chrono::duration<double>(Clock::now()-started).count()},
        {"solverSeconds",solverSeconds},{"CPU_solver_microseconds_per_guide_step",completed?solverSeconds*1e6/completed:0},
        {"unqualifiedChecks",unqualified},{"unresolvedOrders",unresolved},
        {"solver",endpointAir?"implicitMomentumProgressiveAirContinuation":continuation?"implicitMomentumProgressiveContinuation9":progressive?"implicitMomentumProgressiveV3":implicit?"implicitMomentumV2":"cosseratXpbdV1"},
        {"airVelocityTime",endpointAir?"ENDPOINT_VNEW_FROZEN_OLD_TANGENT":"EXPLICIT_VOLD"},
        {"kineticMathVersion",fixtures.front().kineticMathVersion},
        {"load",wind?"physical_air20mps":"probe20mps2"},
        {"GPUPerformance","NOT_MEASURED"},{"behavior","NOT_RUN"},{"independentAudit","PENDING"}}.dump(2);
    return 0;
}

void DynamicRows(std::ostream& output,const Fixture& fixture,const char* kind,int tick,const State& state)
{
    for(size_t node=0;node<state.positions.size();++node)
    {
        output << fixture.id << ',' << fixture.count << ',' << kind << ',' << 1.0/240.0 << ",0," << tick << ',' << tick/240.0 << ',' << node;
        for(const Vec3& value:{state.positions[node],state.velocities[node]})
            for(int axis=0;axis<3;++axis) output << ',' << value[axis];
        if(node<state.orientations.size())
        {
            const auto& orientation=state.orientations[node];
            output << ',' << orientation.w() << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z();
            for(int axis=0;axis<3;++axis) output << ',' << state.angularVelocities[node][axis];
        }
        else output << ",NA,NA,NA,NA,NA,NA,NA";
        output << '\n';
    }
}

int Long(const std::vector<Fixture>& fixtures,const std::filesystem::path& directory,bool progressive,bool wind,bool continuation=false,
         double wallBudgetSeconds=900,std::uintmax_t byteBudget=4294967296ULL,bool workGuard=false,bool endpointAir=false)
{
    constexpr double step=1.0/240.0;
    constexpr int planned=3240;
    auto states=Open(directory/"states.csv");
    states << "guide,points,kind,h,sweeps,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz,wx,wy,wz\n";
    auto steps=Open(directory/"steps.csv");
    steps << "guide,points,tick,time,status,rx_scaled,rq_scaled,internal_residual,Newton,backtracks,linear_residual,pin_error,root_q_error,qnorm,max_adjacent_log,max_step_log,probe_ax,air_speed_mps,balance_scaled,work_consistency_scaled\n";
    auto residuals=Open(directory/"physical-residuals.csv");
    residuals << "guide,points,tick,node,pinned,rx,ry,rz,rqx,rqy,rqz\n";
    auto energy=Open(directory/"energy.csv");
    energy << "guide,points,tick,Uss,Ubt,Ut,Ug,Klinear_old,Klinear_next,Kangular_old,Kangular_next,Uold,Unext,Wexternal,D,Ilinear,Aangular,Iangular_signed,R,B,SB,residual_work,work_consistency\n";
    auto metrics=Open(directory/"metrics.csv");
    metrics << "guide,points,tick,time,D_authored,D_snapshot,speed_over_L,drift_from_2s,stretch_min,stretch_max,probe_ax,air_speed_mps\n";
    auto behavior=Open(directory/"behavior.csv");
    behavior << "guide,points,settled,static_speed,static_drift,Dmax,Ddyn,Trec,valid_suffix,Rtail,stretch,status\n";
    auto external=Open(directory/"external-forces.csv");
    external << "guide,points,tick,node,probe_acceleration_x,air_speed_x,fx,fy,fz\n";
    auto reactions=Open(directory/"root-reactions.csv");
    reactions << "guide,points,tick,valid,fx,fy,fz,body_tx,body_ty,body_tz\n";
    auto groups=Open(directory/"group-proxy.csv");
    groups << "kind,points,minx,miny,minz,maxx,maxy,maxz,cx,cy,cz,extentx,extenty,extentz,bbox_volume_proxy_m3,NN_min_m,NN_max_m\n";
    auto certificates=Open(directory/"work-certificates.csv");
    certificates << "guide,points,tick,free_work,upper_abs_work,root_constraint_work,Eref,frozen_target_balance,SB,work_bound_scaled,balance_scaled,identity_scaled,finite,accepted\n";
    auto airRows=Open(directory/"endpoint-air.csv");
    airRows << "guide,points,tick,node,old_tx,old_ty,old_tz,Ux,Uy,Uz,other_fx,other_fy,other_fz,air_fx,air_fy,air_fz,air_work,other_work\n";
    auto materialRows=Open(directory/"material-force.csv");
    materialRows << "guide,points,tick,segment,length_strain,material_shear_x,material_shear_y,material_axial_strain,traction_body_x,traction_body_y,traction_body_z\n";
    std::vector<State> authoredGroup,settledGroup,finalGroup;
    int accepted=0,behaviorFailures=0;
    const auto started=Clock::now();
    int activeGuide=-1,activePoints=0,activeTick=0;
    try
    {
        for(const auto& fixture:fixtures)
        {
            activeGuide=fixture.id;activePoints=fixture.count;activeTick=0;
            const auto& rest=fixture.rest;
            ProgressiveSupport profile;
            if(progressive) profile=BuildProgressiveSupport(rest);
            const ProgressiveSupport* profilePointer=progressive?&profile:nullptr;
            const double forceScale=std::accumulate(rest.masses.begin(),rest.masses.end(),0.0)*9.8+
                *std::max_element(rest.supportStiffness.begin(),rest.supportStiffness.end())*rest.totalLength;
            const double torqueScale=forceScale*rest.totalLength;
            const double energyScale=forceScale*rest.totalLength;
            StepInput input;input.stepSeconds=step;
            input.root.position=rest.positions.front();input.root.orientation=rest.orientations.front();
            input.externalForces.assign(rest.positions.size(),Vec3::Zero());
            State state=MakeAuthoredState(rest);
            std::vector<State> history{state};
            authoredGroup.push_back(state);
            DynamicRows(states,fixture,"initial",0,state);
            auto potential=[&](const State& value)
            {
                double result=EvaluateEnergy(rest,value).Total()+Support(rest,value,input.root,profilePointer).potential;
                for(size_t node=0;node<value.positions.size();++node)
                    result-=rest.masses[node]*input.gravity.dot(value.positions[node]);
                return result;
            };
            for(int tick=1;tick<=planned;++tick)
            {
                activeTick=tick;
                if(std::chrono::duration<double>(Clock::now()-started).count()>wallBudgetSeconds)
                    throw std::runtime_error("INCOMPLETE_LONG_WALL_BUDGET");
                if(accepted%100==0)
                {
                    const auto bytes=OutputBytes({&states,&steps,&residuals,&energy,&metrics,&behavior,&groups,&external,&reactions,&certificates,&airRows,&materialRows});
                    if(bytes>byteBudget) throw std::runtime_error("INCOMPLETE_LONG_OUTPUT_BUDGET");
                }
                const double probe=!wind && tick>=721 && tick<=840?20:0;
                const double airSpeed=wind && tick>=721 && tick<=840?20:0;
                for(size_t node=0;node<rest.masses.size();++node)
                {
                    if(endpointAir) input.externalForces[node]=Vec3::Zero();
                    else if(wind)
                    {
                        input.externalForces[node]=ProgressiveAirDragForce(profile,rest.masses[node],
                            Vec3(airSpeed,0,0)-state.velocities[node],Tangent(state,node));
                    }
                    else input.externalForces[node]=rest.masses[node]*Vec3(probe,0,0);
                    if(!endpointAir)
                    {
                        external << fixture.id << ',' << fixture.count << ',' << tick << ',' << node << ',' << probe << ',' << airSpeed;
                        for(int axis=0;axis<3;++axis) external << ',' << input.externalForces[node][axis];
                        external << '\n';
                    }
                }
                const State old=state;
                DynamicRows(states,fixture,"old",tick-1,old);
                ResidualWorkCertificate certificate;
                const auto diagnostic=endpointAir?
                    StepImplicitMomentumProgressiveAirContinuation(rest,state,input,profile,Vec3(airSpeed,0,0),certificate):
                    Advance(rest,state,input,profilePointer,continuation,workGuard?&certificate:nullptr);
                if(workGuard)
                    certificates << fixture.id << ',' << fixture.count << ',' << tick << ',' << certificate.freeResidualWork << ','
                                 << certificate.freeResidualWorkUpper << ',' << certificate.rootConstraintWork << ',' << certificate.energyReference << ','
                                 << certificate.frozenTargetBalance << ',' << certificate.ledgerScale << ',' << certificate.workBoundScaled << ','
                                 << certificate.balanceScaled << ',' << certificate.identityScaled << ',' << certificate.finite << ',' << certificate.accepted << '\n';
                reactions << fixture.id << ',' << fixture.count << ',' << tick << ',' << (diagnostic.status==Status::Success);
                for(const Vec3& value:{diagnostic.rootForce,diagnostic.rootBodyTorque})
                    for(int axis=0;axis<3;++axis) reactions << ',' << value[axis];
                reactions << '\n';
                DynamicRows(states,fixture,"candidate",tick,state);
                const auto elastic=EvaluateEnergy(rest,state);
                const auto support=Support(rest,state,input.root,profilePointer);
                State springState=state;
                springState.velocities.assign(state.velocities.size(),Vec3::Zero());
                const auto spring=Support(rest,springState,input.root,profilePointer);
                const auto physical=endpointAir?
                    EvaluateProgressiveAirResidual(rest,old,state,input,profile,FrozenTangentAir{Vec3(airSpeed,0,0),1}):
                    Physical(rest,old,state,input,profilePointer);
                double oldK=0,nextK=0,oldAngular=0,nextAngular=0,inertial=0,angularWork=0;
                double work=0,conservativeWork=0,residualWork=0,pinError=0,qnorm=0,adjacent=0,stepAngle=0,gravity=0;
                double rx=0,rq=0;
                bool finite=true;
                for(size_t node=0;node<state.positions.size();++node)
                {
                    const Vec3 delta=state.positions[node]-old.positions[node];
                    finite=finite && state.positions[node].allFinite() && state.velocities[node].allFinite();
                    oldK+=.5*rest.masses[node]*old.velocities[node].squaredNorm();
                    nextK+=.5*rest.masses[node]*state.velocities[node].squaredNorm();
                    inertial+=.5*rest.masses[node]*(state.velocities[node]-old.velocities[node]).squaredNorm();
                    work+=input.externalForces[node].dot(delta);
                    if(endpointAir)
                    {
                        const auto air=EvaluateFrozenTangentAirDrag(old,state,node,rest.masses[node],step,profile,Vec3(airSpeed,0,0));
                        work+=air.force.dot(delta);
                        external << fixture.id << ',' << fixture.count << ',' << tick << ',' << node << ",0," << airSpeed;
                        for(int axis=0;axis<3;++axis) external << ',' << air.force[axis];
                        external << '\n';
                        airRows << fixture.id << ',' << fixture.count << ',' << tick << ',' << node;
                        for(const Vec3& value:{air.tangent,Vec3(airSpeed,0,0),input.externalForces[node],air.force})
                            for(int axis=0;axis<3;++axis) airRows << ',' << value[axis];
                        airRows << ',' << air.force.dot(delta) << ',' << input.externalForces[node].dot(delta) << '\n';
                    }
                    conservativeWork+=(elastic.forces[node]+spring.forces[node]+rest.masses[node]*input.gravity).dot(delta);
                    residualWork+=physical[node].head<3>().dot(delta);
                    gravity-=rest.masses[node]*input.gravity.dot(state.positions[node]);
                    if(rest.inverseMasses[node]==0)
                        pinError=std::max(pinError,(state.positions[node]-SampleRootSupport(input.root,rest.rootLocalPositions[node]).position).norm());
                    else rx=std::max(rx,physical[node].head<3>().norm()/forceScale);
                    residuals << fixture.id << ',' << fixture.count << ',' << tick << ',' << node << ',' << (rest.inverseMasses[node]==0);
                    for(int axis=0;axis<6;++axis) residuals << ',' << physical[node][axis];
                    residuals << '\n';
                }
                for(size_t segment=0;segment<state.orientations.size();++segment)
                {
                    if(endpointAir)
                    {
                        const auto shear=EvaluateShearBlock(rest,state,segment);
                        const Vec3 traction=rest.stretchShear[segment].cwiseProduct(shear.constraint);
                        materialRows << fixture.id << ',' << fixture.count << ',' << tick << ',' << segment << ','
                            << (state.positions[segment+1]-state.positions[segment]).norm()/rest.lengths[segment]-1;
                        for(const Vec3& value:{shear.constraint,traction})
                            for(int axis=0;axis<3;++axis) materialRows << ',' << value[axis];
                        materialRows << '\n';
                    }
                    const Vec3 theta=Log(old.orientations[segment].conjugate()*state.orientations[segment]);
                    const Vec3 transported=state.orientations[segment].conjugate()*
                        (old.orientations[segment]*rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
                    oldAngular+=.5*old.angularVelocities[segment].dot(rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
                    nextAngular+=.5*state.angularVelocities[segment].dot(rest.inertias[segment].cwiseProduct(state.angularVelocities[segment]));
                    angularWork+=state.angularVelocities[segment].dot(rest.inertias[segment].cwiseProduct(state.angularVelocities[segment])-transported);
                    conservativeWork+=elastic.bodyTorques[segment].dot(theta);
                    residualWork+=physical[segment].tail<3>().dot(theta);
                    if(segment) rq=std::max(rq,physical[segment].tail<3>().norm()/torqueScale);
                    qnorm=std::max(qnorm,std::abs(state.orientations[segment].norm()-1));
                    stepAngle=std::max(stepAngle,theta.norm());
                    if(segment+1<state.orientations.size())
                        adjacent=std::max(adjacent,Log(state.orientations[segment].conjugate()*state.orientations[segment+1]).norm());
                    finite=finite && state.orientations[segment].coeffs().allFinite() && state.angularVelocities[segment].allFinite();
                }
                const double rootQ=(state.orientations.front().coeffs()-input.root.orientation.coeffs()).norm();
                const double oldU=potential(old),nextU=elastic.Total()+support.potential+gravity;
                const double remainder=nextU-oldU+conservativeWork;
                const double angularDiscrete=angularWork-(nextAngular-oldAngular);
                const double damping=step*support.dissipation;
                const double balance=nextK+nextAngular+nextU-oldK-oldAngular-oldU-work+damping+inertial+angularDiscrete-remainder;
                const double scale=energyScale+std::abs(nextK+nextAngular+nextU)+std::abs(oldK+oldAngular+oldU)+
                    std::abs(work)+std::abs(damping)+std::abs(inertial)+std::abs(angularDiscrete)+std::abs(remainder);
                const bool pass=diagnostic.status==Status::Success && (!workGuard || certificate.accepted) && finite && rx<=1e-2 && rq<=1e-2 &&
                    diagnostic.normalizedResidual<=1e-8 && pinError==0 && rootQ==0 && qnorm<=1e-12 &&
                    adjacent<RotationLimit && stepAngle<RotationLimit &&
                    std::abs(balance)/scale<=1e-10 && std::abs(balance-residualWork)/scale<=1e-10;
                steps << fixture.id << ',' << fixture.count << ',' << tick << ',' << tick*step << ',' << (pass?"PASS":"FAIL") << ','
                      << rx << ',' << rq << ',' << diagnostic.normalizedResidual << ',' << diagnostic.newtonIterations << ','
                      << diagnostic.backtracks << ',' << diagnostic.linearSolveResidual << ',' << pinError << ',' << rootQ << ','
                      << qnorm << ',' << adjacent << ',' << stepAngle << ',' << probe << ',' << airSpeed << ',' << std::abs(balance)/scale << ','
                      << std::abs(balance-residualWork)/scale << '\n';
                energy << fixture.id << ',' << fixture.count << ',' << tick << ',' << elastic.stretchShear << ',' << elastic.bendTwist
                       << ',' << support.potential << ',' << gravity << ',' << oldK << ',' << nextK << ',' << oldAngular << ',' << nextAngular
                       << ',' << oldU << ',' << nextU << ',' << work << ',' << damping << ',' << inertial << ',' << angularWork
                       << ',' << angularDiscrete << ',' << remainder << ',' << balance << ',' << scale << ',' << residualWork
                       << ',' << balance-residualWork << '\n';
                if(!pass)
                {
                    auto failedOld=Open(directory/"failed-old-state.csv");
                    failedOld << "guide,points,kind,h,sweeps,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz,wx,wy,wz\n";
                    DynamicRows(failedOld,fixture,"original_old",tick-1,old);
                    State failedCandidate=state;
                    Json traceResult{{"originalStatus",static_cast<int>(diagnostic.status)},
                        {"originalInternalResidual",diagnostic.normalizedResidual},{"tick",tick},
                        {"published",false},{"candidateSource","unpublished validation candidate"}};
                    if(diagnostic.status!=Status::Success && progressive && continuation)
                    {
                        State replay=old;
                        ImplicitTrace trace;
                        ResidualWorkCertificate replayCertificate;
                        const auto traced=endpointAir?
                            StepImplicitMomentumProgressiveAirContinuation(rest,replay,input,profile,Vec3(airSpeed,0,0),replayCertificate,&trace):workGuard?
                            StepImplicitMomentumProgressiveContinuationWorkGuard(rest,replay,input,profile,replayCertificate,&trace):
                            StepImplicitMomentumProgressiveContinuationWithTrace(rest,replay,input,profile,trace);
                        failedCandidate=trace.finalCandidate;
                        traceResult["traceReplayStatus"]=static_cast<int>(traced.status);
                        traceResult["traceReplayInternalResidual"]=traced.normalizedResidual;
                        traceResult["traceReason"]=trace.failureReason;
                        traceResult["iterationCount"]=trace.iterations.size();
                        traceResult["candidateSource"]="solver internal candidate before velocity reconstruction; original failure preserved";
                        if(!trace.iterations.empty())
                        {
                            const auto& lastIteration=trace.iterations.back();
                            traceResult["lastLoadFraction"]=lastIteration.loadFraction;
                            auto failedSystem=Open(directory/"failed-system.csv");
                            failedSystem << "kind,block,row,column,value\n";
                            for(size_t block=0;block<lastIteration.system.diagonal.size();++block)
                            {
                                for(int row=0;row<6;++row) for(int column=0;column<6;++column)
                                    for(const auto& entry:{std::make_pair("lower",&lastIteration.system.lower[block]),
                                        std::make_pair("diagonal",&lastIteration.system.diagonal[block]),
                                        std::make_pair("upper",&lastIteration.system.upper[block])})
                                        failedSystem << entry.first << ',' << block << ',' << row << ',' << column << ','
                                                     << (*entry.second)(row,column) << '\n';
                                for(int axis=0;axis<6;++axis)
                                    failedSystem << "residual," << block << ',' << axis << ",0," << lastIteration.system.residual[block][axis] << '\n';
                            }
                        }
                    }
                    auto failedState=Open(directory/"failed-internal-candidate.csv");
                    failedState << "guide,points,kind,h,sweeps,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz,wx,wy,wz\n";
                    DynamicRows(failedState,fixture,"failed_internal_candidate",tick,failedCandidate);
                    auto traceOutput=Open(directory/"failure-trace.json");traceOutput << traceResult.dump(2);
                    throw std::runtime_error("LONG_NUMERICAL_GATE_FAILURE");
                }
                ++accepted;history.push_back(state);
                if(tick%600==0)
                {
                    states.flush();steps.flush();energy.flush();residuals.flush();
                    std::cout << "long guide=" << fixture.id << " N=" << fixture.count << " tick=" << tick << " seconds="
                              << std::chrono::duration<double>(Clock::now()-started).count() << std::endl;
                }
            }
            settledGroup.push_back(history[720]);finalGroup.push_back(state);
            auto distance=[&](const State& value,const State& reference)
            {
                double maximum=0;
                for(size_t node=0;node<value.positions.size();++node)
                    maximum=std::max(maximum,(value.positions[node]-reference.positions[node]).norm()/rest.totalLength);
                return maximum;
            };
            double staticSpeed=0,staticDrift=0,dynamic=0,tail=0,stretch=0;
            int lastOutside=839;
            for(int tick=0;tick<=planned;++tick)
            {
                const double displacement=distance(history[tick],history[720]);
                const double drift=distance(history[tick],history[480]);
                double speed=0,low=0,high=0;
                for(size_t node=0;node<history[tick].velocities.size();++node)
                    speed=std::max(speed,history[tick].velocities[node].norm()/rest.totalLength);
                if(tick>=480 && tick<=720) {staticSpeed=std::max(staticSpeed,speed);staticDrift=std::max(staticDrift,drift);}
                if(tick>=720) dynamic=std::max(dynamic,displacement);
                if(tick>=840 && displacement>.005) lastOutside=tick;
                if(tick>=3000) tail=std::max(tail,displacement);
                for(size_t segment=0;segment<rest.lengths.size();++segment)
                {
                    const double strain=(history[tick].positions[segment+1]-history[tick].positions[segment]).norm()/rest.lengths[segment]-1;
                    low=std::min(low,strain);high=std::max(high,strain);
                }
                stretch=std::max({stretch,-low,high});
                metrics << fixture.id << ',' << fixture.count << ',' << tick << ',' << tick*step << ','
                        << distance(history[tick],history[0]) << ',' << displacement << ',' << speed << ','
                        << (tick>=480 && tick<=720?drift:0) << ',' << low << ',' << high << ','
                        << (!wind && tick>=721 && tick<=840?20:0) << ',' << (wind && tick>=721 && tick<=840?20:0) << '\n';
            }
            const bool settled=staticSpeed<=1e-3 && staticDrift<=1e-4;
            const double staticShape=distance(history[720],history[0]);
            const int suffix=lastOutside+1;
            const double recovery=(suffix-840)*step;
            const bool pass=settled && staticShape<=.015 && dynamic>=.02 && dynamic<=.08 &&
                suffix<=3000 && recovery<=.5 && tail<=.005 && stretch<=.01;
            behaviorFailures+=!pass;
            behavior << fixture.id << ',' << fixture.count << ',' << settled << ',' << staticSpeed << ',' << staticDrift << ','
                     << staticShape << ',' << dynamic << ',' << recovery << ',' << (suffix<=3000) << ',' << tail << ',' << stretch << ','
                     << (pass?"PASS":"FAIL") << '\n';
            behavior.flush();
        }
    }
    catch(const std::exception& error)
    {
        auto failure=Open(directory/"failure.json");
        failure << Json{{"reason",error.what()},{"guide",activeGuide},{"points",activePoints},{"tick",activeTick},{"acceptedSteps",accepted}}.dump(2);
        auto summary=Open(directory/"summary.json");
        summary << Json{{"numerical","FAIL"},{"acceptedSteps",accepted},{"behavior","INCOMPLETE"},{"independentAudit","PENDING"}}.dump(2);
        throw;
    }
    if(fixtures.size()>1)
    {
        Group(groups,fixtures,"authored",authoredGroup);
        Group(groups,fixtures,"qualified_or_unqualified_static_tick720",settledGroup);
        Group(groups,fixtures,"final",finalGroup);
    }
    auto summary=Open(directory/"summary.json");
    summary << Json{{"numerical","PASS"},{"behavior",behaviorFailures?"FAIL":"PASS"},{"behaviorFailures",behaviorFailures},{"acceptedSteps",accepted},
        {"solver",endpointAir?"implicitMomentumProgressiveAirContinuation":workGuard?"implicitMomentumProgressiveContinuationWorkGuard":continuation?"implicitMomentumProgressiveContinuation9":progressive?"implicitMomentumProgressiveV3":"implicitMomentumV2"},
        {"airVelocityTime",endpointAir?"ENDPOINT_VNEW_FROZEN_OLD_TANGENT":"EXPLICIT_VOLD"},
        {"kineticMathVersion",fixtures.front().kineticMathVersion},
        {"profile",progressive?"effectiveBundleProgressiveV3":"effectiveBundleV1"},{"load",wind?"physical_air20mps":"probe20mps2"},
        {"wallSeconds",std::chrono::duration<double>(Clock::now()-started).count()},{"independentAudit","PENDING"},{"fullHead","NOT_COVERED"}}.dump(2);
    return behaviorFailures?2:0;
}

int LongPriorityBatch(std::vector<Fixture> fixtures,const std::filesystem::path& directory,bool progressive,bool continuation,bool workGuard=false,bool endpointAir=false)
{
    std::stable_sort(fixtures.begin(),fixtures.end(),[](const Fixture& first,const Fixture& second)
    {
        auto rank=[](const Fixture& fixture)
        {
            return fixture.id==8924 && fixture.count==8?0:fixture.id==8924 && fixture.count==32?1:2;
        };
        return rank(first)<rank(second);
    });
    const auto started=Clock::now();
    Json results=Json::array();
    int accepted=0,numericalFailures=0,behaviorFailures=0;
    for(const auto& fixture:fixtures)
    {
        std::uintmax_t written=0;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(directory))
            if(entry.is_regular_file()) written+=entry.file_size();
        const std::uintmax_t maximumBytes=workGuard?1073741824ULL:4294967296ULL;
        const double remaining=(workGuard?300:900)-std::chrono::duration<double>(Clock::now()-started).count();
        if(remaining<=0 || written>=maximumBytes) throw std::runtime_error("INCOMPLETE_PRIORITY_BATCH_BUDGET");
        const auto caseDirectory=directory/("case-"+std::to_string(fixture.id)+"-n"+std::to_string(fixture.count));
        if(!std::filesystem::create_directory(caseDirectory)) throw std::runtime_error("CASE_OUTPUT_EXISTS");
        try
        {
            Long(std::vector<Fixture>{fixture},caseDirectory,progressive,true,continuation,remaining,maximumBytes-written,workGuard,endpointAir);
        }
        catch(const std::exception& error)
        {
            if(std::string(error.what())!="LONG_NUMERICAL_GATE_FAILURE") throw;
        }
        std::ifstream input(caseDirectory/"summary.json");
        Json result=Json::parse(input);
        result["guide"]=fixture.id;result["points"]=fixture.count;
        result["summaryPath"]=std::filesystem::absolute(caseDirectory/"summary.json").generic_string();
        result["statesPath"]=std::filesystem::absolute(caseDirectory/"states.csv").generic_string();
        if(std::filesystem::exists(caseDirectory/"failed-internal-candidate.csv"))
            result["failedCandidatePath"]=std::filesystem::absolute(caseDirectory/"failed-internal-candidate.csv").generic_string();
        accepted+=result.value("acceptedSteps",0);
        numericalFailures+=result.value("numerical",std::string("FAIL"))!="PASS";
        behaviorFailures+=result.value("behavior",std::string("INCOMPLETE"))=="FAIL";
        results.push_back(result);
        auto checkpoint=Open(directory/("checkpoint-"+std::to_string(fixture.id)+"-n"+std::to_string(fixture.count)+".json"));
        checkpoint << result.dump(2);checkpoint.close();
        if(results.size()==2)
        {
            auto priority=Open(directory/"priority-checkpoint.json");
            priority << Json{{"status","DIAGNOSTIC_PRIORITY_CASES_COMPLETED"},{"kineticMathVersion",2},{"cases",results},
                {"temporalAccuracy","ORIGINAL_SHORT_NONMONOTONIC_FLAGS_PRESERVED"},
                {"productionAdmission","NOT_ISSUED"}}.dump(2);
        }
        std::cout << "priority case guide=" << fixture.id << " N=" << fixture.count
                  << " numerical=" << result.value("numerical",std::string("FAIL"))
                  << " accepted=" << result.value("acceptedSteps",0) << std::endl;
    }
    auto summary=Open(directory/"summary.json");
    summary << Json{{"scope","DIAGNOSTIC_CASE_ISOLATED_AIR20_LONG"},{"kineticMathVersion",2},
        {"attemptedFixtures",results.size()},{"requestedFixtures",fixtures.size()},{"acceptedSteps",accepted},
        {"numericalFailureFixtures",numericalFailures},{"behaviorFailureFixtures",behaviorFailures},
        {"cases",results},{"temporalAccuracy","ORIGINAL_SHORT_NONMONOTONIC_FLAGS_PRESERVED"},
        {"wallSeconds",std::chrono::duration<double>(Clock::now()-started).count()},
        {"productionAdmission","NOT_ISSUED"},{"independentAudit","PENDING"}}.dump(2);
    return numericalFailures || behaviorFailures?2:0;
}
}

int main(int argc,char** argv)
{
    std::filesystem::path directory;
    bool ownedDirectory=false;
    try
    {
        const bool endpointAirMode=argc>=2 && std::string(argv[1])=="--implicit-air-time";
        const bool endpointLong=argc>=2 && std::string(argv[1])=="--implicit-air-long-worst";
        if((argc!=4 && argc!=6 && argc!=8) || (!endpointAirMode && !endpointLong && std::string(argv[1])!="--prepare" && std::string(argv[1])!="--short" && std::string(argv[1])!="--perf" && std::string(argv[1])!="--phase-reference" && std::string(argv[1])!="--phase-reference-3840" && std::string(argv[1])!="--long" && std::string(argv[1])!="--wind-long" && std::string(argv[1])!="--wind-short" && std::string(argv[1])!="--air-reference1920" && std::string(argv[1])!="--air-reference3840" && std::string(argv[1])!="--wind-long-work-remainder" && std::string(argv[1])!="--section-air-reference1920" && std::string(argv[1])!="--section-air-reference3840"))
            throw std::runtime_error("usage: vl_rod_groom_validation --prepare|--short|--perf|--phase-reference|--phase-reference-3840 INVENTORY_JSON NEW_DIRECTORY [--solver implicitMomentumV2]");
        const bool implicit=argc>=6;
        const bool sectionInertia=argc==8;
        if(sectionInertia && (std::string(argv[6])!="--kinetic-math-version" || std::string(argv[7])!="2"))
            throw std::runtime_error("UNKNOWN_KINETIC_MATH_VERSION");
        const bool workGuard=implicit && std::string(argv[5])=="implicitMomentumProgressiveContinuationWorkGuard";
        const bool endpointAir=implicit && std::string(argv[5])=="implicitMomentumProgressiveAirContinuation";
        const bool continuation=implicit && (std::string(argv[5])=="implicitMomentumProgressiveContinuation9" || workGuard || endpointAir);
        const bool progressive=implicit && (std::string(argv[5])=="implicitMomentumProgressiveV3" || continuation);
        if(implicit && (std::string(argv[4])!="--solver" || (std::string(argv[5])!="implicitMomentumV2" && !progressive)))
            throw std::runtime_error("UNKNOWN_SOLVER");
        if(workGuard && std::string(argv[1])!="--wind-long-work-remainder" && std::string(argv[1])!="--prepare")
            throw std::runtime_error("WORK_GUARD_SCOPE_IS_FIXED_REMAINDER_OR_PREPARE");
        directory=argv[3];
        if(!std::filesystem::create_directories(directory)) throw std::runtime_error("OUTPUT_EXISTS");
        ownedDirectory=true;
        auto fixtures=Load(argv[2],sectionInertia);
        if(endpointLong)
        {
            if(!endpointAir || !sectionInertia) throw std::runtime_error("ENDPOINT_LONG_REQUIRES_SECTION2_AIR_WORK_GUARD");
            fixtures.erase(std::remove_if(fixtures.begin(),fixtures.end(),[](const Fixture& fixture)
                {return fixture.count!=32 || (fixture.id!=39210 && fixture.id!=4006);}),fixtures.end());
            return LongPriorityBatch(fixtures,directory,true,true,true,true);
        }
        if(endpointAir || endpointAirMode)
        {
            if(!endpointAir || !endpointAirMode || !sectionInertia)
                throw std::runtime_error("ENDPOINT_AIR_REQUIRES_FIXED_TIME_SCOPE_AND_SECTION2");
            fixtures.erase(std::remove_if(fixtures.begin(),fixtures.end(),[](const Fixture& fixture)
                {return fixture.count!=32 || (fixture.id!=33486 && fixture.id!=120);}),fixtures.end());
            const auto started=Clock::now();
            Json tiers=Json::array();
            for(int level=0;level<5;++level)
            {
                const auto tier=directory/("hz"+std::to_string(240*(1<<level)));
                std::filesystem::create_directory(tier);
                Short(fixtures,tier,false,true,true,level,true,true,true,true);
                std::ifstream stream(tier/"summary.json");
                Json result;stream>>result;tiers.push_back(result);
                std::uintmax_t bytes=0;
                for(const auto& entry:std::filesystem::recursive_directory_iterator(directory))
                    if(entry.is_regular_file()) bytes+=entry.file_size();
                if(bytes>134217728ULL) throw std::runtime_error("INCOMPLETE_ENDPOINT_AIR_OUTPUT_BUDGET");
                if(std::chrono::duration<double>(Clock::now()-started).count()>120)
                    throw std::runtime_error("INCOMPLETE_ENDPOINT_AIR_BUDGET");
            }
            auto summary=Open(directory/"summary.json");
            summary << Json{{"identity","SECTION2_ENDPOINT_AIR_FIXED372_DIAGNOSTIC"},{"plannedSteps",372},
                {"tiers",tiers},{"wallSeconds",std::chrono::duration<double>(Clock::now()-started).count()},
                {"independentAudit","PENDING"},{"productionAdmission","NOT_GRANTED"}}.dump(2);
            return 0;
        }
        if(std::string(argv[1])=="--wind-long-work-remainder")
        {
            if(!sectionInertia || !workGuard) throw std::runtime_error("WORK_REMAINDER_REQUIRES_EXPLICIT_SECTION2_WORK_GUARD");
            fixtures.erase(std::remove_if(fixtures.begin(),fixtures.end(),[](const Fixture& fixture)
            {
                return !((fixture.id==39210 && (fixture.count==8 || fixture.count==16 || fixture.count==32)) ||
                    (fixture.id==5651 && fixture.count==8));
            }),fixtures.end());
            return LongPriorityBatch(fixtures,directory,progressive,continuation,true);
        }
        const bool sectionAirReference=std::string(argv[1])=="--section-air-reference1920" || std::string(argv[1])=="--section-air-reference3840";
        const bool airReference=std::string(argv[1])=="--air-reference1920" || std::string(argv[1])=="--air-reference3840" || sectionAirReference;
        const bool reference=std::string(argv[1])=="--phase-reference" || std::string(argv[1])=="--phase-reference-3840" || airReference;
        if(reference)
        {
            if(!implicit) throw std::runtime_error("REFERENCE_REQUIRES_IMPLICIT_V2");
            if(airReference)
            {
                if(!continuation) throw std::runtime_error("AIR_REFERENCE_REQUIRES_CONTINUATION9");
                if(sectionAirReference && !sectionInertia) throw std::runtime_error("SECTION_REFERENCE_REQUIRES_KINETIC_VERSION2");
                const std::vector<std::pair<int,int>> selected=sectionAirReference?
                    std::vector<std::pair<int,int>>{{33486,32},{120,32}}:
                    std::vector<std::pair<int,int>>{{33486,8},{39210,8},{5651,8},{5660,8},{120,8},
                        {584,16},{33486,16},{120,16},{584,32},{33486,32},{120,32}};
                fixtures.erase(std::remove_if(fixtures.begin(),fixtures.end(),[&](const Fixture& fixture)
                    {return std::find(selected.begin(),selected.end(),std::make_pair(fixture.id,fixture.count))==selected.end();}),fixtures.end());
            }
            else fixtures.erase(std::remove_if(fixtures.begin(),fixtures.end(),
                [](const Fixture& fixture){return fixture.id!=4006 || fixture.count!=8;}),fixtures.end());
        }
        if(std::string(argv[1])=="--wind-short" && !progressive) throw std::runtime_error("WIND_REQUIRES_PROGRESSIVE_PROFILE");
        if(std::string(argv[1])=="--long" || std::string(argv[1])=="--wind-long")
        {
            if(!implicit) throw std::runtime_error("LONG_REQUIRES_IMPLICIT_V2");
            if(std::string(argv[1])=="--wind-long" && !progressive) throw std::runtime_error("WIND_REQUIRES_PROGRESSIVE_PROFILE");
            if(std::string(argv[1])=="--wind-long" && sectionInertia)
                return LongPriorityBatch(fixtures,directory,progressive,continuation);
            return Long(fixtures,directory,progressive,std::string(argv[1])=="--wind-long",continuation);
        }
        return std::string(argv[1])=="--prepare"?Prepare(fixtures,directory,progressive):Short(fixtures,directory,std::string(argv[1])=="--perf",implicit,reference,
            std::string(argv[1])=="--phase-reference-3840" || std::string(argv[1])=="--air-reference3840" || std::string(argv[1])=="--section-air-reference3840"?4:3,
            progressive,std::string(argv[1])=="--wind-short" || airReference,continuation);
    }
    catch(const std::exception& error)
    {
        if(ownedDirectory && !std::filesystem::exists(directory/"summary.json"))
        {
            auto failure=Open(directory/"failure.txt");failure << error.what() << '\n';
        }
        std::cerr << error.what() << '\n';return 2;
    }
}
