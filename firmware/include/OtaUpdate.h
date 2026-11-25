#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "OtaManifest.h"
#include "AlbumClockConfig.h"

// Only official firmware from this fixed, TLS-validated origin is accepted.
static constexpr char OTA_ORIGIN[]="https://albumclock.sadke8465.workers.dev";
struct OtaStatus {
  uint8_t state=0,progress=0;
  uint32_t size=0;
  char version[32]{},hash[65]{},notes[160]{},message[100]{};
};
static OtaStatus otaStatus;
static portMUX_TYPE otaMux=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t otaQueue=nullptr;
static char otaToken[17]{};
static inline OtaStatus getOtaStatus(){
  portENTER_CRITICAL(&otaMux);OtaStatus s=otaStatus;portEXIT_CRITICAL(&otaMux);return s;
}
static inline void setOtaStatus(const OtaStatus &s){
  portENTER_CRITICAL(&otaMux);otaStatus=s;portEXIT_CRITICAL(&otaMux);
}
static inline void otaMessage(uint8_t state,const char *message,uint8_t progress=0){
  OtaStatus s=getOtaStatus();s.state=state;s.progress=progress;strlcpy(s.message,message,sizeof(s.message));setOtaStatus(s);
}
static inline bool otaInstalling(){auto s=getOtaStatus();return s.state==4||s.state==6;}
static inline bool otaHttpBegin(HTTPClient &http,WiFiClientSecure &secure,const uint8_t *cert,const String &path){
  secure.setCACertBundle(cert);
  http.setConnectTimeout(5000);http.setTimeout(8000);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  return http.begin(secure,String(OTA_ORIGIN)+path);
}
static inline void otaCheck(const uint8_t *cert){
  HTTPClient http;WiFiClientSecure secure;
  if(!otaHttpBegin(http,secure,cert,"/v1/firmware/latest.json")){otaMessage(5,"Unable to connect to update service");return;}
  int code=http.GET(),size=http.getSize();
  if(code!=200||size<1||size>4096){http.end();otaMessage(5,"Update manifest unavailable");return;}
  String json=http.getString();http.end();
  JsonDocument doc;
  if(deserializeJson(doc,json)){otaMessage(5,"Invalid update manifest");return;}
  const char *v=doc["version"]|"",*hash=doc["sha256"]|"",*board=doc["board"]|"";
  uint32_t bytes=doc["size"]|0U,parsed[3];
  if(strcmp(board,"waveshare-s3-n32r16")||!otaParseVersion(v,parsed)||!otaHashValid(hash)||bytes<100000||bytes>0x640000){
    otaMessage(5,"Update is not compatible with this display");return;
  }
  OtaStatus s{};s.size=bytes;strlcpy(s.version,v,sizeof(s.version));strlcpy(s.hash,hash,sizeof(s.hash));strlcpy(s.notes,doc["notes"]|"",sizeof(s.notes));
  s.state=otaNewer(v,FIRMWARE_VERSION)?2:3;strlcpy(s.message,s.state==2?"New firmware available":"Already up to date",sizeof(s.message));setOtaStatus(s);
}
static inline void otaInstall(const uint8_t *cert){
  OtaStatus s=getOtaStatus();
  if(!otaNewer(s.version,FIRMWARE_VERSION)||!otaHashValid(s.hash)){otaMessage(5,"No newer verified update");return;}
  const esp_partition_t *partition=esp_ota_get_next_update_partition(nullptr);
  if(!partition||s.size>partition->size){otaMessage(5,"No compatible inactive firmware slot");return;}
  HTTPClient http;WiFiClientSecure secure;
  if(!otaHttpBegin(http,secure,cert,String("/v1/firmware/")+s.hash+".bin")){otaMessage(5,"Download connection failed");return;}
  int code=http.GET();
  if(code!=200||http.getSize()!=(int)s.size){http.end();otaMessage(5,"Firmware size mismatch");return;}
  uint8_t *chunk=(uint8_t *)heap_caps_malloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!chunk){http.end();otaMessage(5,"Not enough memory for update");return;}
  esp_ota_handle_t handle;
  if(esp_ota_begin(partition,s.size,&handle)!=ESP_OK){free(chunk);http.end();otaMessage(5,"Cannot prepare inactive slot");return;}
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
  size_t received=0;uint32_t last=millis(),started=last;bool ok=true;
  auto stream=http.getStreamPtr();
  while(received<s.size){
    if(millis()-last>15000||millis()-started>180000){ok=false;break;}
    int available=stream->available();
    if(available<=0){if(!http.connected()){ok=false;break;}vTaskDelay(pdMS_TO_TICKS(10));continue;}
    size_t limit=min((size_t)4096,min((size_t)available,(size_t)s.size-received));
    int n=stream->read(chunk,limit);if(n<=0)continue;
    if(esp_ota_write(handle,chunk,n)!=ESP_OK){ok=false;break;}
    mbedtls_sha256_update_ret(&sha,chunk,n);received+=n;last=millis();
    uint8_t progress=received*100/s.size;
    if(progress!=s.progress){s.progress=progress;strlcpy(s.message,"Downloading and verifying firmware",sizeof(s.message));setOtaStatus(s);}
    vTaskDelay(1);
  }
  unsigned char digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);free(chunk);http.end();
  char actual[65];for(int i=0;i<32;i++)snprintf(actual+2*i,3,"%02x",digest[i]);
  if(!ok||received!=s.size||strcmp(actual,s.hash)){esp_ota_abort(handle);otaMessage(5,"Download or checksum failed; current firmware retained");return;}
  if(esp_ota_end(handle)!=ESP_OK){otaMessage(5,"Firmware image validation failed");return;}
  if(esp_ota_set_boot_partition(partition)!=ESP_OK){otaMessage(5,"Could not select new firmware");return;}
  otaMessage(6,"Verified. Restarting display",100);vTaskDelay(pdMS_TO_TICKS(1500));ESP.restart();
}
