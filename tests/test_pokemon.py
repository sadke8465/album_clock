import pathlib
import subprocess
import tempfile
import shutil
import unittest


class PokemonTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'host compiler unavailable')
    def test_packet_decode_and_exact_grid_coverage(self):
        header = pathlib.Path(__file__).resolve().parents[1] / 'firmware/include'
        source = r'''
#include "PokemonPacket.h"
#include "PokemonCatalog.h"
#include <cassert>
int main(){
  uint8_t p[]={80,75,65,49,2,1,2,0,1,0,0,1, 40,0,4,0,0, 0,0,1,0, 120,0,3,0,1, 2,0,248};
  PokemonPacket v;assert(v.parse(p,sizeof(p)));assert(v.frames==2&&v.width==2&&v.transparent==1);
  uint16_t pixels[2];assert(v.decode(0,pixels)&&pixels[0]==0&&pixels[1]==1);assert(v.decode(1,pixels)&&pixels[0]==0xf800&&pixels[1]==0xf800);
  assert(v.delay(0)==40&&v.delay(1)==120);assert(!v.parse(p,sizeof(p)-1));p[26]=1;assert(!v.parse(p,sizeof(p)));
  for(int layout=0;layout<=8;layout++){
    const auto grid=pkGrid(layout);int coverage[4096]={};
    for(int i=0;i<grid.cols*grid.rows;i++){auto r=pkCell(layout,i);assert(r.w>0&&r.h>0);
      for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)++coverage[y*64+x];}
    for(int i=0;i<4096;i++)assert(coverage[i]==1);
  }
  assert(POKEMON_COUNT==809&&POKEMON_GAME_COUNT==16);
  assert(POKEMON_SPECIES[25].generation==1&&POKEMON_SPECIES[25].type1==13);
  assert(POKEMON_SPECIES[2].family==POKEMON_SPECIES[1].family);
  assert(POKEMON_GAMES[12].maxDex==649&&POKEMON_GAMES[7].animated);
}
'''
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / 'test'
            subprocess.run(['g++','-std=c++11','-I',str(header),'-x','c++','-','-o',str(binary)], input=source,text=True,check=True)
            subprocess.run([str(binary)],check=True)

    def test_mode_isolation_and_remote_controls(self):
        root = pathlib.Path(__file__).resolve().parents[1] / 'firmware'
        main = (root/'src/main.cpp').read_text()
        portal = (root/'include/PortalPage.h').read_text()
        self.assertIn('if(displayMode==2){wasPokemon=true;pokemon.networkStep',main)
        self.assertIn('if(displayMode==2){pokemon.service(draw565)',main)
        self.assertIn('if(m==2)enterPokemon()',main)
        self.assertIn('pokemon.exit()',main)
        self.assertIn('displayMode==2?pokemon.screen:',main)
        self.assertIn('data-family="pokemon"',portal)
        self.assertIn('data-family="album"',portal)
        self.assertIn('pkSelectCell(Math.max',portal)
        self.assertIn('/pokemon/settings',portal)
