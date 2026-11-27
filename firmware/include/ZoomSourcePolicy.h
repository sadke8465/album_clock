#pragma once

// Live artwork may be scanned only while WiiM is playing, and random
// artwork only when there is no known active playback. Unknown/cached art can still use the
// inexpensive text and quadrant fallbacks without running face inference.
static inline bool faceArtworkEligible(bool live,bool random){return live||random;}
static inline bool faceScanAllowed(bool live,bool random,bool musicAllowed,bool idleAllowed){
  return (live&&musicAllowed)||(random&&idleAllowed);
}

enum class SmartZoomChoice {None,Face,Text,Quadrant};
static inline SmartZoomChoice chooseSmartZoom(bool facesEnabled,int faceCount,bool facePending,
    bool textEnabled,int textCount,bool quadrantEnabled,bool detailWaiting,int textIndex,int quadrantIndex){
  if(detailWaiting)return SmartZoomChoice::None;
  if(quadrantEnabled&&quadrantIndex>=0)return SmartZoomChoice::Quadrant;
  if(textEnabled&&textIndex>=0&&textCount>0)return SmartZoomChoice::Text;
  if(facesEnabled&&faceCount>0)return SmartZoomChoice::Face;
  if(facesEnabled&&facePending)return SmartZoomChoice::None;
  if(textEnabled&&textCount>0)return SmartZoomChoice::Text;
  if(quadrantEnabled)return SmartZoomChoice::Quadrant;
  return SmartZoomChoice::None;
}
