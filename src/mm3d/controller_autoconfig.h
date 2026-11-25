// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <optional>
#include <vector>
#include "common/settings.h"
#include "common/param_package.h"

namespace MM3D::ControllerAutoConfig {
template<class Bindings> bool SameBindings(const Bindings& a,const Bindings& b) {
    for(unsigned i=0;i<a.size();++i) {
        if(a[i]==b[i])continue;
        const Common::ParamPackage first(a[i]),second(b[i]);
        if(!first.Has("engine")||!second.Has("engine")||first.Serialize()!=second.Serialize())return false;
    }
    return true;
}
inline bool Untouched(const Settings::InputProfile& p,
    const std::array<std::string,Settings::NativeButton::NumButtons>& buttons,
    const std::array<std::string,Settings::NativeAnalog::NumAnalogs>& analogs) {
    // Motion, touch, profile name and additional profiles do not establish a
    // custom gamepad mapping. Copy those preferences unchanged when mapping.
    return p.maptype==Settings::InputMappingType::GuidPort&&
        SameBindings(p.buttons,buttons)&&SameBindings(p.analogs,analogs);
}
// Build in a copy, then publish once all game controls have mappings. Retain
// keyboard bindings for emulator-only inputs not exposed by a standard gamepad.
template<class Buttons,class Analogs> bool Map(Settings::InputProfile& profile,
    const Common::ParamPackage& controller,Buttons buttons,Analogs analogs) {
    if(controller.Get("engine","")!="sdl"||controller.Get("api","")!="controller"||
       controller.Get("guid","").empty())return false;
    auto result=profile;
    for(int i=0;i<Settings::NativeButton::NumButtons;++i) {
        const auto binding=buttons(controller,i);
        if(binding.Has("engine"))result.buttons[i]=binding.Serialize();
        else if(i<=Settings::NativeButton::Select||i==Settings::NativeButton::ZL||i==Settings::NativeButton::ZR)return false;
    }
    for(int i=0;i<Settings::NativeAnalog::NumAnalogs;++i) {
        const auto binding=analogs(controller,i);if(!binding.Has("engine"))return false;
        result.analogs[i]=binding.Serialize();
    }
    result.name="Controller (automatic)";
    result.maptype=Settings::InputMappingType::GuidPort;
    profile=std::move(result);return true;
}
inline bool SameDevice(const Common::ParamPackage& a,const Common::ParamPackage& b) {
    return a.Get("guid","")==b.Get("guid","")&&a.Get("port",0)==b.Get("port",0);
}
template<class Buttons,class Analogs> bool Managed(const Settings::InputProfile& p,Buttons buttons,Analogs analogs) {
    if(p.name!="Controller (automatic)"||p.maptype!=Settings::InputMappingType::GuidPort)return false;
    auto candidate=Common::ParamPackage(p.buttons[Settings::NativeButton::A]);
    auto expected=p;
    // Reconstruct what we originally generated, including the face layout.
    return Map(expected,candidate,buttons,analogs)&&SameBindings(p.buttons,expected.buttons)&&
        SameBindings(p.analogs,expected.analogs);
}
// An automatic profile for a controller that is not connected, e.g. a user
// folder copied from another device, where the same controller has another
// SDL GUID. Its bindings cannot be checked against the controller (Managed),
// and they cannot work either; remap it to a connected controller.
inline bool Stale(const Settings::InputProfile& p,const std::vector<Common::ParamPackage>& controllers) {
    if(p.name!="Controller (automatic)"||p.maptype!=Settings::InputMappingType::GuidPort)return false;
    const Common::ParamPackage bound(p.buttons[Settings::NativeButton::A]);
    if(bound.Get("engine","")!="sdl")return false;
    return std::none_of(controllers.begin(),controllers.end(),[&](const auto& c){
        return c.Get("guid","")==bound.Get("guid","");});
}
template<class Buttons,class Analogs> std::optional<Settings::InputProfile> Select(
    const Settings::InputProfile& current,std::vector<Common::ParamPackage> controllers,
    const std::array<std::string,Settings::NativeButton::NumButtons>& defaults,
    const std::array<std::string,Settings::NativeAnalog::NumAnalogs>& sticks,
    Buttons buttons,Analogs analogs) {
    const bool managed=Managed(current,buttons,analogs)||Stale(current,controllers);
    if(!managed&&!Untouched(current,defaults,sticks))return {};
    const Common::ParamPackage bound(current.buttons[Settings::NativeButton::A]);
    std::stable_sort(controllers.begin(),controllers.end(),[&](const auto& a,const auto& b) {
        const auto rank=[&](const auto& p){return (p.Get("active",0)?2:0)+(managed&&SameDevice(p,bound)?1:0);};
        return rank(a)>rank(b);
    });
    for(const auto& controller:controllers) {
        auto result=current;
        if(!Map(result,controller,buttons,analogs))continue;
        if(SameBindings(result.buttons,current.buttons)&&SameBindings(result.analogs,current.analogs))return {};
        return result;
    }
    return {};
}
// The controller whose gyroscope drives the console's motion sensors: the one
// the profile's A button is bound to, else the one used last, else any other.
inline std::optional<Common::ParamPackage> MotionController(const Settings::InputProfile& p,
    const std::vector<Common::ParamPackage>& controllers) {
    const Common::ParamPackage bound(p.buttons[Settings::NativeButton::A]);
    const bool sdl=bound.Get("engine","")=="sdl";
    std::optional<Common::ParamPackage> best;int best_rank=-1;
    for(const auto& controller:controllers) {
        if(!controller.Get("gyro",0))continue;
        const int rank=(sdl&&SameDevice(controller,bound)?2:0)+(controller.Get("active",0)?1:0);
        if(rank>best_rank){best=controller;best_rank=rank;}
    }
    return best;
}

}
