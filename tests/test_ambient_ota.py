import pathlib
import shutil
import subprocess
import tempfile
import unittest


class AmbientAndOtaTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "host C++ compiler unavailable")
    def test_rendering_and_manifest(self):
        header = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "include"
        source = r"""
#include "AmbientEffects.h"
#include "OtaManifest.h"
#include <cassert>
int main(){
    uint16_t base[4096],out[4096],second[4096],copy[4096];
    for(int i=0;i<4096;i++)base[i]=i%64<16?0:image565(120,170,220);
    memcpy(copy,base,sizeof(base));
    AmbientSettings a;
    a.mode=0;ambientRender(base,out,a,1000);assert(!memcmp(base,out,sizeof(base)));
    a.intensity=70;a.density=48;a.size=3;
    for(int mode=1;mode<=5;mode++){
        a.mode=mode;ambientRender(base,out,a,1323);ambientRender(base,second,a,2354);
        assert(!memcmp(base,copy,sizeof(base)));
        assert(memcmp(out,second,sizeof(out)));
        for(int i=0;i<4096;i++)if(i%64<16)assert(out[i]==0&&second[i]==0);
    }
    a.mode=1;a.intensity=0;ambientRender(base,out,a,1000);assert(!memcmp(base,out,sizeof(base)));
    // Default sparkle must visibly change more than a handful of pixels,
    // without lifting black areas.
    a=AmbientSettings{};ambientRender(base,out,a,2100);
    int changed=0,strong=0;
    for(int i=0;i<4096;i++){
        if(out[i]!=base[i])changed++;
        if(imageR(out[i])-imageR(base[i])>=16)strong++;
        if(i%64<16)assert(out[i]==0);
    }
    assert(changed>=20&&strong>=10);
    // White has no headroom for additive highlights; Light & Shade must
    // still produce visible movement on white artwork.
    for(int i=0;i<4096;i++)base[i]=i%64<16?0:0xffff;
    a.mode=3;a.blend=3;a.intensity=60;
    ambientRender(base,out,a,0);ambientRender(base,second,a,4000);
    assert(imageR(out[32])<230);
    assert(memcmp(out,second,sizeof(out)));
    for(int i=0;i<4096;i++)if(i%64<16)assert(out[i]==0&&second[i]==0);
    assert(otaNewer("3.8.1-wiim","3.8.0-wiim"));
    assert(otaNewer("3.10.0-wiim","3.9.9-wiim"));
    assert(!otaNewer("3.7.9-wiim","3.8.0-wiim"));
    assert(!otaNewer("3.8.0-wiim","3.8.0-wiim"));
    assert(!otaNewer("4.0.0-other","3.8.0-wiim"));
    assert(!otaNewer("999999999999.0.0-wiim","3.8.0-wiim"));
    assert(otaHashValid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(!otaHashValid("not a hash"));
}
"""
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / "test"
            subprocess.run(["g++", "-std=c++11", "-I", str(header), "-x", "c++", "-", "-o", str(binary)],input=source,text=True,check=True)
            subprocess.run([str(binary)],check=True)
