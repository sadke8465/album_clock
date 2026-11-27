// Kan metadata and catalogue titles frequently mix Hebrew and Latin scripts.
// Match both song and artist; never accept a title-only or unrelated cover.
export function radioNormalize(value: unknown): string {
  return String(value ?? '').normalize('NFKD').toLowerCase()
    .replace(/\p{M}/gu, '').replace(/&(?:amp;)?/g, 'and')
    .replace(/[^\p{L}\p{N}]/gu, '');
}

const hebrew = /[\u0590-\u05ff]/u;
const latin = /[a-z]/i;
const releaseSuffix = /\s*[\[(](?:\d{4}\s*)?(?:remaster(?:ed)?|original mix|mono|stereo|radio edit|גרסה מחודשת)(?:\s+\d{4})?[\])]/gi;
const dashVersion = /\s+[-–—]\s*(?:\d{4}\s+)?(?:remaster(?:ed)?(?:\s+version)?|original mix|mono|stereo|radio edit|גרסה מחודשת)(?:\s+\d{4})?\s*$/i;

// Kan/Spotify use dash-separated edition tags while Apple often omits them.
// Remove only recognized technical edition labels, never a song subtitle,
// part number, alternate take, or live performance descriptor.
export function radioSearchTitle(value: unknown): string {
  let title=String(value??'').replace(releaseSuffix,'').trim(), prior:string;
  do{prior=title;title=title.replace(dashVersion,'').trim();}while(title!==prior);
  return title;
}

function variants(value: unknown): string[] {
  const raw = radioSearchTitle(value);
  const result = [raw];
  // A separate Hebrew or Latin parenthetical is usually a localized alias,
  // not a version descriptor (e.g. Tzchok Hagoral (צחוק הגורל)).
  const remainder = raw.replace(/[([]([^\])]+)[\])]/g, (_all, inner: string) => {
    if ((hebrew.test(inner) && latin.test(raw.replace(_all, ''))) ||
        (latin.test(inner) && hebrew.test(raw.replace(_all, '')))) {
      result.push(inner); return ' ';
    }
    return _all;
  }).trim();
  result.push(remainder);
  const separated=raw.split(/\s+(?:[-–—/|])\s+/);
  if(separated.length===2 && hebrew.test(separated[0])!==hebrew.test(separated[1]))result.push(...separated);
  return [...new Set(result.map(radioNormalize).filter(Boolean))];
}

function distance(a: string, b: string): number {
  let row = Array.from({length: b.length + 1}, (_, i) => i);
  for (let i = 0; i < a.length; i++) {
    const next = [i + 1];
    for(let j=0;j<b.length;j++) next.push(Math.min(next[j]+1,row[j+1]+1,row[j]+Number(a[i]!==b[j])));
    row = next;
  }
  return row[b.length];
}

const consonants: Record<string,string> = {
  'א':'','ב':'b','ג':'g','ד':'d','ה':'h','ו':'v','ז':'z','ח':'x','ט':'t','י':'',
  'כ':'x','ך':'x','ל':'l','מ':'m','ם':'m','נ':'n','ן':'n','ס':'s','ע':'','פ':'p','ף':'p',
  'צ':'c','ץ':'c','ק':'k','ר':'r','ש':'s','ת':'t'
};
function skeleton(value: string): string[] {
  if(hebrew.test(value)) {
    const base = [...value].map(c=>consonants[c]??c).join('');
    const variants=[base,base.replace(/v/g,''),base.replace(/h/g,''),base.replace(/[hv]/g,''),base.replace(/p/g,'f'),base.replace(/b/g,'v')];
    return [...new Set([...variants,...variants.map(v=>v.replace(/x/g,'h'))])];
  }
  const fold=(v:string)=>v.replace(/tz|ts/g,'c').replace(/sh/g,'s')
    .replace(/th/g,'t').replace(/ph/g,'f').replace(/q/g,'k').replace(/w/g,'v').replace(/[aeiouy]/g,'');
  // Also retain literal k+h: the digraph may straddle a removed space.
  const bases=[fold(value.replace(/ch|kh/g,'x')),fold(value.replace(/ch/g,'x'))];
  const variants=bases.flatMap(base=>[base,base.replace(/h/g,''),base.replace(/v/g,''),base.replace(/[hv]/g,'')]);
  return [...new Set([...variants,...variants.map(v=>v.replace(/x/g,'h'))])];
}

function nameScore(expected: unknown, actual: unknown): number {
  let score=0;
  for(const a of variants(expected)) for(const b of variants(actual)) {
    if(a===b){score=Math.max(score,100);continue;}
    if(hebrew.test(a)!==hebrew.test(b)) {
      if(skeleton(a).some(x=>x.length>=4&&skeleton(b).includes(x))) score=Math.max(score,90);
      continue;
    }
    const size=Math.max(a.length,b.length), short=Math.min(a.length,b.length);
    if(short<5 || Math.abs(a.length-b.length)>Math.max(1,Math.floor(size*.18)))continue;
    const edits=distance(a,b);
    if(edits<=Math.max(1,Math.floor(size*.18)))score=Math.max(score,Math.round(100*(1-edits/size)));
  }
  return score;
}

function artistScore(expected: string, actual: unknown): number {
  const strip = (v: unknown) => String(v??'').replace(/\s+(?:and|&)\s+(?:his|her|their|the)\s+(?:orchestra|band|ensemble|quartet|trio)$/i,'');
  const a = strip(expected), b = strip(actual);
  let score = nameScore(expected, actual);
  const credits=String(actual??'').split(/\s+(?:&|and|with|feat\.?|ft\.?|עם)\s+|[,;]/i);
  if(credits.length>1&&credits.some(credit=>nameScore(expected,credit)===100))score=Math.max(score,92);
  if (radioNormalize(a).length>=5 && radioNormalize(a)===radioNormalize(b)) return Math.max(92,score);
  return score;
}

export function radioMatchScore(title: string, artists: string[], track: unknown, artist: unknown): number {
  const ts=nameScore(title,track), as=Math.max(0,...artists.map(a=>artistScore(a,artist)));
  // Broaden spelling and bilingual matches, but require strong independent
  // evidence on both fields and at least one near-exact field.
  return ts>=80 && as>=80 && Math.max(ts,as)>=90 ? ts+as : 0;
}

export function radioTrackKey(title: string, artists: string[]): string {
  return [radioNormalize(title),...artists.map(radioNormalize).sort()].join('|');
}

export interface RadioTrack {title:string;artists:string[];key:string;start:number;end:number;}
export function parseRadioTrack(value:unknown,now:number):RadioTrack|null{
  if(!value||typeof value!=='object')return null;
  const live=value as Record<string,unknown>;
  const title=typeof live.title==='string'?live.title.trim():'';
  const artists=Array.isArray(live.artists)?live.artists.filter((a):a is string=>typeof a==='string').map(a=>a.trim()).filter(Boolean):[];
  const start=Date.parse(String(live.startTime??'')),end=Date.parse(String(live.endTime??''));
  if(!title||!artists.length||!Number.isFinite(start)||!Number.isFinite(end)||end<=start||now>end+15000||now<start-3000)return null;
  return {title,artists,start,end,key:radioTrackKey(title,artists)};
}
export function canPublishRadio(track:RadioTrack,current:RadioTrack|null,now:number):boolean{
  return !!current&&current.key===track.key&&now<=current.end+15000;
}

export function radioCompilation(album: string): boolean {
  return /best of|all the best|\bgreatest\b|hits and history|collection|anthology|compilation|\bthe essential\b|\bthe very best\b|\bcomplete recordings\b|המיטב|אוסף|להיטים|כל השירים|various artists|אמנים שונים/i.test(album);
}

export function rankRadioReleases<T extends {album:string; score:number; compilation:boolean; single:boolean; date:string}>(candidates:T[]):T[] {
  const live=(s:string)=>/\b(?:live at|live in|live from|live album|in concert)\b|\(live\)|הופעה חיה/i.test(s);
  const soundtrack=(s:string)=>/\bsoundtrack\b|פסקול/i.test(s);
  const confidence=(s:number)=>s>=190?2:s>=180?1:0;
  return candidates.sort((a,b)=>confidence(b.score)-confidence(a.score)||
    Number(a.compilation)-Number(b.compilation)||Number(live(a.album))-Number(live(b.album))||
    Number(soundtrack(a.album))-Number(soundtrack(b.album))||Number(a.single)-Number(b.single)||b.score-a.score||(a.date||'9999').localeCompare(b.date||'9999'));
}
