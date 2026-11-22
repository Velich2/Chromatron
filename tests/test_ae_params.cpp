// Exercise the actual Adobe plug-in entry point with a minimal SDK callback host.
// This checks parameter registration and supervised edits without launching Adobe.
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectSuites.h"
#include "SPBasic.h"
#include "ae/ChromatronParams.h"
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <utility>

using namespace chromatron::ae;
using Entry = PF_Err (*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef*[],PF_LayerDef*,void*);
struct Host {std::vector<PF_ParamDef> values{1}; A_long app=0;};
static PF_ParamUtilsSuite3 paramSuite{};
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
static PF_Err addParam(PF_ProgPtr ref,PF_ParamIndex index,PF_ParamDefPtr def)
{
    check(index==-1,"Unexpected registration index");
    reinterpret_cast<Host*>(ref)->values.push_back(*def);return PF_Err_NONE;
}
static PF_Err SPAPI updateUI(PF_ProgPtr ref,PF_ParamIndex index,const PF_ParamDef* def)
{
    auto& host=*reinterpret_cast<Host*>(ref);
    auto& values=host.values;
    check(index>0 && (size_t)index<values.size(),"Invalid UI parameter index");
    const auto allowed=PF_PUI_DISABLED | (host.app=='PrMr' ? PF_PUI_INVISIBLE : 0);
    check(((values[index].ui_flags^def->ui_flags)&~allowed)==0,"Unsupported dynamic UI flag sent to the Adobe host");
    values[index].ui_flags=def->ui_flags;return PF_Err_NONE;
}
static SPErr SPAPI acquire(const char* name,int32 version,const void** result)
{
    *result=nullptr;
    if(std::strcmp(name,kPFParamUtilsSuite)==0 && version==kPFParamUtilsSuiteVersion3) {*result=&paramSuite;return kSPNoError;}
    return -1;
}
static SPErr SPAPI release(const char*,int32) {return kSPNoError;}

static void exercise(Entry entry,A_long app)
{
    Host host;host.app=app;
    SPBasicSuite basic{};basic.AcquireSuite=acquire;basic.ReleaseSuite=release;
    PF_InData in{};PF_OutData out{};
    in.effect_ref=reinterpret_cast<PF_ProgPtr>(&host);in.inter.add_param=addParam;in.pica_basicP=&basic;
    in.appl_id=app;in.width=3840;in.height=2160;
    check(entry(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)==0,"ParamsSetup failed");
    check(out.num_params==P_NUM_PARAMS && host.values.size()==P_NUM_PARAMS,"Registered count does not match GPU indices");
    auto& v=host.values;
    auto available=[&](int index) {return (v[index].ui_flags&(app=='PrMr'?PF_PUI_INVISIBLE:PF_PUI_DISABLED))==0;};
    for (int index : {P_PALETTE, P_GRADIENT, P_USE_B_MIDDLE, P_MIDPOINT_A, P_MIDPOINT_B})
        check(v[index].flags & PF_ParamFlag_SUPERVISE, "Adobe will not notify the plug-in about this colour edit");
    for(const auto& item : std::vector<std::pair<int,const char*>>{{P_PALETTE,"Palette"},{P_PAIR,"Color Pair"},{P_GRADIENT,"Gradient"},
             {P_COLOR_A,"Color A"},{P_COLOR_A_END,"Color A End"},{P_COLOR_B,"Color B"},{P_COLOR_B_END,"Color B End"},
             {P_MIDPOINT_A,"Color A Transition"},{P_MIDPOINT_B,"Color B Transition"},{P_BALANCE,"Fringe Balance"},{P_SATURATION,"Fringe Saturation"}})
        check(std::strcmp(v[item.first].PF_DEF_NAME,item.second)==0,"Registered name does not match parameter index");
    check(v[P_PALETTE].u.pd.value==1 && v[P_PALETTE].u.pd.num_choices==9,"Original is not the default palette");
    check(v[P_FRINGE_SOFTNESS].u.fs_d.value==100,"Fringe Softness changes prior defaults");
    std::vector<PF_ParamDef*> params;for(auto& value:v)params.push_back(&value);
    auto edit=[&](int index) {PF_UserChangedParamExtra extra{};extra.param_index=index;
        check(entry(PF_Cmd_USER_CHANGED_PARAM,&in,&out,params.data(),nullptr,&extra)==0,"Supervised edit failed");};
    check(entry(PF_Cmd_UPDATE_PARAMS_UI,&in,&out,params.data(),nullptr,nullptr)==0,"Initial UI update failed");
    check(available(P_PAIR) && !available(P_COLOR_A),"Original Separation availability is incorrect");
    for (int preset = 2; preset <= 5; ++preset) {
        v[P_PALETTE].u.pd.value = preset;
        edit(P_PALETTE);
        check(!v[P_GRADIENT].u.bd.value && !available(P_COLOR_A_END), "Solid preset enables Gradient by default");
        const auto a = v[P_COLOR_A].u.cd.value;
        const auto b = v[P_COLOR_B].u.cd.value;
        // A project saved before the fix can have matching start and end colours.
        v[P_COLOR_A_END].u.cd.value = a;
        v[P_COLOR_B_END].u.cd.value = b;
        v[P_GRADIENT].u.bd.value = TRUE;
        edit(P_GRADIENT);
        check(v[P_PALETTE].u.pd.value == 8 && available(P_COLOR_A_END) && available(P_COLOR_B_END),
              "Gradient did not switch a solid preset to editable Custom colours");
        check(v[P_COLOR_A].u.cd.value.blue == a.blue && v[P_COLOR_B].u.cd.value.red == b.red &&
              v[P_COLOR_A_END].u.cd.value.blue != a.blue && v[P_COLOR_B_END].u.cd.value.red != b.red,
              "Gradient did not preserve the starts and populate distinct ends");
    }
    v[P_PALETTE].u.pd.value=6;edit(P_PALETTE);
    check(v[P_COLOR_A].u.cd.value.red==16 && v[P_COLOR_A].u.cd.value.green==16 && v[P_COLOR_A].u.cd.value.blue==61,"Vintage start differs from approved palette");
    check(v[P_GRADIENT].u.bd.value && v[P_MIDPOINT_A].u.fs_d.value==75 && v[P_MIDPOINT_B].u.fs_d.value==50,"Vintage gradient transition was not populated");
    check(available(P_COLOR_A_END),"Gradient end controls are unavailable");
    const auto end=v[P_COLOR_A_END].u.cd.value;
    v[P_COLOR_A].u.cd.value.red=35;edit(P_COLOR_A);
    check(v[P_PALETTE].u.pd.value==8 && v[P_GRADIENT].u.bd.value,"Colour edit did not preserve gradient in Custom");
    check(v[P_COLOR_A_END].u.cd.value.red==end.red,"Colour edit changed an unrelated endpoint");
    v[P_MIDPOINT_A].u.fs_d.value=80;edit(P_MIDPOINT_A);
    check(v[P_PALETTE].u.pd.value==8 && v[P_MIDPOINT_A].u.fs_d.value==80,"Custom transition edit failed");
    v[P_PALETTE].u.pd.value=7;edit(P_PALETTE);
    check(v[P_COLOR_A].u.cd.value.red==37 && v[P_COLOR_A_END].u.cd.value.blue==239,"Coated Glass endpoint mismatch");
    v[P_PALETTE].u.pd.value=9;edit(P_PALETTE);
    check(v[P_GRADIENT].u.bd.value && v[P_USE_B_MIDDLE].u.bd.value && available(P_COLOR_B_MIDDLE),
          "Lens Coating did not expose the warm middle stop");
    check(v[P_COLOR_A].u.cd.value.blue==103 && v[P_COLOR_B_MIDDLE].u.cd.value.green==148 &&
          std::fabs(v[P_MIDPOINT_B].u.fs_d.value-55)<1e-9,"Lens Coating colours or stop position differ");
    v[P_USE_B_MIDDLE].u.bd.value = FALSE;
    edit(P_USE_B_MIDDLE);
    check(v[P_PALETTE].u.pd.value == 8 && !available(P_COLOR_B_MIDDLE) && v[P_GRADIENT].u.bd.value,
          "Turning off the middle colour did not keep the two-stop gradient");
    v[P_USE_B_MIDDLE].u.bd.value = TRUE;
    edit(P_USE_B_MIDDLE);
    check(available(P_COLOR_B_MIDDLE) && v[P_COLOR_B_MIDDLE].u.cd.value.green == 148,
          "Turning the middle colour back on did not restore its control and value");
    v[P_COLOR_A].u.cd.value.green=60;edit(P_COLOR_A);
    check(v[P_PALETTE].u.pd.value==8 && v[P_USE_B_MIDDLE].u.bd.value &&
          v[P_COLOR_B_MIDDLE].u.cd.value.green==148,"Custom edit lost the middle stop");
    v[P_MODE].u.pd.value=2;edit(P_MODE);
    check(available(P_BALANCE) && !available(P_PAIR),"Lens mode colour controls are incorrect");
    v[P_LATERAL].u.fs_d.value=13.5;v[P_THRESHOLD].u.fs_d.value=0.4;
    v[P_FRINGE_SOFTNESS].u.fs_d.value=50;
    v[P_SATURATION].u.fs_d.value=150;edit(P_COLORS_RESET);
    check(v[P_PALETTE].u.pd.value==1 && !v[P_GRADIENT].u.bd.value && !v[P_USE_B_MIDDLE].u.bd.value &&
          v[P_SATURATION].u.fs_d.value==100,"Colour Reset failed");
    check(v[P_LATERAL].u.fs_d.value==13.5 && v[P_THRESHOLD].u.fs_d.value==0.4 && v[P_MODE].u.pd.value==2,"Colour Reset changed spatial controls");
    check(v[P_FRINGE_SOFTNESS].u.fs_d.value==50,"Colour Reset changed Fringe Softness");
    edit(P_ABR_RESET);
    check(v[P_FRINGE_SOFTNESS].u.fs_d.value==100,"Lens Aberration Reset did not restore Fringe Softness");
    std::printf("[ ok ] %s registration, presets, Custom, transitions, visibility and Reset\n",app=='PrMr'?"Premiere callbacks":"After Effects callbacks");
}
int main(int argc,char** argv)
{
    try {
        check(argc==2,"Pass the built .aex path");
        HMODULE module=LoadLibraryA(argv[1]);check(module!=nullptr,"Could not load the Adobe plug-in");
        auto entry=reinterpret_cast<Entry>(GetProcAddress(module,"EffectMain"));check(entry!=nullptr,"EffectMain export is missing");
        paramSuite.PF_UpdateParamUI=updateUI;
        exercise(entry,'FXTC');exercise(entry,'PrMr');
        FreeLibrary(module);return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"[FAIL] %s\n",error.what());return 1;}
}
