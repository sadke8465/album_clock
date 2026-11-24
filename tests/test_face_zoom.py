import pathlib
import shutil
import subprocess
import tempfile
import unittest


class FaceZoomTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "host C++ compiler unavailable")
    def test_crop_and_sampling(self):
        header = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "include"
        source = r"""
#include "FaceZoom.h"
#include <cassert>
int main(){
    FaceCrop c=faceCrop({0,0,25,25});
    assert(c.x==0 && c.y==0 && c.side==64);
    c=faceCrop({227,220,255,255});
    assert(c.x+c.side==256 && c.y+c.side==256);
    c=faceCrop({100,100,150,150});
    assert(c.x<100 && c.x+c.side>150 && c.y<100 && c.y+c.side>150);
    uint16_t original[256*256], output[64*64];
    for(auto &v:original)v=0x07e0;
    faceResize(original,256,256,output,64,c);
    for(auto v:output)assert(v==0x07e0);
    const uint16_t tiny[4]={0,0xffff,0xffff,0};
    uint16_t middle=faceSample(tiny,2,2,.5f,.5f);
    assert(middle!=0 && middle!=0xffff);
    const FaceCrop q0=quadrantCrop(0),q1=quadrantCrop(1),q2=quadrantCrop(2),q3=quadrantCrop(3);
    assert(q0.x==0&&q0.y==0&&q0.side==192);
    assert(q1.x==64&&q1.y==0&&q2.x==64&&q2.y==64&&q3.x==0&&q3.y==64);
    FaceCrop beginning=interpolateCrop({0,0,256},q2,0), end=interpolateCrop({0,0,256},q2,1);
    assert(beginning.side==256 && end.x==64 && end.y==64 && end.side==192);
    for(int i=0;i<=100;i++){FaceCrop c=interpolateCrop({0,0,256},q2,i/100.f);assert(c.x>=0&&c.y>=0&&c.x+c.side<=256&&c.y+c.side<=256);}
}
"""
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / "test"
            subprocess.run(["g++", "-std=c++11", "-I", str(header), "-x", "c++", "-", "-o", str(binary)],
                           input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)
