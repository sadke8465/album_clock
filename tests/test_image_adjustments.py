import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ImageAdjustmentTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "host C++ compiler unavailable")
    def test_rendering(self):
        header = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "include"
        source = r"""
#include "ImageAdjustments.h"
#include <cassert>
#include <cstring>
int main() {
    uint16_t original[4096], out[4096], scratch[4096];
    for(int i=0;i<4096;i++) original[i]=uint16_t(i*17);
    ImageAdjustments a;
    imageRender(original,out,scratch,a);
    assert(!memcmp(original,out,sizeof(out)));
    a.saturation=0;
    imageRender(original,out,scratch,a);
    for(auto v:out) {
        assert(abs(imageR(v)-imageG(v))<=8);
        assert(abs(imageR(v)-imageB(v))<=8);
    }
    a=ImageAdjustments{};
    a.blur=2;
    memset(original,0,sizeof(original));
    original[32*64+32]=0xffff;
    imageRender(original,out,scratch,a);
    assert(out[32*64+32]<0xffff && out[32*64+33]>0);
    assert(out[0]==0 && original[32*64+32]==0xffff);
    a=ImageAdjustments{};
    a.warmth=50;
    for(auto &v:original) v=0x8410;
    imageRender(original,out,scratch,a);
    assert(imageR(out[0])>imageB(out[0]));
    a=cleanImagePreset();
    memset(original,0,sizeof(original));
    original[0]=image565(8,8,8);
    original[1]=image565(247,247,247);
    original[2]=0xf800;
    original[3]=0xffff;
    imageRender(original,out,scratch,a);
    assert(out[0]==0);
    assert(out[1]==0xffff);
    assert(out[2]==0xf800);
    assert(out[3]==0xffff);
    a=ImageAdjustments{};
    a.shadows=100; a.highlights=100;
    for(auto &v:original) v=image565(16,16,16);
    original[32*64+32]=image565(40,40,40);
    original[32*64+33]=0xffff;
    imageRender(original,out,scratch,a);
    assert(imageR(out[32*64+32])<imageR(original[32*64+32]));
    assert(out[32*64+33]==0xffff);
    for(auto &v:original) v=image565(248,248,248);
    original[32*64+32]=image565(224,224,224);
    imageRender(original,out,scratch,a);
    assert(imageR(out[32*64+32])>=imageR(original[32*64+32]));
}
"""
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / "test"
            subprocess.run(["g++", "-std=c++11", "-I", str(header), "-x", "c++", "-", "-o", str(binary)],
                           input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)
