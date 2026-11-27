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
#include "SmartRegions.h"
#include <cassert>
int main(){
    FaceCrop tile=faceScanTile(4);
    assert(tile.x==96&&tile.y==96&&tile.side==160);
    auto mapped=mapFaceRect({0,0,192,192},tile,192);
    assert(mapped.x0==96&&mapped.y0==96&&mapped.x1==255&&mapped.y1==255);
    assert(sameFace({10,10,50,50},{12,12,48,48}));
    assert(!sameFace({10,10,50,50},{80,80,100,100}));
    assert(zoomDepthCrop({0,0,192},150).side==128);
    uint8_t gray[128*128]{};
    auto blank=analyzeSmartRegions(gray,128);assert(blank.textCount==0);
    for(int y=10;y<22;y++)for(int x=6;x<58;x++)if((x%6)<3)gray[y*128+x]=255;
    auto text=analyzeSmartRegions(gray,128);
    assert(text.textCount>0&&text.text[0].y==0);
    assert(text.detailOrder[0]==0);
    uint8_t rgb[3];
    faceRgb888(0xf800,rgb);assert(rgb[0]==255&&rgb[1]==0&&rgb[2]==0);
    faceRgb888(0x001f,rgb);assert(rgb[0]==0&&rgb[1]==0&&rgb[2]==255);
    FaceCrop c=faceCrop({0,0,25,25});
    assert(c.x==0 && c.y==0 && c.side==64);
    c=faceCrop({227,220,255,255});
    assert(c.x+c.side==256 && c.y+c.side==256);
    c=faceCrop({100,100,150,150});
    assert(c.x<100 && c.x+c.side>150 && c.y<100 && c.y+c.side>150);
    c=faceCrop({20,10,230,235});
    assert(c.side==224 && c.x+c.side<=256 && c.y+c.side<=256);
    uint16_t original[256*256], output[64*64];
    for(auto &v:original)v=0x07e0;
    faceResize(original,256,256,output,64,c);
    for(auto v:output)assert(v==0x07e0);
    faceResizeClean(original,256,output,64,{0,0,256});
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
    MotionCrop a{0,0,256}, b=motionCrop(q2);
    auto fractional=interpolateMotion(a,b,.1f,100);
    assert(fractional.x>0 && fractional.x<1 && fractional.side<256);
    auto linear=interpolateMotion(a,b,.25f,0);
    assert(linear.x==16 && linear.side==240);
    auto endMotion=interpolateMotion(a,b,1.1f,100);
    assert(endMotion.x==64 && endMotion.side==192);
    assert(motionSample(tiny,2,32768,32768)==middle);
    for(int i=0;i<=100;i++){
      auto crop=interpolateMotion(a,b,i/100.f,100);
      assert(crop.x>=0&&crop.y>=0&&crop.x+crop.side<=256.001f);
      faceResizeMotion(original,256,output,64,crop);
      for(auto v:output)assert(v==0x07e0);
    }
    uint16_t base[64*64];
    for(auto &v:base)v=0xf800;
    faceResizeMotion(original,256,output,64,a,base);
    for(auto v:output)assert(v==0xf800);
    faceResizeMotion(original,256,output,64,b,base);
    for(auto v:output)assert(v==0x07e0);
    // Fractional moves must actually change pixel values before a whole
    // normalized source pixel has moved.
    uint16_t edge[64*64], before[64*64];
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)edge[y*64+x]=x<32?0:0xffff;
    faceResizeMotion(edge,64,before,64,{64,64,128});
    faceResizeMotion(edge,64,output,64,{64.75f,64,128});
    bool changed=false;
    for(int i=0;i<4096;i++)if(before[i]!=output[i])changed=true;
    assert(changed);
}
"""
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / "test"
            subprocess.run(["g++", "-std=c++11", "-I", str(header), "-x", "c++", "-", "-o", str(binary)],
                           input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)
