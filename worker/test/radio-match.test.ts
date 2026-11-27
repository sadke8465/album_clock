import test from 'node:test';
import assert from 'node:assert/strict';
import {radioMatchScore,radioTrackKey,radioCompilation,rankRadioReleases,parseRadioTrack,canPublishRadio,radioSearchTitle,hebrewSearchVariants} from '../src/radio-match.ts';

test('Kan Hebrew title matches bilingual Apple track from live feed',()=>{
  assert.equal(radioMatchScore('צחוק הגורל',['Peter Roth'],'Tzchok Hagoral (צחוק הגורל)','Peter Roth'),200);
  assert.equal(radioMatchScore('צחוק הגורל',['פיטר רוט'],'Tzchok Hagoral (צחוק הגורל)','Peter Roth (פיטר רוט)'),200);
  assert.ok(radioMatchScore('צחוק הגורל',['פיטר רוט'],'Tzchok Hagoral','Peter Roth')>=180);
  assert.ok(radioMatchScore('מחכים למשיח',['שלום חנוך'],'מחכים למשיח','Shalom Hanoch')>=190);
  assert.equal(radioMatchScore('צחוק הגורל',['Peter Roth'],'צחוק הגורל','Doriel'),0);
});

test('Spelling, remasters, ampersands and credits are bounded',()=>{
  assert.equal(radioMatchScore('Leaving on a Jet Plane',['John Denver'],'Leaving on a Jet Plane (Remastered)','John Denver'),200);
  assert.equal(radioMatchScore('I Got You Babe',['Sonny & Cher'],'I Got You Babe','Sonny and Cher'),200);
  assert.ok(radioMatchScore('The Sound of Silence',['Simon & Garfunkel'],'The Sounds of Silence','Simon & Garfunkel')>=180);
  assert.equal(radioMatchScore('One',['U2'],'Two','U2'),0);
  assert.equal(radioMatchScore('One',['U2'],'One','Metallica'),0);
  assert.equal(radioTrackKey('שלום',['ב','א']),radioTrackKey('שָׁלוֹם',['א','ב']));
});

test('Original release wins over compilations and reissues at comparable confidence',()=>{
  const release=(album:string,score:number,date:string,single=false)=>({album,score,date,single,compilation:radioCompilation(album)});
  const a=[release('Greatest Hits',200,'1979'),release('הלהיטים הגדולים',200,'1975'),release('Original Album (Remastered)',200,'2010'),release('Original Album',200,'1971'),release('New Single',200,'1970',true),release('Uncertain Match',182,'1960')];
  assert.equal(rankRadioReleases(a)[0].album,'Original Album');
  assert.equal(rankRadioReleases([release('Greatest Hits',200,'1980'),release('Weak album',168,'1970')])[0].album,'Greatest Hits');
});

test('A slow lookup cannot publish over a newer radio song or expired metadata',()=>{
  const now=Date.parse('2026-11-27T13:38:00Z');
  const x={title:'צחוק הגורל',artists:['Peter Roth'],startTime:'2026-11-27T13:36:46Z',endTime:'2026-11-27T13:40:21Z'};
  const a=parseRadioTrack(x,now)!;
  const b=parseRadioTrack({...x,title:'Another Song'},now)!;
  assert.ok(a&&b);
  assert.equal(canPublishRadio(a,a,now),true);
  assert.equal(canPublishRadio(a,b,now),false);
  assert.equal(canPublishRadio(a,null,now),false);
  assert.equal(canPublishRadio(a,a,a.end+16000),false);
  assert.equal(parseRadioTrack(x,a.end+16000),null);
  assert.equal(parseRadioTrack(x,a.start-4000),null);
});

test('Kan dash-separated remaster tags match original catalog titles and credits',()=>{
  const kan='Live And Let Die - 2018 Remaster';
  assert.equal(radioSearchTitle(kan),'Live And Let Die');
  assert.equal(radioSearchTitle('Song - Remastered 2011'),'Song');
  assert.equal(radioSearchTitle('Song - Part 2'),'Song - Part 2');
  assert.equal(radioSearchTitle('Song - Live'),'Song - Live');
  assert.equal(radioMatchScore(kan,['Wings'],'Live and Let Die','Wings'),200);
  assert.equal(radioMatchScore(kan,['Wings'],'Live And Let Die','Paul McCartney & Wings'),192);
  assert.equal(radioMatchScore(kan,['Wings'],'Live And Let Die (2022 Remaster)',"Guns N' Roses"),0);
  const release=(album:string,single=false)=>({album,single,score:200,date:'1973',compilation:radioCompilation(album)});
  assert.equal(rankRadioReleases([release('Wings Greatest'),release('All the Best!'),release('Wingspan: Hits and History'),release('Shrek The Third (Motion Picture Soundtrack)'),release('Live and Let Die - Single',true)])[0].album,'Live and Let Die - Single');
});

test('Latin-only Kan titles can find their Hebrew catalogue originals',()=>{
  const title='Hozrim Habaita',artists=['Shlomo Artzi','Arik Einstein'];
  assert.ok(hebrewSearchVariants(title).includes('חוזרים הביתה'));
  assert.ok(radioMatchScore(title,artists,'חוזרים הביתה','Shlomo Artzi')>=190);
  assert.ok(radioMatchScore(title,artists,'חוזרים הבית','Arik Einstein & Shlomo Artzi')>=180);
  assert.equal(radioMatchScore(title,artists,'Hozrim Habaita','ariela savir'),0);
});
