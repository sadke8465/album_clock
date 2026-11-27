import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RandomZoomTests(unittest.TestCase):
    def test_random_art_gets_detail_source_and_idle_scan(self):
        root = pathlib.Path(__file__).resolve().parents[1] / 'firmware'
        main = (root / 'src' / 'main.cpp').read_text()
        fallback = main.split('bool beginNextFallback(bool manual) {', 1)[1].split('void processNetworkEvents()', 1)[0]
        self.assertIn('/v1/detail/fallback/', fallback)
        self.assertIn('false,false,true)', fallback)
        self.assertIn('lastWasIdle=!active&&!recognized;', main)
        self.assertIn('runFaceDetection(job,radio||faceRandomArtwork)', main)
        self.assertIn('faceRandomArtwork&&!lastWasIdle?nullptr:', main)

    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler unavailable')
    def test_source_gating_and_fallback_order(self):
        header = pathlib.Path(__file__).resolve().parents[1] / 'firmware' / 'include'
        source = r'''
#include "ZoomSourcePolicy.h"
#include <cassert>
int main() {
    assert(faceArtworkEligible(false,true));
    assert(faceArtworkEligible(true,false));
    assert(!faceArtworkEligible(false,false));
    assert(faceScanAllowed(false,true,false,true));
    assert(!faceScanAllowed(false,true,true,false));
    assert(faceScanAllowed(true,false,true,false));
    assert(!faceScanAllowed(true,false,false,true));
    using Z=SmartZoomChoice;
    assert(chooseSmartZoom(true,2,false,true,3,true,false,-1,-1)==Z::Face);
    assert(chooseSmartZoom(true,0,false,true,3,true,false,-1,-1)==Z::Text);
    assert(chooseSmartZoom(true,0,false,true,0,true,false,-1,-1)==Z::Quadrant);
    assert(chooseSmartZoom(true,0,true,true,3,true,false,-1,-1)==Z::None);
    assert(chooseSmartZoom(true,0,false,true,3,true,true,-1,-1)==Z::None);
    assert(chooseSmartZoom(false,2,false,false,3,false,false,-1,-1)==Z::None);
    assert(chooseSmartZoom(true,2,false,true,3,true,false,0,-1)==Z::Text);
    assert(chooseSmartZoom(true,2,false,true,3,true,false,-1,0)==Z::Quadrant);
}
'''
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / 'test'
            subprocess.run(['g++', '-std=c++11', '-I', str(header), '-x', 'c++', '-', '-o', str(binary)], input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)
