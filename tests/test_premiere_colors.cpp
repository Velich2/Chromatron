#include "ae/PremiereColor.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace chromatron;
using namespace chromatron::ae;
static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static bool equal(FringeColor a,FringeColor b) {
    return std::fabs(a.r-b.r)<1e-12 && std::fabs(a.g-b.g)<1e-12 && std::fabs(a.b-b.b)<1e-12;
}
int main()
{
    try {
        for (const auto color : {0x00BCD4u,0x355FD3u,0x10103Du,0xFFFFFFu,0x000000u,0xFF0000u,0x0000FFu}) {
            const auto r=(color>>16)&255u,g=(color>>8)&255u,b=color&255u;
            const FringeColor expected{r/255.0,g/255.0,b/255.0};
            Inputs wide,legacy;
            PrParam p{};p.mType=kPrParamType_Int64;
            p.mInt64=static_cast<csSDK_int64>(0xFF00000000000000ull | (std::uint64_t(r)<<40) | (std::uint64_t(g)<<24) | (std::uint64_t(b)<<8));
            check(ReadPremiereColorParam(P_COLOR_A,p,wide),"64-bit colour was rejected");
            check(equal(wide.settings.colorA,expected),"64-bit ARGB slots changed the chosen RGB colour");
            check(ReadPremiereColorParam(P_COLOR_B_END,p,wide) && equal(wide.settings.colorBEnd,expected),"Gradient endpoint colour changed");
            p.mType=kPrParamType_Int32;p.mInt32=static_cast<csSDK_int32>(0xFF000000u|color);
            check(ReadPremiereColorParam(P_COLOR_A,p,legacy) && equal(legacy.settings.colorA,expected),"Legacy BGRA colour changed");
            check(equal(wide.settings.colorA,legacy.settings.colorA),"32-bit and 64-bit encodings disagree");
        }
        Inputs inputs;const auto before=inputs.settings.colorA;
        PrParam invalid{};invalid.mType=kPrParamType_PrMemoryPtr;invalid.mMemoryPtr=reinterpret_cast<PrMemoryPtr>(0x1234);
        check(!ReadPremiereColorParam(P_COLOR_A,invalid,inputs) && equal(before,inputs.settings.colorA),"Unsupported pointer was interpreted as RGB");
        check(!ReadPremiereColorParam(P_COLOR_A,PrParam{},inputs),"Failed/empty parameter lookup was accepted");
        std::puts("[ ok ] Premiere turquoise, gradients, primary colours and 32/64-bit parameter transport");
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"[FAIL] %s\n",error.what());return 1;}
}
