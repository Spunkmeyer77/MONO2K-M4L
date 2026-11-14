/* Engine test: Live at 48 kHz, a 120 BPM beat phase, start mid-song, follow the steps, save and reload, render a WAV. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m2k.h"
#include "m2k_engine.h"
int m2k_ext_step(void); int m2k_ext_len(void);
static FILE *wav; static long wn;
static void wav_open(const char *p){ wav=fopen(p,"wb"); fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data\0\0\0\0",1,44,wav); }
static void wav_put(float l,float r){ short s[2]={(short)fmaxf(-32767,fminf(32767,l*32767)),(short)fmaxf(-32767,fminf(32767,r*32767))}; fwrite(s,2,2,wav); wn++; }
static void wav_close(void){ unsigned d=wn*4,rr=d+36; fseek(wav,4,0); fwrite(&rr,4,1,wav); fseek(wav,40,0); fwrite(&d,4,1,wav); fclose(wav); }
int main(int argc,char**argv){
  const double SR=48000, BPM=120; const int N=128;
  e_init(SR);
  float ph[N], L[N], R[N]; double beat=0; int fails=0;
  wav_open(argc>1?argv[1]:"out.wav");
  /* 1 s stopped: phase still */
  for(int b=0;b<SR/N;b++){ for(int i=0;i<N;i++) ph[i]=0; e_process(ph,L,R,N); }
  /* Live starts at bar 3 (beat 8): the phase moves, the transport and song time come ~20 ms later */
  beat=8.0; double peak=0; int told=0; int maxerr=0; int checked=0;
  for(long s=0;s<SR*12;){
    for(int i=0;i<N;i++,s++){ ph[i]=(float)(beat-floor(beat)); beat+=BPM/60.0/SR; }
    if(!told && s>SR*0.02){ e_push(EV_POS,0,(int)(beat*1000)); e_push(EV_TRANSPORT,1,0); told=1; }
    e_process(ph,L,R,N);
    for(int i=0;i<N;i++){ wav_put(L[i],R[i]); if(fabsf(L[i])>peak) peak=fabsf(L[i]); }
    if(told && s>SR*0.5){ /* the core's step against Live's 16th (the core's step is the next due) */
      int want=(int)floor(beat*4) , got=m2k_ext_step();
      int len=m2k_ext_len(); int d=((got-(want+1))%len+len)%len; if(d>len/2)d-=len;
      /* sample where the step just changed may lag a block */
      double into=beat*4-floor(beat*4); if(into>0.05&&into<0.95){ checked++; if(abs(d)>abs(maxerr)) maxerr=d; }
    }
  }
  printf("peak %.3f, steps checked %d, worst step offset %d, playing %d\n",peak,checked,maxerr,m2k_playing());
  if(peak<0.01){puts("FAIL silent");fails++;}
  if(maxerr!=0){puts("FAIL step offset");fails++;}
  /* stop */
  e_push(EV_TRANSPORT,0,0); for(int b=0;b<40;b++){ for(int i=0;i<N;i++) ph[i]=(float)(beat-floor(beat)); e_process(ph,L,R,N);} 
  printf("after stop playing %d\n",m2k_playing()); if(m2k_playing()){puts("FAIL stop");fails++;}
  /* state: change the pattern's tempo via a knob turn on GLO? use the master encoder + a machine change, then save */
  static uint8_t st[E_STATE_MAX], st2[E_STATE_MAX]; int n=0;
  for(int b=0;b<SR*3/N && !n;b++){ e_process(NULL,L,R,N); n=e_state_take(st,sizeof st); }
  printf("state %d bytes\n",n); if(n<=0){puts("FAIL no state");fails++;}
  e_push(EV_ENC,E_ALGO,3);   /* the track's machine changes */
  int n2=0; for(int b=0;b<SR*5/N && !n2;b++){ e_process(NULL,L,R,N); n2=e_state_take(st2,sizeof st2); }
  printf("state after edit %d bytes, differs %d\n",n2, n2&&(n2!=n||memcmp(st,st2,n))); if(!n2){puts("FAIL edit not saved");fails++;}
  /* encode/decode, reload the first, then the next save should equal it again */
  int na=e_state_atoms(n); int32_t *at=malloc(4*na); e_state_encode(st,n,at);
  for(int i=0;i<na;i++) if(at[i]<0||at[i]>=(1<<24)){puts("FAIL atom range");fails++;break;}
  static uint8_t dec[E_STATE_MAX]; int dn=e_state_decode(at,na,dec,sizeof dec);
  if(dn!=n||memcmp(dec,st,n)){puts("FAIL decode");fails++;}
  at[10]^=1; if(e_state_decode(at,na,dec,sizeof dec)>=0){puts("FAIL checksum");fails++;} at[10]^=1;
  e_state_put(dec,dn);
  for(int b=0;b<SR/N;b++) e_process(NULL,L,R,N);   /* the reload played in */
  e_push(EV_ENC,E_ALGO,3);     /* the same edit as before the reload: the save should equal st2's first edit */
  int n3=0; static uint8_t st3[E_STATE_MAX]; for(int b=0;b<SR*5/N && !n3;b++){ e_process(NULL,L,R,N); n3=e_state_take(st3,sizeof st3); }
  printf("reloaded and edited again: %d bytes, patterns same as the first edit %d\n",n3, n3==n2 && !memcmp(st3+26,st2+26,n3-26));
  { int d12=0,d13=0,d23=0; for(int i=0;i<n;i++){d12+=st[i]!=st2[i]; d13+=st[i]!=st3[i]; d23+=st2[i]!=st3[i];}
    printf("bytes differing: orig-vs-edit1 %d, orig-vs-edit2 %d, edit1-vs-edit2 %d\n",d12,d13,d23);
  }
  if(!(n3==n2 && !memcmp(st3+26,st2+26,n3-26))){puts("FAIL reload");fails++;}   /* (past the header and the 16 settings) */
  wav_close();
  printf(fails?"FAILED %d\n":"ALL OK\n",fails); return fails;
}
