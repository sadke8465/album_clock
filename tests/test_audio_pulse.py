import pathlib
import shutil
import subprocess
import tempfile
import unittest


class AudioPulseTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'host compiler unavailable')
    def test_audio_envelope_and_native_pixel_gain(self):
        header = pathlib.Path(__file__).resolve().parents[1] / 'firmware' / 'include'
        source = r'''
#include "AudioPulse.h"
#include "AmbientEffects.h"
#include <cassert>
int main(){
    int16_t pcm[8]={1000,3000,1000,1000,1000,3000,1000,1000};
    assert(stereoMicRms(pcm,4)==1000); // DC removed; strongest channel
    AudioPulse env;
    assert(env.update(80,0,100,12,35,240)==0); // silence below gate
    const int first=env.update(1800,16,100,12,35,240);
    assert(first>0&&first<255);
    const int second=env.update(1800,32,100,12,35,240);
    assert(second>first&&second<255);
    const int quiet=env.update(0,48,100,12,35,240);
    assert(quiet<second&&quiet>0);
    for(int t=64;t<2000;t+=16)env.update(0,t,100,12,35,240);
    assert(env.level<0.01f);
    env.reset();AudioPulse slower;
    assert(env.update(900,0,100,12,10,240)>slower.update(900,0,100,12,200,240));
    uint16_t base[4096],out[4096],quietFrame[4096];
    for(int i=0;i<4096;i++)base[i]=0;
    base[1]=image565(30,30,30);base[2]=0xffff; // dark gray, white
    base[3]=image565(0,0,190);base[4]=image565(160,150,140); // saturated blue, bright neutral
    AmbientSettings s;s.mode=7;s.target=5;s.threshold=100;s.intensity=100;
    ambientRender(base,out,s,0,-1);assert(!memcmp(base,out,sizeof(base)));
    ambientRender(base,quietFrame,s,0,0);ambientRender(base,out,s,0,255);
    assert(!out[0]&&!quietFrame[0]);assert(out[1]==base[1]&&quietFrame[1]==base[1]);
    assert(out[2]==0xffff&&imageR(quietFrame[2])<180);
    assert(imageB(out[3])>imageB(base[3])&&imageB(quietFrame[3])<imageB(base[3]));
    assert(imageR(out[3])==0&&imageG(out[3])==0);
    assert(imageR(out[4])>imageR(base[4])&&imageR(quietFrame[4])<imageR(base[4]));
    ambientRender(base,out,s,0,128);assert(!memcmp(base,out,sizeof(base)));
    ambientRender(base,out,s,1234,255);uint16_t firstBright=out[3];
    ambientRender(base,out,s,99999,255);assert(out[3]==firstBright); // no time oscillator
    s.intensity=0;ambientRender(base,out,s,0,0);assert(!memcmp(base,out,sizeof(base)));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / 'test'
            subprocess.run(['g++', '-std=c++11', '-I', str(header), '-x', 'c++', '-', '-o', str(binary)], input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_audio_is_sampled_before_compositing(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        main = (root / 'firmware/src/main.cpp').read_text()
        self.assertIn('ambientRender(ambientBase,ambientOutput,ambientSettings,now,level)', main)
        self.assertIn('displayMode!=0||ambientSettings.mode!=7', main)
        self.assertIn('serviceAlbumAudio();\n  if(otaInstalling())', main)
        self.assertIn('serviceAlbumAudio();\n    if(displayMode==2)', main)
        mic = (root / 'firmware/include/AudioReactive.h').read_text()
        capture = mic.split('inline bool readLevel', 1)[1].split('inline void readBands', 1)[0]
        self.assertNotIn('portMAX_DELAY', capture)
