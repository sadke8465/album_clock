import pathlib
import shutil
import subprocess
import tempfile
import unittest


class WiiMMetadataTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "host C++ compiler unavailable")
    def test_escaped_metadata(self):
        header = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "include"
        source = r"""
#include "WiimMetadata.h"
#include <cassert>
int main(){
    assert(wiimFieldMatches("Rhymes & Reasons","Rhymes &amp; Reasons"));
    assert(wiimFieldMatches("Sonny & Cher","Sonny &amp; Cher"));
    assert(wiimFieldMatches("Leaving on a Jet Plane","Leaving on a Jet Plane"));
    assert(wiimFieldMatches("'74-'75","&#39;74-&#x27;75"));
    assert(wiimFieldMatches("Beyoncé","Beyonc&#233;"));
    assert(wiimFieldMatches("A B","A&nbsp;B"));
    assert(wiimFieldMatches("A &amp; B","A &amp;amp; B"));
    assert(wiimFieldMatches(" A & B ","A &amp; B"));
    assert(!wiimFieldMatches("Album","Other album"));
    assert(!wiimFieldMatches("Album",""));
    assert(!wiimFieldMatches("",""));
    assert(decodeWiimEntities("A &unknown; B")=="A &unknown; B");
    assert(decodeWiimEntities("&#0; &#xD800; &#999999999;")=="&#0; &#xD800; &#999999999;");
    assert(decodeWiimEntities("&amp;lt;")=="&lt;");
}
"""
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / "test"
            subprocess.run(
                ["g++", "-std=c++11", "-I", str(header), "-x", "c++", "-", "-o", str(binary)],
                input=source, text=True, check=True
            )
            subprocess.run([str(binary)], check=True)
