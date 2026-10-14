/* Spatial Ink 0.1.0 - Windows x64 CLAP spatial drawing effect.
   Copyright (c) 2026. MIT license. See LICENSE.txt.
   Analytic binaural approximation: ITD, ILD, head shadow and diffuse room.
   No measured/personalized HRTF dataset. No allocations or locks in process(). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <xmmintrin.h>
#include <clap/clap.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Realtime path and parameter atomics must be lock-free");

#define PI 3.14159265358979323846
#define POINTS 256
#define QUEUE 1024
#define WIDTH 1040
#define HEIGHT 730
enum { REACH, ROOM, AMBIENCE, BARS, PHASE, HEIGHT_P, MIX, OUTPUT, MOTION, BYPASS, NPARAM };
typedef struct { const char *name; double min,max,def; bool stepped; } Param;
static const Param definitions[NPARAM]={
 {"Maximum distance",1,100,30,false}, {"Room size",0,1,.55,false},
 {"Ambience",0,1,.38,false}, {"Cycle length (bars)",1,16,4,true},
 {"Path position",0,1,0,false}, {"Height (degrees)",-90,90,0,false},
 {"Mix",0,1,1,false}, {"Output (dB)",-24,6,-3,false},
 {"Follow transport",0,1,1,true}, {"Bypass",0,1,0,true}
};
typedef struct {float x,y;} Point;
typedef struct {uint16_t type,id; uint32_t generation; double value;} UiEvent;
typedef struct {float *data; uint32_t size,pos; double lp;} Delay;
typedef struct Plugin {
 clap_plugin_t clap;
 const clap_host_t *host;
 const clap_host_params_t *host_params;
 const clap_host_state_t *host_state;
 const clap_host_gui_t *host_gui;
 _Atomic uint32_t param[NPARAM], path[POINTS], count, sequence;
 _Atomic uint32_t qr,qw,generation, display_phase, display_distance;
 UiEvent queue[QUEUE];
 Point cached[POINTS], draft[POINTS], undo[POINTS];
 uint32_t cached_count,draft_count,undo_count;
 double smooth[NPARAM], sr, free_beats, phase_seen, x,y;
 double gain[2],cut[2], itd[2],shadow[2], low[2], room_send, out_gain;
 double notch_b0,notch_b1,notch_b2,notch_a1,notch_a2, nz1[2],nz2[2];
 double feedback[8],delay_samples[8],damp_coeff;
 Delay direct,reverb[8];
 uint32_t tick;
 bool active,processing;
 HWND window;
 bool floating, drawing, place_mode;
 int drag_param;
 double scale;
 HFONT font,small,title;
} Plugin;

static HINSTANCE module_instance;
static LONG entry_refs;
static double clampd(double x,double lo,double hi){return x<lo?lo:x>hi?hi:x;}
static uint32_t float_bits(float f){uint32_t u; memcpy(&u,&f,4);return u;}
static float bits_float(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static double value(Plugin*p,int id){return bits_float(atomic_load_explicit(&p->param[id],memory_order_relaxed));}
static void set_value(Plugin*p,int id,double v){
 if(id<0||id>=NPARAM||!isfinite(v))return;
 v=clampd(v,definitions[id].min,definitions[id].max);
 if(definitions[id].stepped)v=floor(v+.5);
 atomic_store_explicit(&p->param[id],float_bits((float)v),memory_order_relaxed);
}
static uint32_t pack_point(Point pt){
 int16_t x=(int16_t)lround(clampd(pt.x,-1,1)*32767),y=(int16_t)lround(clampd(pt.y,-1,1)*32767);
 return (uint16_t)x|((uint32_t)(uint16_t)y<<16);
}
static Point unpack_point(uint32_t u){return (Point){(float)(int16_t)u/32767.f,(float)(int16_t)(u>>16)/32767.f};}
static void store_path(Plugin*p,const Point*pts,uint32_t count){
 if(!count||count>POINTS)return;
 atomic_fetch_add_explicit(&p->sequence,1,memory_order_seq_cst);
 for(uint32_t i=0;i<count;i++)atomic_store_explicit(&p->path[i],pack_point(pts[i]),memory_order_seq_cst);
 atomic_store_explicit(&p->count,count,memory_order_seq_cst);
 atomic_fetch_add_explicit(&p->sequence,1,memory_order_seq_cst);
}
static void publish_path(Plugin*p,const Point*pts,uint32_t count){store_path(p,pts,count);if(p->host_state)p->host_state->mark_dirty(p->host);}
/* Bounded seqlock snapshot: if a UI edit overlaps, use the last complete path. */
static void snapshot_path(Plugin*p){
 uint32_t before=atomic_load_explicit(&p->sequence,memory_order_seq_cst);
 if(before&1)return;
 Point temp[POINTS];uint32_t n=atomic_load_explicit(&p->count,memory_order_seq_cst);
 if(!n||n>POINTS)return;
 for(uint32_t i=0;i<n;i++)temp[i]=unpack_point(atomic_load_explicit(&p->path[i],memory_order_seq_cst));
 if(before==atomic_load_explicit(&p->sequence,memory_order_seq_cst)){
  memcpy(p->cached,temp,n*sizeof(Point));p->cached_count=n;
 }
}
static Point position(const Point*pts,uint32_t n,double phase){
 if(!n)return (Point){0,-.5f}; if(n==1)return pts[0];
 double f=clampd(phase,0,1)*(n-1);uint32_t i=(uint32_t)f;
 if(i>=n-1)return pts[n-1];double t=f-i;
 return(Point){(float)(pts[i].x+(pts[i+1].x-pts[i].x)*t),(float)(pts[i].y+(pts[i+1].y-pts[i].y)*t)};
}
static void save_undo(Plugin*p){
 p->undo_count=atomic_load(&p->count);
 for(uint32_t i=0;i<p->undo_count;i++)p->undo[i]=unpack_point(atomic_load(&p->path[i]));
}
static void request_flush(Plugin*p){if(p->host_params)p->host_params->request_flush(p->host);else if(p->host&&p->host->request_process)p->host->request_process(p->host);}
static void ui_queue(Plugin*p,int type,int id,double v){
 uint32_t w=atomic_load_explicit(&p->qw,memory_order_relaxed),r=atomic_load_explicit(&p->qr,memory_order_acquire);
 if(w-r<QUEUE){p->queue[w%QUEUE]=(UiEvent){(uint16_t)type,(uint16_t)id,atomic_load(&p->generation),v};atomic_store_explicit(&p->qw,w+1,memory_order_release);}
 request_flush(p);
}
static void ui_set(Plugin*p,int id,double v){set_value(p,id,v);ui_queue(p,CLAP_EVENT_PARAM_VALUE,id,value(p,id));}
static void ui_triplet(Plugin*p,int id,double v){ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,id,0);ui_set(p,id,v);ui_queue(p,CLAP_EVENT_PARAM_GESTURE_END,id,0);}
static void send_ui(Plugin*p,const clap_output_events_t*out){
 if(!out||!out->try_push)return;
 uint32_t r=atomic_load_explicit(&p->qr,memory_order_relaxed),w=atomic_load_explicit(&p->qw,memory_order_acquire);
 while(r!=w){
  UiEvent e=p->queue[r%QUEUE];bool ok;
  if(e.generation!=atomic_load(&p->generation)){r++;continue;}
  if(e.type==CLAP_EVENT_PARAM_VALUE){
   clap_event_param_value_t v={0};v.header=(clap_event_header_t){sizeof(v),0,CLAP_CORE_EVENT_SPACE_ID,e.type,CLAP_EVENT_IS_LIVE};
   v.param_id=e.id;v.note_id=-1;v.port_index=v.channel=v.key=-1;v.value=e.value;
   ok=out->try_push(out,&v.header);
  }else{
   clap_event_param_gesture_t g={{sizeof(g),0,CLAP_CORE_EVENT_SPACE_ID,e.type,CLAP_EVENT_IS_LIVE},e.id};
   ok=out->try_push(out,&g.header);
  }
  if(!ok)break;r++;
 }
 atomic_store_explicit(&p->qr,r,memory_order_release);
}
static void receive_event(Plugin*p,const clap_event_header_t*e){
 if(e&&e->space_id==CLAP_CORE_EVENT_SPACE_ID&&e->type==CLAP_EVENT_PARAM_VALUE&&e->size>=sizeof(clap_event_param_value_t)){
  const clap_event_param_value_t*v=(const void*)e;
  if(v->param_id<NPARAM&&v->note_id==-1&&v->port_index==-1&&v->channel==-1&&v->key==-1)set_value(p,(int)v->param_id,v->value);
 }
}
static void preset(Plugin*p,int which,bool inform){
 Point pts[POINTS];uint32_t n=128;
 if(which==0){for(uint32_t i=0;i<n;i++){double a=2*PI*i/(n-1);pts[i]=(Point){(float)(.5*sin(a)),(float)(-.5*cos(a))};}}
 else if(which==1){for(uint32_t i=0;i<n;i++){double t=(double)i/(n-1);double r=.94*(1-t)+.045*t;double a=PI*(1-t)+PI*.5*t;pts[i]=(Point){(float)(r*sin(a)),(float)(-r*cos(a))};}}
 else{n=1;pts[0]=(Point){-.10f,-.025f};}
 publish_path(p,pts,n);
 double reach=which==1?100:which==2?2:30;
 double room=which==1?.94:which==2?.06:.55;
 double amb=which==2?.10:.38;
 if(inform){ui_triplet(p,REACH,reach);ui_triplet(p,ROOM,room);ui_triplet(p,AMBIENCE,amb);ui_triplet(p,MOTION,which==2?0:1);ui_triplet(p,PHASE,0);}
 else{set_value(p,REACH,reach);set_value(p,ROOM,room);set_value(p,AMBIENCE,amb);set_value(p,MOTION,which==2?0:1);}
}

static bool alloc_delay(Delay*d,uint32_t size){d->data=calloc(size,sizeof(float));d->size=size;d->pos=0;d->lp=0;return d->data!=NULL;}
static void release_delay(Delay*d){free(d->data);memset(d,0,sizeof(*d));}
static double read_delay(Delay*d,double samples){
 samples=clampd(samples,1,d->size-2);uint32_t whole=(uint32_t)samples;double frac=samples-whole;
 uint32_t a=(d->pos+d->size-whole)%d->size,b=(a+d->size-1)%d->size;
 return d->data[a]*(1-frac)+d->data[b]*frac;
}
static void clear_dsp(Plugin*p){
 if(p->direct.data)memset(p->direct.data,0,p->direct.size*sizeof(float));p->direct.pos=0;
 for(int j=0;j<8;j++){if(p->reverb[j].data)memset(p->reverb[j].data,0,p->reverb[j].size*sizeof(float));p->reverb[j].pos=0;p->reverb[j].lp=0;}
 memset(p->low,0,sizeof(p->low));memset(p->nz1,0,sizeof(p->nz1));memset(p->nz2,0,sizeof(p->nz2));
 for(int j=0;j<NPARAM;j++)p->smooth[j]=value(p,j);
 snapshot_path(p);Point pt=position(p->cached,p->cached_count,value(p,PHASE));p->x=pt.x;p->y=pt.y;
 p->free_beats=0;p->tick=0;
}
static void coefficients(Plugin*p){
 double r=clampd(hypot(p->x,p->y),0,1),reach=p->smooth[REACH];
 double d=.15*pow(reach/.15,r);
 double angle=r>1e-6?atan2(p->x,-p->y):0,pan=sin(angle),rear=(1-cos(angle))*.5;
 double sx=d*sin(angle),sz=d*cos(angle),h=.0875;
 double ears[2]={hypot(sx+h,sz),hypot(sx-h,sz)};
 double att=pow(.3/fmax(.3,d),.58);
 for(int c=0;c<2;c++){
  double side=c==0?-pan:pan,ear_gain=clampd(d/fmax(.09,ears[c]),.6,1.4);
  p->gain[c]=att*sqrt(fmax(.04,(1+.80*side)*.5))*ear_gain;
  p->itd[c]=1+clampd((ears[c]-fmin(ears[0],ears[1]))/343*p->sr,0,.001*p->sr);
  double head=clampd(-side,0,1),air=18000/(1+.045*d);
  double cutoff=fmin(air,18000/(1+4.8*head+1.2*rear));
  p->cut[c]=1-exp(-2*PI*clampd(cutoff,500,.45*p->sr)/p->sr);
  p->shadow[c]=head*.85+rear*.15;
 }
 /* Approximate elevation/rear spectral cue; not a measured HRTF. */
 double freq=clampd(7500+p->smooth[HEIGHT_P]*25-1200*rear,4000,.40*p->sr);
 double omega=2*PI*freq/p->sr,alpha=sin(omega)/(2*2.2),a0=1+alpha;
 p->notch_b0=1/a0;p->notch_b1=-2*cos(omega)/a0;p->notch_b2=p->notch_b0;
 p->notch_a1=p->notch_b1;p->notch_a2=(1-alpha)/a0;
 double room=p->smooth[ROOM],rt60=.22+room*7.78;
 const double base[8]={.0297,.0371,.0433,.0539,.0617,.0719,.0833,.0971};
 for(int j=0;j<8;j++){
  p->delay_samples[j]=base[j]*(.48+room*2.25)*p->sr;
  p->feedback[j]=pow(.001,p->delay_samples[j]/p->sr/rt60);
 }
 p->damp_coeff=1-exp(-2*PI*(2200+3600*(1-room))/p->sr);
 p->room_send=p->smooth[AMBIENCE]*(.18+.65*(1-att))*pow(.3/fmax(.3,d),.12);
 p->out_gain=pow(10,p->smooth[OUTPUT]/20);
 atomic_store_explicit(&p->display_distance,float_bits((float)d),memory_order_relaxed);
}
static void sample_dsp(Plugin*p,double l,double r,double*outl,double*outr){
 double mono=.5*(l+r);p->direct.data[p->direct.pos]=(float)mono;
 double wet[2];
 for(int c=0;c<2;c++){
  double v=read_delay(&p->direct,p->itd[c]);p->low[c]+=p->cut[c]*(v-p->low[c]);
  v=p->low[c];double notch=p->notch_b0*v+p->nz1[c];
  p->nz1[c]=p->notch_b1*v-p->notch_a1*notch+p->nz2[c];p->nz2[c]=p->notch_b2*v-p->notch_a2*notch;
  double amount=.12+.28*fabs(p->smooth[HEIGHT_P])/90+.16*(p->y>0?1:0);
  wet[c]=(v+(notch-v)*amount)*p->gain[c];
 }
 double rv[8],matrix[8];
 for(int j=0;j<8;j++){
  double v=read_delay(&p->reverb[j],p->delay_samples[j]);p->reverb[j].lp+=p->damp_coeff*(v-p->reverb[j].lp);
  rv[j]=matrix[j]=p->reverb[j].lp;
 }
 for(int step=1;step<8;step*=2)for(int group=0;group<8;group+=2*step)for(int j=0;j<step;j++){
  double a=matrix[group+j],b=matrix[group+j+step];matrix[group+j]=a+b;matrix[group+j+step]=a-b;
 }
 double feed=read_delay(&p->direct,(.008+.07*p->smooth[ROOM])*p->sr)*.27;
 for(int j=0;j<8;j++){
  Delay*d=&p->reverb[j];double v=feed*(j&1?-1:1)+matrix[j]*.3535533905932738*p->feedback[j];
  if(fabs(v)<1e-25)v=0;d->data[d->pos]=(float)v;d->pos=(d->pos+1)%d->size;
 }
 wet[0]+=p->room_send*(rv[0]+rv[2]-rv[4]+rv[6])*.50;
 wet[1]+=p->room_send*(rv[1]-rv[3]+rv[5]+rv[7])*.50;
 p->direct.pos=(p->direct.pos+1)%p->direct.size;
 double mix=p->smooth[MIX];
 *outl=(l*(1-mix)+wet[0]*mix)*p->out_gain;*outr=(r*(1-mix)+wet[1]*mix)*p->out_gain;
 /* Transparent below 0.98; prevent accidental overload when close to an ear. */
 if(fabs(*outl)>.98)*outl=copysign(.98+.02*tanh((fabs(*outl)-.98)/.02),*outl);
 if(fabs(*outr)>.98)*outr=copysign(.98+.02*tanh((fabs(*outr)-.98)/.02),*outr);
 if(!isfinite(*outl))*outl=0;if(!isfinite(*outr))*outr=0;
}

static bool CLAP_ABI plugin_init(const clap_plugin_t*clap){
 Plugin*p=clap->plugin_data;
 if(p->host->get_extension){p->host_params=p->host->get_extension(p->host,CLAP_EXT_PARAMS);p->host_state=p->host->get_extension(p->host,CLAP_EXT_STATE);p->host_gui=p->host->get_extension(p->host,CLAP_EXT_GUI);}
 return true;
}
static void gui_free(Plugin*p);
static void CLAP_ABI deactivate(const clap_plugin_t*clap){Plugin*p=clap->plugin_data;release_delay(&p->direct);for(int j=0;j<8;j++)release_delay(&p->reverb[j]);p->active=false;p->processing=false;}
static void CLAP_ABI destroy(const clap_plugin_t*clap){Plugin*p=clap->plugin_data;gui_free(p);deactivate(clap);free(p);}
static bool CLAP_ABI activate(const clap_plugin_t*clap,double sr,uint32_t min,uint32_t max){
 (void)min;(void)max;Plugin*p=clap->plugin_data;if(!isfinite(sr)||sr<1000||sr>768000||p->active)return false;
 p->sr=sr;if(!alloc_delay(&p->direct,(uint32_t)(sr*.16)+8)){deactivate(clap);return false;}
 for(int j=0;j<8;j++)if(!alloc_delay(&p->reverb[j],(uint32_t)(sr*.32)+8)){deactivate(clap);return false;}
 p->active=true;clear_dsp(p);coefficients(p);return true;
}
static bool CLAP_ABI start(const clap_plugin_t*c){Plugin*p=c->plugin_data;p->processing=p->active;return p->processing;}
static void CLAP_ABI stop(const clap_plugin_t*c){((Plugin*)c->plugin_data)->processing=false;}
static void CLAP_ABI reset(const clap_plugin_t*c){Plugin*p=c->plugin_data;clear_dsp(p);if(p->active)coefficients(p);}
static double read_audio(const clap_audio_buffer_t*b,uint32_t c,uint32_t i){
 if(c>=b->channel_count)c=0;double v=b->data64?b->data64[c][i]:b->data32?b->data32[c][i]:0;return isfinite(v)?v:0;
}
static clap_process_status CLAP_ABI process(const clap_plugin_t*c,const clap_process_t*pr){
 Plugin*p=c->plugin_data;if(!p->active||!pr||pr->audio_outputs_count!=1||pr->audio_outputs[0].channel_count<2)return CLAP_PROCESS_ERROR;
 clap_audio_buffer_t*out=&pr->audio_outputs[0];if(!out->data32&&!out->data64)return CLAP_PROCESS_ERROR;
 const clap_audio_buffer_t*in=pr->audio_inputs_count?&pr->audio_inputs[0]:NULL;
 send_ui(p,pr->out_events);snapshot_path(p);
 double targets[NPARAM];for(int k=0;k<NPARAM;k++)targets[k]=value(p,k);
 double smooth_coeff=1-exp(-1/(.035*p->sr)),room_coeff=1-exp(-1/(.5*p->sr));
 uint32_t ei=0,ec=pr->in_events?pr->in_events->size(pr->in_events):0;
 const clap_event_header_t*event=ec?pr->in_events->get(pr->in_events,0):NULL;
 const clap_event_transport_t*tr=pr->transport;
 double beat=p->free_beats,tempo=120,tempo_inc=0,barbeats=4;bool playing=true;
 if(tr){
  playing=(tr->flags&CLAP_TRANSPORT_IS_PLAYING)!=0;
  if((tr->flags&CLAP_TRANSPORT_HAS_TEMPO)&&isfinite(tr->tempo)&&tr->tempo>0){tempo=tr->tempo;tempo_inc=isfinite(tr->tempo_inc)?tr->tempo_inc:0;}
  if(tr->flags&CLAP_TRANSPORT_HAS_BEATS_TIMELINE)beat=(double)tr->song_pos_beats/CLAP_BEATTIME_FACTOR;
  if((tr->flags&CLAP_TRANSPORT_HAS_TIME_SIGNATURE)&&tr->tsig_denom&&tr->tsig_num)barbeats=tr->tsig_num*4.0/tr->tsig_denom;
 }
 unsigned int oldcsr=_mm_getcsr();_mm_setcsr(oldcsr|0x8040);
 for(uint32_t i=0;i<pr->frames_count;i++){
  while(event&&event->time<=i){
   receive_event(p,event);
   if(event->space_id==CLAP_CORE_EVENT_SPACE_ID&&event->type==CLAP_EVENT_TRANSPORT&&event->size>=sizeof(clap_event_transport_t)){
    tr=(const void*)event;playing=(tr->flags&CLAP_TRANSPORT_IS_PLAYING)!=0;
    if((tr->flags&CLAP_TRANSPORT_HAS_TEMPO)&&isfinite(tr->tempo)&&tr->tempo>0){tempo=tr->tempo;tempo_inc=isfinite(tr->tempo_inc)?tr->tempo_inc:0;}
    if(tr->flags&CLAP_TRANSPORT_HAS_BEATS_TIMELINE)beat=(double)tr->song_pos_beats/CLAP_BEATTIME_FACTOR;
    if((tr->flags&CLAP_TRANSPORT_HAS_TIME_SIGNATURE)&&tr->tsig_denom&&tr->tsig_num)barbeats=tr->tsig_num*4.0/tr->tsig_denom;
   }
   ei++;event=ei<ec?pr->in_events->get(pr->in_events,ei):NULL;
   for(int k=0;k<NPARAM;k++)targets[k]=value(p,k);
  }
  for(int k=0;k<NPARAM;k++){double a=(k==ROOM?room_coeff:smooth_coeff);if(definitions[k].stepped)p->smooth[k]=targets[k];else p->smooth[k]+=a*(targets[k]-p->smooth[k]);}
  double phase=p->smooth[PHASE];
  if(targets[MOTION]>=.5){double duration=fmax(1,targets[BARS])*barbeats;phase=fmod(beat/duration,1);if(phase<0)phase+=1;}
  Point pt=position(p->cached,p->cached_count,phase);p->x+=smooth_coeff*(pt.x-p->x);p->y+=smooth_coeff*(pt.y-p->y);
  if((p->tick++&31)==0)coefficients(p);
  double l=0,r=0,a=0,b=0;if(in&&in->channel_count&&(in->data32||in->data64)){l=read_audio(in,0,i);r=read_audio(in,1,i);}
  sample_dsp(p,l,r,&a,&b);if(targets[BYPASS]>=.5){a=l;b=r;}
  if(out->data64){out->data64[0][i]=a;out->data64[1][i]=b;}else{out->data32[0][i]=(float)a;out->data32[1][i]=(float)b;}
  p->phase_seen=phase;if(playing)beat+=tempo/(60*p->sr);tempo=clampd(tempo+tempo_inc,1,999);
 }
 _mm_setcsr(oldcsr);p->free_beats=beat;out->constant_mask=0;
 atomic_store_explicit(&p->display_phase,float_bits((float)p->phase_seen),memory_order_relaxed);
 return CLAP_PROCESS_CONTINUE;
}

static uint32_t CLAP_ABI ports_count(const clap_plugin_t*c,bool in){(void)c;(void)in;return 1;}
static bool CLAP_ABI ports_get(const clap_plugin_t*c,uint32_t index,bool in,clap_audio_port_info_t*info){
 (void)c;if(index)return false;memset(info,0,sizeof(*info));info->id=0;
 snprintf(info->name,sizeof(info->name),"%s",in?"Source (stereo downmix)":"Headphone stereo");
 info->flags=CLAP_AUDIO_PORT_IS_MAIN|CLAP_AUDIO_PORT_SUPPORTS_64BITS|CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
 info->channel_count=2;info->port_type=CLAP_PORT_STEREO;info->in_place_pair=0;return true;
}
static const clap_plugin_audio_ports_t ports={ports_count,ports_get};
static uint32_t CLAP_ABI params_count(const clap_plugin_t*c){(void)c;return NPARAM;}
static bool CLAP_ABI params_info(const clap_plugin_t*c,uint32_t index,clap_param_info_t*info){
 (void)c;if(index>=NPARAM)return false;memset(info,0,sizeof(*info));info->id=index;info->flags=CLAP_PARAM_IS_AUTOMATABLE;
 if(definitions[index].stepped)info->flags|=CLAP_PARAM_IS_STEPPED;if(index==BYPASS)info->flags|=CLAP_PARAM_IS_BYPASS;
 snprintf(info->name,sizeof(info->name),"%s",definitions[index].name);snprintf(info->module,sizeof(info->module),"Spatial Ink");
 info->min_value=definitions[index].min;info->max_value=definitions[index].max;info->default_value=definitions[index].def;return true;
}
static bool CLAP_ABI params_value(const clap_plugin_t*c,clap_id id,double*out){if(id>=NPARAM)return false;*out=value(c->plugin_data,id);return true;}
static bool CLAP_ABI params_text(const clap_plugin_t*c,clap_id id,double v,char*b,uint32_t n){
 (void)c;if(id>=NPARAM||!b||!n)return false;
 if(id==REACH)snprintf(b,n,"%.1f m",v);else if(id==BARS)snprintf(b,n,"%.0f bars",v);else if(id==HEIGHT_P)snprintf(b,n,"%+.0f deg",v);
 else if(id==OUTPUT)snprintf(b,n,"%+.1f dB",v);else if(id==MOTION)snprintf(b,n,"%s",v>=.5?"Follow":"Manual");
 else if(id==BYPASS)snprintf(b,n,"%s",v>=.5?"Bypass":"Active");else snprintf(b,n,"%.0f %%",v*100);return true;
}
static bool CLAP_ABI params_parse(const clap_plugin_t*c,clap_id id,const char*b,double*v){
 (void)c;if(id>=NPARAM||!b||!v)return false;char*end;double d=strtod(b,&end);
 if(end==b){if(id==MOTION){if(!_stricmp(b,"Follow"))d=1;else if(!_stricmp(b,"Manual"))d=0;else return false;}
 else if(id==BYPASS){if(!_stricmp(b,"Bypass"))d=1;else if(!_stricmp(b,"Active"))d=0;else return false;}else return false;}
 else if(strchr(end,'%')&&(id==ROOM||id==AMBIENCE||id==PHASE||id==MIX))d/=100;
 if(!isfinite(d))return false;*v=clampd(d,definitions[id].min,definitions[id].max);return true;
}
static void CLAP_ABI params_flush(const clap_plugin_t*c,const clap_input_events_t*in,const clap_output_events_t*out){
 Plugin*p=c->plugin_data;send_ui(p,out);if(in)for(uint32_t i=0;i<in->size(in);i++)receive_event(p,in->get(in,i));
}
static const clap_plugin_params_t params={params_count,params_info,params_value,params_text,params_parse,params_flush};

typedef struct {uint32_t magic,version,count,reserved;float params[NPARAM];uint32_t pts[POINTS];} State;
static bool write_all(const clap_ostream_t*s,const void*data,uint64_t n){const char*p=data;while(n){int64_t k=s->write(s,p,n);if(k<=0||(uint64_t)k>n)return false;p+=k;n-=k;}return true;}
static bool read_all(const clap_istream_t*s,void*data,uint64_t n){char*p=data;while(n){int64_t k=s->read(s,p,n);if(k<=0||(uint64_t)k>n)return false;p+=k;n-=k;}return true;}
static bool CLAP_ABI state_save(const clap_plugin_t*c,const clap_ostream_t*s){
 Plugin*p=c->plugin_data;State st={0};st.magic=0x4b4e4953;st.version=1;st.count=atomic_load(&p->count);
 for(int k=0;k<NPARAM;k++)st.params[k]=(float)value(p,k);for(uint32_t i=0;i<st.count;i++)st.pts[i]=atomic_load(&p->path[i]);
 return write_all(s,&st,sizeof(st));
}
static bool CLAP_ABI state_load(const clap_plugin_t*c,const clap_istream_t*s){
 Plugin*p=c->plugin_data;State st;
 if(!read_all(s,&st,sizeof(st))||st.magic!=0x4b4e4953||st.version!=1||!st.count||st.count>POINTS)return false;
 for(int k=0;k<NPARAM;k++)if(!isfinite(st.params[k])||st.params[k]<definitions[k].min||st.params[k]>definitions[k].max)return false;
 Point pts[POINTS];for(uint32_t i=0;i<st.count;i++)pts[i]=unpack_point(st.pts[i]);
 /* Validate the complete state before replacing anything. */
 for(int k=0;k<NPARAM;k++)set_value(p,k,st.params[k]);
 atomic_fetch_add(&p->generation,1);
 store_path(p,pts,st.count);p->undo_count=0;
 if(p->host_params)p->host_params->rescan(p->host,CLAP_PARAM_RESCAN_VALUES);
 if(p->window)InvalidateRect(p->window,NULL,FALSE);return true;
}
static const clap_plugin_state_t state={state_save,state_load};
static uint32_t CLAP_ABI tail_get(const clap_plugin_t*c){Plugin*p=c->plugin_data;return(uint32_t)(p->sr*9);}
static const clap_plugin_tail_t tail={tail_get};
static uint32_t CLAP_ABI latency_get(const clap_plugin_t*c){(void)c;return 0;}
static const clap_plugin_latency_t latency={latency_get};

/* Native, embedded Win32 GUI. All coordinates below are logical pixels. */
static COLORREF bg=RGB(12,18,25),panel=RGB(19,29,38),line=RGB(42,62,72),text=RGB(227,237,239),muted=RGB(133,156,165),mint=RGB(87,222,186),orange=RGB(255,175,91);
static void rect_fill(HDC dc,int x,int y,int w,int h,COLORREF color){RECT r={x,y,x+w,y+h};HBRUSH b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);}
static void draw_line(HDC dc,int x,int y,int xx,int yy,COLORREF col,int width){HPEN pen=CreatePen(PS_SOLID,width,col);HGDIOBJ old=SelectObject(dc,pen);MoveToEx(dc,x,y,NULL);LineTo(dc,xx,yy);SelectObject(dc,old);DeleteObject(pen);}
static void ellipse(HDC dc,int x,int y,int w,int h,COLORREF stroke,COLORREF fill,int width){HPEN pen=CreatePen(PS_SOLID,width,stroke);HBRUSH brush=CreateSolidBrush(fill);HGDIOBJ a=SelectObject(dc,pen),b=SelectObject(dc,brush);Ellipse(dc,x,y,x+w,y+h);SelectObject(dc,a);SelectObject(dc,b);DeleteObject(pen);DeleteObject(brush);}
static void label(HDC dc,HFONT font,int x,int y,const char*s,COLORREF color){HGDIOBJ old=SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);TextOutA(dc,x,y,s,(int)strlen(s));SelectObject(dc,old);}
static void button(HDC dc,Plugin*p,int x,int y,int w,const char*s,bool selected){rect_fill(dc,x,y,w,32,selected?mint:panel);label(dc,p->small,x+12,y+7,s,selected?bg:text);}
static double normalized(Plugin*p,int id){double v=value(p,id);if(id==REACH)return log(v)/log(100);return (v-definitions[id].min)/(definitions[id].max-definitions[id].min);}
static int slider_y(int id){return 143+id*55;}
static void draw_gui(Plugin*p,HDC dc){
 rect_fill(dc,0,0,WIDTH,HEIGHT,bg);
 label(dc,p->title,28,22,"SPATIAL INK",text);label(dc,p->small,29,60,"DRAW THE SPACE. HEAR THE DISTANCE.",muted);
 label(dc,p->small,751,29,"HEADPHONE SPATIAL EFFECT",mint);label(dc,p->small,751,53,"WINDOWS 64 / CLAP / 0.1",muted);
 button(dc,p,28,92,85,"DRAW",!p->place_mode);button(dc,p,121,92,85,"PLACE",p->place_mode);
 button(dc,p,224,92,65,"UNDO",false);button(dc,p,305,92,86,"ORBIT",false);button(dc,p,399,92,130,"FAR > EAR",false);button(dc,p,537,92,91,"WHISPER",false);button(dc,p,636,92,70,"CLEAR",false);
 rect_fill(dc,28,139,678,475,panel);
 const int cx=367,cy=377,rad=206;
 for(int ring=5;ring>=1;ring--){int r=rad*ring/5;ellipse(dc,cx-r,cy-r,2*r,2*r,line,panel,1);}
 draw_line(dc,cx-rad,cy,cx+rad,cy,line,1);draw_line(dc,cx,cy-rad,cx,cy+rad,line,1);
 label(dc,p->small,cx-21,149,"FRONT",muted);label(dc,p->small,cx-18,591,"BACK",muted);
 label(dc,p->small,49,cy-8,"LEFT",muted);label(dc,p->small,650,cy-8,"RIGHT",muted);
 char s[80];for(int ring=1;ring<=5;ring++){
  double d=.15*pow(value(p,REACH)/.15,ring/5.0);if(d>=10)snprintf(s,sizeof(s),"%.0f m",d);else if(d>=1)snprintf(s,sizeof(s),"%.1f m",d);else snprintf(s,sizeof(s),"%.2f m",d);
  label(dc,p->small,cx+5,cy-rad*ring/5+5,s,muted);
 }
 Point pts[POINTS];uint32_t n=atomic_load(&p->count);
 if(p->drawing){memcpy(pts,p->draft,p->draft_count*sizeof(Point));n=p->draft_count;}
 else for(uint32_t i=0;i<n;i++)pts[i]=unpack_point(atomic_load(&p->path[i]));
 for(uint32_t i=1;i<n;i++)draw_line(dc,cx+(int)(pts[i-1].x*rad),cy+(int)(pts[i-1].y*rad),cx+(int)(pts[i].x*rad),cy+(int)(pts[i].y*rad),mint,3);
 if(n){ellipse(dc,cx+(int)(pts[0].x*rad)-5,cy+(int)(pts[0].y*rad)-5,10,10,mint,panel,2);label(dc,p->small,cx+(int)(pts[0].x*rad)+8,cy+(int)(pts[0].y*rad)-8,"START",mint);}
 ellipse(dc,cx-15,cy-18,30,37,muted,bg,2);ellipse(dc,cx-21,cy-5,7,14,muted,bg,1);ellipse(dc,cx+14,cy-5,7,14,muted,bg,1);
 draw_line(dc,cx-6,cy-19,cx,cy-27,mint,2);draw_line(dc,cx,cy-27,cx+6,cy-19,mint,2);label(dc,p->small,cx-12,cy+24,"YOU",text);
 double phase=bits_float(atomic_load(&p->display_phase));if(!p->active)phase=value(p,PHASE);
 Point cur=position(pts,n,phase);int xx=cx+(int)(cur.x*rad),yy=cy+(int)(cur.y*rad);
 ellipse(dc,xx-11,yy-11,22,22,orange,panel,2);ellipse(dc,xx-5,yy-5,10,10,orange,orange,1);
 double r=clampd(hypot(cur.x,cur.y),0,1),d=.15*pow(value(p,REACH)/.15,r);
 snprintf(s,sizeof(s),"%.2f m",d);label(dc,p->font,49,164,s,orange);label(dc,p->small,49,188,"SOURCE DISTANCE",muted);
 snprintf(s,sizeof(s),"%u path points",n);label(dc,p->small,567,586,s,muted);
 rect_fill(dc,732,92,280,522,panel);
 const char* names[8]={"REACH","ROOM SIZE","AMBIENCE","CYCLE LENGTH","PATH POSITION","HEIGHT","MIX","OUTPUT"};
 for(int k=0;k<8;k++){
  int y=slider_y(k);label(dc,p->small,751,y-28,names[k],muted);params_text(&p->clap,k,value(p,k),s,sizeof(s));label(dc,p->small,912,y-28,s,text);
  rect_fill(dc,751,y,240,4,line);double t=normalized(p,k);rect_fill(dc,751,y,(int)(240*t),4,k==PHASE?orange:mint);
  ellipse(dc,751+(int)(240*t)-5,y-3,10,10,text,text,1);
 }
 button(dc,p,28,631,154,"FOLLOW TRANSPORT",value(p,MOTION)>.5);button(dc,p,190,631,131,"MANUAL / SCRUB",value(p,MOTION)<.5);
 button(dc,p,888,631,124,value(p,BYPASS)>.5?"BYPASSED":"ACTIVE",value(p,BYPASS)<.5);
 rect_fill(dc,340,642,518,5,line);rect_fill(dc,340,642,(int)(518*clampd(phase,0,1)),5,orange);
 label(dc,p->small,29,685,p->drawing?"Release to commit the path. Playback uses equal distance along your drawing.":p->place_mode?"Click in the map to place a stationary source. Drag to move it.":"Draw a new path inside the circle. Follow loops to the project tempo; Manual lets you automate Path position.",muted);
}
static void refresh(Plugin*p){if(p->window)InvalidateRect(p->window,NULL,FALSE);}
static Point mouse_point(int x,int y){double a=(x-367)/206.0,b=(y-377)/206.0,r=hypot(a,b);if(r>1){a/=r;b/=r;}return (Point){(float)a,(float)b};}
static bool in_map(int x,int y){return x>=28&&x<=706&&y>=139&&y<=614;}
static void append_point(Plugin*p,Point pt){
 if(p->draft_count&&hypot(pt.x-p->draft[p->draft_count-1].x,pt.y-p->draft[p->draft_count-1].y)<.008)return;
 if(p->draft_count>=POINTS){for(uint32_t i=0;i<POINTS/2;i++)p->draft[i]=p->draft[i*2];p->draft_count=POINTS/2;}
 p->draft[p->draft_count++]=pt;
}
static void commit_draw(Plugin*p){
 if(!p->draft_count)return;
 if(p->draft_count<2){publish_path(p,p->draft,p->draft_count);return;}
 double lengths[POINTS];lengths[0]=0;for(uint32_t i=1;i<p->draft_count;i++)lengths[i]=lengths[i-1]+hypot(p->draft[i].x-p->draft[i-1].x,p->draft[i].y-p->draft[i-1].y);
 Point pts[POINTS];uint32_t n=128,index=1;double total=lengths[p->draft_count-1];
 if(total<1e-8){publish_path(p,p->draft,1);return;}
 for(uint32_t j=0;j<n;j++){
  double target=total*j/(n-1);while(index<p->draft_count-1&&lengths[index]<target)index++;
  double span=lengths[index]-lengths[index-1],t=span>0?(target-lengths[index-1])/span:0;
  pts[j]=(Point){(float)(p->draft[index-1].x+t*(p->draft[index].x-p->draft[index-1].x)),(float)(p->draft[index-1].y+t*(p->draft[index].y-p->draft[index-1].y))};
 }
 publish_path(p,pts,n);
}
static void slider_update(Plugin*p,int id,int x){double t=clampd((x-751)/240.0,0,1);double v=id==REACH?pow(100,t):definitions[id].min+t*(definitions[id].max-definitions[id].min);ui_set(p,id,v);refresh(p);}
static void release_mouse(Plugin*p){
 if(p->drawing){commit_draw(p);p->drawing=false;ui_triplet(p,MOTION,1);}
 if(p->drag_param>=0){ui_queue(p,CLAP_EVENT_PARAM_GESTURE_END,p->drag_param,0);p->drag_param=-1;}
 refresh(p);
}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 Plugin*p=(Plugin*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
 if(msg==WM_NCCREATE){p=((CREATESTRUCTW*)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)p);}
 if(!p)return DefWindowProcW(hwnd,msg,wp,lp);
 RECT cr;GetClientRect(hwnd,&cr);int width=cr.right?cr.right:WIDTH,height=cr.bottom?cr.bottom:HEIGHT;
 int x=(int)((double)GET_X_LPARAM(lp)*WIDTH/width),y=(int)((double)GET_Y_LPARAM(lp)*HEIGHT/height);
 switch(msg){
 case WM_ERASEBKGND:return 1;
 case WM_PRINTCLIENT:{int saved=SaveDC((HDC)wp);SetMapMode((HDC)wp,MM_ANISOTROPIC);SetWindowExtEx((HDC)wp,WIDTH,HEIGHT,NULL);SetViewportExtEx((HDC)wp,width,height,NULL);draw_gui(p,(HDC)wp);RestoreDC((HDC)wp,saved);return 0;}
 case WM_PAINT:{
  PAINTSTRUCT ps;HDC dst=BeginPaint(hwnd,&ps),dc=CreateCompatibleDC(dst);HBITMAP bmp=CreateCompatibleBitmap(dst,width,height);HGDIOBJ old=SelectObject(dc,bmp);
  SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,WIDTH,HEIGHT,NULL);SetViewportExtEx(dc,width,height,NULL);draw_gui(p,dc);
  SetMapMode(dc,MM_TEXT);BitBlt(dst,0,0,width,height,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);EndPaint(hwnd,&ps);return 0;}
 case WM_TIMER:refresh(p);return 0;
 case WM_LBUTTONDOWN:
  if(y>=92&&y<=124){
   if(x>=28&&x<=113)p->place_mode=false;else if(x>=121&&x<=206)p->place_mode=true;
   else if(x>=224&&x<=289&&p->undo_count){Point old[POINTS];uint32_t n=atomic_load(&p->count);for(uint32_t i=0;i<n;i++)old[i]=unpack_point(atomic_load(&p->path[i]));publish_path(p,p->undo,p->undo_count);memcpy(p->undo,old,n*sizeof(Point));p->undo_count=n;}
   else if(x>=305&&x<=391){save_undo(p);preset(p,0,true);}else if(x>=399&&x<=529){save_undo(p);preset(p,1,true);}
   else if(x>=537&&x<=628){save_undo(p);preset(p,2,true);}else if(x>=636&&x<=706){save_undo(p);Point q={0,-.3f};publish_path(p,&q,1);ui_triplet(p,MOTION,0);}
   refresh(p);return 0;
  }
  if(y>=631&&y<=663){if(x>=28&&x<=182)ui_triplet(p,MOTION,1);else if(x>=190&&x<=321)ui_triplet(p,MOTION,0);else if(x>=888&&x<=1012)ui_triplet(p,BYPASS,value(p,BYPASS)<.5?1:0);refresh(p);return 0;}
  if(x>=741&&x<=1003)for(int k=0;k<8;k++)if(abs(y-slider_y(k))<=14){
   if(k==PHASE)ui_triplet(p,MOTION,0);p->drag_param=k;ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,k,0);slider_update(p,k,x);SetCapture(hwnd);return 0;
  }
  if(in_map(x,y)){
   save_undo(p);SetCapture(hwnd);Point q=mouse_point(x,y);
   if(p->place_mode){publish_path(p,&q,1);ui_triplet(p,MOTION,0);}else{p->drawing=true;p->draft_count=0;append_point(p,q);}refresh(p);return 0;
  }break;
 case WM_MOUSEMOVE:
  if(GetCapture()==hwnd&&(wp&MK_LBUTTON)){
   if(p->drag_param>=0)slider_update(p,p->drag_param,x);
   else if(p->drawing){append_point(p,mouse_point(x,y));refresh(p);}
   else if(p->place_mode){Point q=mouse_point(x,y);publish_path(p,&q,1);refresh(p);}
  }return 0;
 case WM_LBUTTONUP:if(GetCapture()==hwnd){release_mouse(p);ReleaseCapture();}return 0;
 case WM_CAPTURECHANGED:release_mouse(p);return 0;
 case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);if(p->host_gui)p->host_gui->closed(p->host,false);return 0;
 case WM_DESTROY:KillTimer(hwnd,1);return 0;
 }return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool register_window(void){
 WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.hInstance=module_instance;wc.lpfnWndProc=window_proc;wc.lpszClassName=L"SpatialInk.CLAP.0.1";wc.hCursor=LoadCursor(NULL,IDC_ARROW);
 return RegisterClassExW(&wc)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
}
static void gui_free(Plugin*p){if(p->window){DestroyWindow(p->window);p->window=NULL;}if(p->font)DeleteObject(p->font);if(p->small)DeleteObject(p->small);if(p->title)DeleteObject(p->title);p->font=p->small=p->title=NULL;}
static bool CLAP_ABI gui_supported(const clap_plugin_t*c,const char*api,bool floating){(void)c;(void)floating;return api&&!strcmp(api,CLAP_WINDOW_API_WIN32);}
static bool CLAP_ABI gui_preferred(const clap_plugin_t*c,const char**api,bool*floating){(void)c;*api=CLAP_WINDOW_API_WIN32;*floating=false;return true;}
static bool CLAP_ABI gui_create(const clap_plugin_t*c,const char*api,bool floating){
 Plugin*p=c->plugin_data;if(p->window||!gui_supported(c,api,floating)||!register_window())return false;
 p->floating=floating;p->scale=1;p->drag_param=-1;
 p->font=CreateFontW(-19,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 p->small=CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 p->title=CreateFontW(-30,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 DWORD style=floating?(WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU):WS_POPUP;
 RECT r={0,0,WIDTH,HEIGHT};AdjustWindowRect(&r,style,FALSE);
 p->window=CreateWindowExW(0,L"SpatialInk.CLAP.0.1",L"Spatial Ink",style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,module_instance,p);
 if(!p->window){gui_free(p);return false;}SetTimer(p->window,1,33,NULL);return true;
}
static void CLAP_ABI gui_destroy(const clap_plugin_t*c){gui_free(c->plugin_data);}
static bool CLAP_ABI gui_scale(const clap_plugin_t*c,double scale){
 Plugin*p=c->plugin_data;if(!p->window||scale<.75||scale>3)return false;double old_scale=p->scale;p->scale=scale;
 RECT r={0,0,(LONG)lround(WIDTH*scale),(LONG)lround(HEIGHT*scale)};DWORD style=(DWORD)GetWindowLongPtrW(p->window,GWL_STYLE);AdjustWindowRect(&r,style,FALSE);
 if(!p->floating&&p->host_gui&&p->host_gui->request_resize){if(!p->host_gui->request_resize(p->host,r.right-r.left,r.bottom-r.top)){p->scale=old_scale;return false;}}
 return SetWindowPos(p->window,NULL,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER)!=0;
}
static bool CLAP_ABI gui_size(const clap_plugin_t*c,uint32_t*w,uint32_t*h){Plugin*p=c->plugin_data;if(!p->window)return false;RECT r;GetClientRect(p->window,&r);*w=r.right;*h=r.bottom;return true;}
static bool CLAP_ABI gui_can_resize(const clap_plugin_t*c){(void)c;return false;}
static bool CLAP_ABI gui_hints(const clap_plugin_t*c,clap_gui_resize_hints_t*h){(void)c;memset(h,0,sizeof(*h));return false;}
static bool CLAP_ABI gui_adjust(const clap_plugin_t*c,uint32_t*w,uint32_t*h){Plugin*p=c->plugin_data;*w=(uint32_t)lround(WIDTH*p->scale);*h=(uint32_t)lround(HEIGHT*p->scale);return true;}
static bool CLAP_ABI gui_set_size(const clap_plugin_t*c,uint32_t w,uint32_t h){Plugin*p=c->plugin_data;if(!p->window||w<520||h<365||w>3120||h>2190)return false;return SetWindowPos(p->window,NULL,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER)!=0;}
static bool CLAP_ABI gui_parent(const clap_plugin_t*c,const clap_window_t*parent){
 Plugin*p=c->plugin_data;if(!p->window||p->floating||!parent||strcmp(parent->api,CLAP_WINDOW_API_WIN32)||!parent->win32)return false;
 SetWindowLongPtrW(p->window,GWL_STYLE,WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN);SetLastError(0);SetParent(p->window,parent->win32);return GetLastError()==0;
}
static bool CLAP_ABI gui_transient(const clap_plugin_t*c,const clap_window_t*parent){Plugin*p=c->plugin_data;if(!p->window||!p->floating||!parent||strcmp(parent->api,CLAP_WINDOW_API_WIN32))return false;SetWindowLongPtrW(p->window,GWLP_HWNDPARENT,(LONG_PTR)parent->win32);return true;}
static void CLAP_ABI gui_title(const clap_plugin_t*c,const char*title){(void)c;(void)title;}
static bool CLAP_ABI gui_show(const clap_plugin_t*c){Plugin*p=c->plugin_data;if(!p->window)return false;ShowWindow(p->window,SW_SHOWNOACTIVATE);return true;}
static bool CLAP_ABI gui_hide(const clap_plugin_t*c){Plugin*p=c->plugin_data;if(!p->window)return false;ShowWindow(p->window,SW_HIDE);return true;}
static const clap_plugin_gui_t gui={gui_supported,gui_preferred,gui_create,gui_destroy,gui_scale,gui_size,gui_can_resize,gui_hints,gui_adjust,gui_set_size,gui_parent,gui_transient,gui_title,gui_show,gui_hide};
static const void*CLAP_ABI extension(const clap_plugin_t*c,const char*id){
 (void)c;if(!strcmp(id,CLAP_EXT_AUDIO_PORTS))return &ports;if(!strcmp(id,CLAP_EXT_PARAMS))return &params;if(!strcmp(id,CLAP_EXT_STATE))return &state;
 if(!strcmp(id,CLAP_EXT_GUI))return &gui;if(!strcmp(id,CLAP_EXT_TAIL))return &tail;if(!strcmp(id,CLAP_EXT_LATENCY))return &latency;return NULL;
}
static void CLAP_ABI main_thread(const clap_plugin_t*c){refresh(c->plugin_data);}
static const char*const features[]={CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,CLAP_PLUGIN_FEATURE_MULTI_EFFECTS,CLAP_PLUGIN_FEATURE_REVERB,CLAP_PLUGIN_FEATURE_STEREO,NULL};
static const clap_plugin_descriptor_t descriptor={CLAP_VERSION_INIT,"local.spatialink.headphone","Spatial Ink","Spatial Ink","","","","0.1.0","Draw motion from vast spaces to a close headphone source.",features};
static uint32_t CLAP_ABI factory_count(const clap_plugin_factory_t*f){(void)f;return 1;}
static const clap_plugin_descriptor_t*CLAP_ABI factory_desc(const clap_plugin_factory_t*f,uint32_t index){(void)f;return index?NULL:&descriptor;}
static const clap_plugin_t*CLAP_ABI factory_create(const clap_plugin_factory_t*f,const clap_host_t*host,const char*id){
 (void)f;if(!host||!clap_version_is_compatible(host->clap_version)||strcmp(id,descriptor.id))return NULL;
 Plugin*p=calloc(1,sizeof(*p));if(!p)return NULL;p->host=host;p->scale=1;p->drag_param=-1;
 p->clap=(clap_plugin_t){&descriptor,p,plugin_init,destroy,activate,deactivate,start,stop,reset,process,extension,main_thread};
 for(int k=0;k<NPARAM;k++){atomic_init(&p->param[k],float_bits((float)definitions[k].def));p->smooth[k]=definitions[k].def;}
 for(int k=0;k<POINTS;k++)atomic_init(&p->path[k],0);
 atomic_init(&p->count,1);atomic_init(&p->sequence,0);atomic_init(&p->qr,0);atomic_init(&p->qw,0);atomic_init(&p->generation,0);atomic_init(&p->display_phase,0);atomic_init(&p->display_distance,0);
 preset(p,0,false);snapshot_path(p);return &p->clap;
}
static const clap_plugin_factory_t factory={factory_count,factory_desc,factory_create};
static bool CLAP_ABI entry_init(const char*path){(void)path;InterlockedIncrement(&entry_refs);return true;}
static void CLAP_ABI entry_deinit(void){if(InterlockedDecrement(&entry_refs)==0)UnregisterClassW(L"SpatialInk.CLAP.0.1",module_instance);}
static const void*CLAP_ABI entry_factory(const char*id){return !strcmp(id,CLAP_PLUGIN_FACTORY_ID)?&factory:NULL;}
CLAP_EXPORT const clap_plugin_entry_t clap_entry={CLAP_VERSION_INIT,entry_init,entry_deinit,entry_factory};
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){(void)reserved;if(reason==DLL_PROCESS_ATTACH){module_instance=instance;DisableThreadLibraryCalls(instance);}return TRUE;}
