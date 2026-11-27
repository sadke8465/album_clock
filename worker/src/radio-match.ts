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
    const folded=[...variants,...variants.map(v=>v.replace(/x/g,'h'))];
    return [...new Set([...folded,...folded.map(v=>v.replace(/h$/,''))])];
  }
  const fold=(v:string)=>v.replace(/tz|ts/g,'c').replace(/sh/g,'s')
    .replace(/th/g,'t').replace(/ph/g,'f').replace(/q/g,'k').replace(/w/g,'v').replace(/[aeiouy]/g,'');
  // Also retain literal k+h: the digraph may straddle a removed space.
  const bases=[fold(value.replace(/ch|kh/g,'x')),fold(value.replace(/ch/g,'x'))];
  const variants=bases.flatMap(base=>[base,base.replace(/h/g,''),base.replace(/v/g,''),base.replace(/[hv]/g,'')]);
  const folded=[...variants,...variants.map(v=>v.replace(/x/g,'h'))];
  return [...new Set([...folded,...folded.map(v=>v.replace(/h$/,''))])];
}

// Search-only phonetic guesses for Israeli titles reported in Latin script.
// They do not establish a match; the original metadata is always scored.
export function hebrewSearchVariants(value: string): string[] {
  if(hebrew.test(value)||!/[a-z]/i.test(value))return [];
  const words=value.toLowerCase().match(/[a-z]+/g)??[];
  if(!words.length||words.length>10)return [];
  const letters:Record<string,string>={b:'ב',g:'ג',d:'ד',h:'ה',v:'ו',w:'ו',z:'ז',t:'ת',y:'י',k:'ק',q:'ק',l:'ל',m:'מ',n:'נ',s:'ס',p:'פ',f:'פ',r:'ר',c:'ק',j:'י',x:'ח'};
  const digraphs:Record<string,string>={sh:'ש',ch:'ח',kh:'כ',tz:'צ',ts:'צ',ph:'פ',th:'ת',ai:'י',ay:'י',ei:'י',ey:'י'};
  const convert=(word:string,guttural:boolean)=>{
    let out='';
    for(let i=0;i<word.length;i++){
      const pair=word.slice(i,i+2),c=word[i];
      if(digraphs[pair]){out+=digraphs[pair];i++;continue;}
      if(c==='a'){if(i===0)out+='א';else if(i===word.length-1)out+='ה';continue;}
      if(c==='e'){if(i===0)out+='א';continue;}
      if(c==='i'){out+='י';continue;}
      if(c==='o'||c==='u'){out+='ו';continue;}
      if(c==='h'&&guttural&&i===0&&word[1]!=='a'){out+='ח';continue;}
      out+=letters[c]??'';
    }
    return out.replace(/מ$/,'ם').replace(/נ$/,'ן').replace(/צ$/,'ץ').replace(/פ$/,'ף').replace(/כ$/,'ך');
  };
  return [...new Set([true,false].map(g=>words.map(word=>convert(word,g)).join(' ')))].filter(s=>s.length>=4);
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
  const ts=nameScore(title,track), scores=artists.map(a=>artistScore(a,artist));
  // A duet credited to both Kan artists is stronger evidence than one name.
  const as=scores.length>1&&scores.every(s=>s>=92)?100:Math.max(0,...scores);
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
  return /best of|all the best|\bgreatest\b|\bhits\b|collection|anthology|compilation|\bthe essential\b|\bthe very best\b|\bcomplete recordings\b|\bnumber\s*1['’]?s\b|\bmotown\s+25\b|\bat the close of a century\b|\boriginal musiquarium\b|המיטב|אוסף|להיטים|כל השירים|various artists|אמנים שונים/i.test(album);
}

export interface RadioReleaseEvidence {album:string;score:number;compilation:boolean;single:boolean;date:string;albumDate?:string;source?:string;order?:number;}
const live=(s:string)=>/\b(?:live at|live in|live from|live album|in concert)\b|\(live\)|הופעה חיה/i.test(s);
const soundtrack=(s:string)=>/\bsoundtrack\b|פסקול/i.test(s);
const provider=(s:string='')=>s.startsWith('Apple Music')?'apple':s==='Deezer'?'deezer':s==='MusicBrainz'?'musicbrainz':s;
const year=(s:string='')=>/^\d{4}/.test(s)?Number(s.slice(0,4)):9999;
export function radioAlbumKey(s:string):string{
  return radioNormalize(s.replace(/\s*[\[(][^\])]*(?:remaster|deluxe|expanded|anniversary|special edition|מהדורה מיוחדת)[^\])]*[\])]/gi,'').replace(/\s+[-–—]\s+(?:deluxe|expanded|special edition|מהדורה מיוחדת).*$/i,''));
}

export function rankRadioReleases<T extends RadioReleaseEvidence>(candidates:T[]):T[] {
  const confidence=(s:number)=>s>=190?2:s>=180?1:0;
  const firstYear=Math.min(9999,...candidates.filter(c=>c.score>=180).map(c=>year(c.date)));
  const groups=new Map<string,{providers:Set<string>;year:number;compilation:boolean}>();
  for(const c of candidates){
    const key=radioAlbumKey(c.album),g=groups.get(key)??{providers:new Set(),year:9999,compilation:false};
    if(c.source)g.providers.add(provider(c.source));
    g.year=Math.min(g.year,year(c.albumDate));g.compilation ||= c.compilation;groups.set(key,g);
  }
  const evidence=(c:T)=>groups.get(radioAlbumKey(c.album))!;
  const datePenalty=(c:T)=>{const y=evidence(c).year;return y===9999?1:firstYear!==9999&&y>firstYear+2?2:0;};
  const preference=(c:T)=>provider(c.source)==='apple'?0:provider(c.source)==='deezer'?1:2;
  return candidates.sort((a,b)=>confidence(b.score)-confidence(a.score)||
    Number(evidence(a).compilation)-Number(evidence(b).compilation)||Number(live(a.album))-Number(live(b.album))||
    Number(soundtrack(a.album))-Number(soundtrack(b.album))||datePenalty(a)-datePenalty(b)||
    Number(a.single)-Number(b.single)||evidence(b).providers.size-evidence(a).providers.size||
    b.score-a.score||evidence(a).year-evidence(b).year||(a.date||'9999').localeCompare(b.date||'9999')||
    preference(a)-preference(b)||(a.order??999)-(b.order??999));
}

export function radioReleaseQuality(c:RadioReleaseEvidence,candidates:RadioReleaseEvidence[]):'original'|'fallback'{
  const group=candidates.filter(r=>radioAlbumKey(r.album)===radioAlbumKey(c.album));
  if(group.some(r=>r.compilation)||live(c.album)||soundtrack(c.album)||c.score<190)return 'fallback';
  const firstYear=Math.min(9999,...candidates.filter(r=>r.score>=180).map(r=>year(r.date)));
  const releaseYear=Math.min(9999,...group.map(r=>year(r.albumDate)));
  if(firstYear!==9999&&releaseYear!==9999&&releaseYear>firstYear+2)return 'fallback';
  const corroborated=new Set(group.map(r=>provider(r.source)).filter(Boolean)).size>=2;
  return releaseYear!==9999||corroborated?'original':'fallback';
}
