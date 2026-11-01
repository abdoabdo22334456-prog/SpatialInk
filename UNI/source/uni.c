/* UNI 0.8.0 - Windows x64 spatial effect with two independent pages.
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
#include <clap/ext/context-menu.h>
#include "tempo_clock.h"
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Realtime path and parameter atomics must be lock-free");

#define PI 3.14159265358979323846
#define POINTS 256
#define QUEUE 1024
#define WIDTH 1040
#define HEIGHT 930
/* IDs 0..11 are unchanged so existing automation lanes remain valid. */
enum { REACH, ROOM, AMBIENCE, BARS, PHASE, HEIGHT_P, MIX, OUTPUT, MOTION, BYPASS, ORBIT_SIZE, ORBIT_DIR, SOURCE_X, SOURCE_Y, POSITION_MODE, TEMPO_SOURCE, CUSTOM_BPM, SPEED, DOUBLE_MODE, POINT_B_PHASE, TWO_PAGES, B_REACH, B_ROOM, B_AMBIENCE, B_BARS, B_PHASE, B_HEIGHT, B_MIX, B_OUTPUT, B_MOTION, B_BYPASS, B_ORBIT_SIZE, B_ORBIT_DIR, B_SOURCE_X, B_SOURCE_Y, B_POSITION_MODE, B_TEMPO_SOURCE, B_CUSTOM_BPM, B_SPEED, NPARAM };
#define PAGE_PARAMS 18
#define B_BASE B_REACH
#define ORBIT_POINTS 128
#define NSLIDERS 10
static const int slider_ids[NSLIDERS]={REACH,ORBIT_SIZE,ROOM,AMBIENCE,BARS,PHASE,POINT_B_PHASE,HEIGHT_P,MIX,OUTPUT};
typedef struct { const char *name; double min,max,def; bool stepped; } Param;
static const Param definitions[NPARAM]={
 {"Maximum distance",1,100,30,false}, {"Room size",0,1,.55,false},
 {"Ambience",0,1,.38,false}, {"Cycle length (bars)",1,16,4,true},
 {"Path position",0,1,0,false}, {"Height (degrees)",-90,90,0,false},
 {"Mix",0,1,1,false}, {"Output (dB)",-24,6,-3,false},
 {"Follow transport",0,1,1,true}, {"Bypass",0,1,0,true},
 {"Orbit size",.05,.95,.5,false}, {"Direction",0,1,0,true},
 {"Source X (left-right)",-1,1,0,false}, {"Source Y (front-back)",-1,1,-.5,false},
 {"Position mode",0,1,0,true}, {"Tempo source",0,1,0,true},
 {"Custom tempo (BPM)",20,999,120,false}, {"Speed multiplier",.0625,16,1,false},
 {"Double",0,1,0,true}, {"Point B position",0,1,.5,false},
 {"Two pages",0,1,0,true},
 {"Page B: Maximum distance",1,100,30,false}, {"Page B: Room size",0,1,.55,false},
 {"Page B: Ambience",0,1,.38,false}, {"Page B: Cycle length (bars)",1,16,4,true},
 {"Page B: Path position",0,1,.5,false}, {"Page B: Height (degrees)",-90,90,0,false},
 {"Page B: Mix",0,1,1,false}, {"Page B: Output (dB)",-24,6,-3,false},
 {"Page B: Follow transport",0,1,1,true}, {"Page B: Bypass",0,1,0,true},
 {"Page B: Orbit size",.05,.95,.5,false}, {"Page B: Direction",0,1,0,true},
 {"Page B: Source X (left-right)",-1,1,0,false}, {"Page B: Source Y (front-back)",-1,1,-.5,false},
 {"Page B: Position mode",0,1,0,true}, {"Page B: Tempo source",0,1,0,true},
 {"Page B: Custom tempo (BPM)",20,999,120,false}, {"Page B: Speed multiplier",.0625,16,1,false}
};
typedef struct {float x,y;} Point;
typedef struct {uint16_t type,id; uint32_t generation; double value;} UiEvent;
typedef struct {float *data; uint32_t size,pos; double lp;} Delay;
typedef struct {
 double x,y,gain[2],cut[2],itd[2],low[2],nz1[2],nz2[2],room_send;
 double notch_b0,notch_b1,notch_b2,notch_a1,notch_a2;
} Voice;
typedef struct {
 _Atomic uint32_t path[POINTS],count,sequence,orbit,display_phase,display_phase_b,display_distance;
 Point cached[POINTS],draft[POINTS],undo[POINTS];
 uint32_t cached_count,draft_count,undo_count,tick;
 double smooth[PAGE_PARAMS],phase_seen,phase_b_seen,double_mix;
 UniTempoClock tempo_clock;
 Voice voice[2];
 double feedback[8],delay_samples[8],damp_coeff,out_gain;
 Delay direct,reverb[8];
 bool drawing,place_mode,xy_drag,undo_orbit;
 double undo_mode,undo_x,undo_y;
} Page;
typedef struct Plugin {
 clap_plugin_t clap;
 const clap_host_t *host;
 const clap_host_params_t *host_params;
 const clap_host_state_t *host_state;
 const clap_host_gui_t *host_gui;
 const clap_host_context_menu_t *host_menu;
 _Atomic uint32_t param[NPARAM];
 _Atomic uint32_t qr,qw,generation;
 _Atomic uint32_t automation_received,automation_applied,automation_last_id,automation_last_value,automation_selectors;
 _Atomic uint32_t display_bpm,display_tempo_valid;
 UiEvent queue[QUEUE];
 Page page[2];
 double sr,free_beats,dual_mix;
 bool active,processing;
 HWND window;
 HWND bpm_edit;
 WNDPROC bpm_proc;
 bool bpm_cancel;
 HBRUSH bpm_brush;
 bool floating;
 int selected_page;
 int drag_param;
 double scale;
 HFONT font,small,title;
 HFONT bpm_font;
} Plugin;

static HINSTANCE module_instance;
static LONG entry_refs;
static double clampd(double x,double lo,double hi){return x<lo?lo:x>hi?hi:x;}
static uint32_t float_bits(float f){uint32_t u; memcpy(&u,&f,4);return u;}
static float bits_float(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static double value(Plugin*p,int id){return bits_float(atomic_load_explicit(&p->param[id],memory_order_relaxed));}
static int ui_page(Plugin*p){return value(p,TWO_PAGES)>=.5?p->selected_page:0;}
static int page_param(int page,int id){return page?B_BASE+id:id;}
static int ui_param(Plugin*p,int id){return id<PAGE_PARAMS?page_param(ui_page(p),id):id;}
static double page_value(Plugin*p,int page,int id){return value(p,page_param(page,id));}
static double ui_value(Plugin*p,int id){return value(p,ui_param(p,id));}
static Page* selected(Plugin*p){return &p->page[ui_page(p)];}
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
static void store_path(Page*pg,const Point*pts,uint32_t count){
 if(!count||count>POINTS)return;
 atomic_fetch_add_explicit(&pg->sequence,1,memory_order_seq_cst);
 for(uint32_t i=0;i<count;i++)atomic_store_explicit(&pg->path[i],pack_point(pts[i]),memory_order_seq_cst);
 atomic_store_explicit(&pg->count,count,memory_order_seq_cst);
 atomic_fetch_add_explicit(&pg->sequence,1,memory_order_seq_cst);
}
/* Any path edit other than the ORBIT button turns the live orbit off. */
static void ui_triplet(Plugin*p,int id,double v);
static void publish_path(Plugin*p,const Point*pts,uint32_t count){
 Page*pg=selected(p);if(ui_value(p,POSITION_MODE)>=.5)ui_triplet(p,POSITION_MODE,0);
 atomic_store(&pg->orbit,0);store_path(pg,pts,count);if(p->host_state)p->host_state->mark_dirty(p->host);
}
/* XY lanes remain independent; constrain only the rendered point to the map circle. */
static Point xy_point(double x,double y){double r=hypot(x,y);if(r>1){x/=r;y/=r;}return (Point){(float)x,(float)y};}
/* Starts in front of the listener; counter-clockwise (seen from above) mirrors the angle. */
static void orbit_points(double radius,bool counter_clockwise,Point*pts){
 for(uint32_t i=0;i<ORBIT_POINTS;i++){double a=2*PI*i/(ORBIT_POINTS-1);if(counter_clockwise)a=-a;pts[i]=(Point){(float)(radius*sin(a)),(float)(-radius*cos(a))};}
}
/* The live path: a circle of the current ORBIT SIZE while the orbit is on, else the stored path. */
static uint32_t current_path(Plugin*p,Point*pts){
 Page*pg=selected(p);int page=ui_page(p);
 if(ui_value(p,POSITION_MODE)>=.5){pts[0]=xy_point(ui_value(p,SOURCE_X),ui_value(p,SOURCE_Y));return 1;}
 if(atomic_load(&pg->orbit)){orbit_points(page_value(p,page,ORBIT_SIZE),page_value(p,page,ORBIT_DIR)>=.5,pts);return ORBIT_POINTS;}
 uint32_t n=atomic_load(&pg->count);if(n>POINTS)n=POINTS;
 for(uint32_t i=0;i<n;i++)pts[i]=unpack_point(atomic_load(&pg->path[i]));
 return n;
}
/* Bounded seqlock snapshot: if a UI edit overlaps, use the last complete path. */
static void snapshot_path(Page*pg){
 uint32_t before=atomic_load_explicit(&pg->sequence,memory_order_seq_cst);
 if(before&1)return;
 Point temp[POINTS];uint32_t n=atomic_load_explicit(&pg->count,memory_order_seq_cst);
 if(!n||n>POINTS)return;
 for(uint32_t i=0;i<n;i++)temp[i]=unpack_point(atomic_load_explicit(&pg->path[i],memory_order_seq_cst));
 if(before==atomic_load_explicit(&pg->sequence,memory_order_seq_cst)){
  memcpy(pg->cached,temp,n*sizeof(Point));pg->cached_count=n;
 }
}
static Point position(const Point*pts,uint32_t n,double phase){
 if(!n)return (Point){0,-.5f}; if(n==1)return pts[0];
 double f=clampd(phase,0,1)*(n-1);uint32_t i=(uint32_t)f;
 if(i>=n-1)return pts[n-1];double t=f-i;
 return(Point){(float)(pts[i].x+(pts[i+1].x-pts[i].x)*t),(float)(pts[i].y+(pts[i+1].y-pts[i].y)*t)};
}
static double path_phase(Plugin*p,int page,double phase){return !atomic_load(&p->page[page].orbit)&&page_value(p,page,ORBIT_DIR)>=.5?1-phase:phase;}
static double wrap_phase(double phase){return phase-floor(phase);}
static void save_undo(Plugin*p){
 Page*pg=selected(p);pg->undo_count=atomic_load(&pg->count);for(uint32_t i=0;i<pg->undo_count;i++)pg->undo[i]=unpack_point(atomic_load(&pg->path[i]));
 pg->undo_orbit=atomic_load(&pg->orbit)!=0;pg->undo_mode=ui_value(p,POSITION_MODE);pg->undo_x=ui_value(p,SOURCE_X);pg->undo_y=ui_value(p,SOURCE_Y);
}
static void request_flush(Plugin*p){
 if(p->host_params)p->host_params->request_flush(p->host);
 /* A live edit must wake a suspended effect, including an otherwise silent mixer track. */
 if(p->host&&p->host->request_process)p->host->request_process(p->host);
}
static void ui_queue(Plugin*p,int type,int id,double v){
 id=ui_param(p,id);
 uint32_t w=atomic_load_explicit(&p->qw,memory_order_relaxed),r=atomic_load_explicit(&p->qr,memory_order_acquire);
 if(w-r<QUEUE){p->queue[w%QUEUE]=(UiEvent){(uint16_t)type,(uint16_t)id,atomic_load(&p->generation),v};atomic_store_explicit(&p->qw,w+1,memory_order_release);}
 request_flush(p);
}
static void ui_set(Plugin*p,int id,double v){set_value(p,ui_param(p,id),v);ui_queue(p,CLAP_EVENT_PARAM_VALUE,id,ui_value(p,id));}
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
  atomic_fetch_add_explicit(&p->automation_received,1,memory_order_relaxed);
  atomic_store_explicit(&p->automation_last_id,v->param_id,memory_order_relaxed);
  atomic_store_explicit(&p->automation_last_value,float_bits((float)v->value),memory_order_relaxed);
  uint32_t selectors=(v->note_id!=-1?1u:0u)|(v->port_index!=-1?2u:0u)|(v->channel!=-1?4u:0u)|(v->key!=-1?8u:0u);
  atomic_store_explicit(&p->automation_selectors,selectors,memory_order_relaxed);
  if(v->param_id<NPARAM&&!selectors&&isfinite(v->value)){
   set_value(p,(int)v->param_id,v->value);atomic_fetch_add_explicit(&p->automation_applied,1,memory_order_relaxed);
  }
 }
}
static void preset(Plugin*p,int which,bool inform){
 Page*pg=selected(p);int page=ui_page(p);
 Point pts[POINTS];uint32_t n=128;
 if(which==0){n=ORBIT_POINTS;orbit_points(page_value(p,page,ORBIT_SIZE),page_value(p,page,ORBIT_DIR)>=.5,pts);}
 else if(which==1){for(uint32_t i=0;i<n;i++){double t=(double)i/(n-1);double r=.94*(1-t)+.045*t;double a=PI*(1-t)+PI*.5*t;pts[i]=(Point){(float)(r*sin(a)),(float)(-r*cos(a))};}}
 else{n=1;pts[0]=(Point){-.10f,-.025f};}
 publish_path(p,pts,n);
 if(which==0)atomic_store(&pg->orbit,1);
 double reach=which==1?100:which==2?2:30;
 double room=which==1?.94:which==2?.06:.55;
 double amb=which==2?.10:.38;
 if(inform){ui_triplet(p,REACH,reach);ui_triplet(p,ROOM,room);ui_triplet(p,AMBIENCE,amb);ui_triplet(p,MOTION,which==2?0:1);ui_triplet(p,PHASE,0);}
 else{set_value(p,page_param(page,REACH),reach);set_value(p,page_param(page,ROOM),room);set_value(p,page_param(page,AMBIENCE),amb);set_value(p,page_param(page,MOTION),which==2?0:1);}
}

static bool alloc_delay(Delay*d,uint32_t size){d->data=calloc(size,sizeof(float));d->size=size;d->pos=0;d->lp=0;return d->data!=NULL;}
static void release_delay(Delay*d){free(d->data);memset(d,0,sizeof(*d));}
static double read_delay(Delay*d,double samples){
 samples=clampd(samples,1,d->size-2);uint32_t whole=(uint32_t)samples;double frac=samples-whole;
 uint32_t a=(d->pos+d->size-whole)%d->size,b=(a+d->size-1)%d->size;
 return d->data[a]*(1-frac)+d->data[b]*frac;
}
/* Audio-side copy of the path: while the orbit is on, rebuild the circle from ORBIT SIZE (no shared writes). */
static void apply_orbit(Plugin*p,int page){
 Page*pg=&p->page[page];if(!atomic_load(&pg->orbit))return;
 orbit_points(page_value(p,page,ORBIT_SIZE),page_value(p,page,ORBIT_DIR)>=.5,pg->cached);pg->cached_count=ORBIT_POINTS;
}
static void clear_dsp(Plugin*p){
 for(int page=0;page<2;page++){
  Page*pg=&p->page[page];if(pg->direct.data)memset(pg->direct.data,0,pg->direct.size*sizeof(float));pg->direct.pos=0;
  for(int j=0;j<8;j++){if(pg->reverb[j].data)memset(pg->reverb[j].data,0,pg->reverb[j].size*sizeof(float));pg->reverb[j].pos=0;pg->reverb[j].lp=0;}
  memset(pg->voice,0,sizeof(pg->voice));pg->double_mix=page==0&&value(p,TWO_PAGES)<.5?value(p,DOUBLE_MODE):0;
  for(int j=0;j<PAGE_PARAMS;j++)pg->smooth[j]=page_value(p,page,j);
  snapshot_path(pg);apply_orbit(p,page);
  for(int j=0;j<2;j++){
   double phase=page==0&&j?value(p,POINT_B_PHASE):page_value(p,page,PHASE);
   Point pt=page_value(p,page,POSITION_MODE)>=.5?xy_point(page_value(p,page,SOURCE_X),page_value(p,page,SOURCE_Y)):position(pg->cached,pg->cached_count,path_phase(p,page,phase));
   pg->voice[j].x=pt.x;pg->voice[j].y=pt.y;
  }
  pg->tick=0;pg->phase_seen=0;pg->phase_b_seen=.5;uni_tempo_reset(&pg->tempo_clock);
 }
 p->free_beats=0;p->dual_mix=value(p,TWO_PAGES);
}
static void voice_coefficients(Plugin*p,Page*pg,Voice*v){
 double r=clampd(hypot(v->x,v->y),0,1),reach=pg->smooth[REACH];
 double d=.15*pow(reach/.15,r);
 double angle=r>1e-6?atan2(v->x,-v->y):0,pan=sin(angle),rear=(1-cos(angle))*.5;
 double sx=d*sin(angle),sz=d*cos(angle),h=.0875;
 double ears[2]={hypot(sx+h,sz),hypot(sx-h,sz)};
 double att=pow(.3/fmax(.3,d),.58);
 for(int c=0;c<2;c++){
  double side=c==0?-pan:pan,ear_gain=clampd(d/fmax(.09,ears[c]),.6,1.4);
  v->gain[c]=att*sqrt(fmax(.04,(1+.80*side)*.5))*ear_gain;
  v->itd[c]=1+clampd((ears[c]-fmin(ears[0],ears[1]))/343*p->sr,0,.001*p->sr);
  double head=clampd(-side,0,1),air=18000/(1+.045*d);
  double cutoff=fmin(air,18000/(1+4.8*head+1.2*rear));
  v->cut[c]=1-exp(-2*PI*clampd(cutoff,500,.45*p->sr)/p->sr);
 }
 /* Approximate elevation/rear spectral cue; not a measured HRTF. */
 double freq=clampd(7500+pg->smooth[HEIGHT_P]*25-1200*rear,4000,.40*p->sr);
 double omega=2*PI*freq/p->sr,alpha=sin(omega)/(2*2.2),a0=1+alpha;
 v->notch_b0=1/a0;v->notch_b1=-2*cos(omega)/a0;v->notch_b2=v->notch_b0;
 v->notch_a1=v->notch_b1;v->notch_a2=(1-alpha)/a0;
 v->room_send=pg->smooth[AMBIENCE]*(.18+.65*(1-att))*pow(.3/fmax(.3,d),.12);
}
static void coefficients(Plugin*p,Page*pg){
 for(int j=0;j<2;j++)voice_coefficients(p,pg,&pg->voice[j]);
 double room=pg->smooth[ROOM],rt60=.22+room*7.78;
 const double base[8]={.0297,.0371,.0433,.0539,.0617,.0719,.0833,.0971};
 for(int j=0;j<8;j++){
  pg->delay_samples[j]=base[j]*(.48+room*2.25)*p->sr;
  pg->feedback[j]=pow(.001,pg->delay_samples[j]/p->sr/rt60);
 }
 pg->damp_coeff=1-exp(-2*PI*(2200+3600*(1-room))/p->sr);
 pg->out_gain=pow(10,pg->smooth[OUTPUT]/20);
 double d=.15*pow(pg->smooth[REACH]/.15,clampd(hypot(pg->voice[0].x,pg->voice[0].y),0,1));
 atomic_store_explicit(&pg->display_distance,float_bits((float)d),memory_order_relaxed);
}
static void sample_dsp(Plugin*p,Page*pg,double l,double r,double*outl,double*outr){
 double mono=.5*(l+r);pg->direct.data[pg->direct.pos]=(float)mono;
 double wet[2]={0,0},weight[2]={1-.5*pg->double_mix,.5*pg->double_mix};
 for(int j=0;j<2;j++)if(weight[j]>0)for(int c=0;c<2;c++){
  Voice*v=&pg->voice[j];double sample=read_delay(&pg->direct,v->itd[c]);v->low[c]+=v->cut[c]*(sample-v->low[c]);
  sample=v->low[c];double notch=v->notch_b0*sample+v->nz1[c];
  v->nz1[c]=v->notch_b1*sample-v->notch_a1*notch+v->nz2[c];v->nz2[c]=v->notch_b2*sample-v->notch_a2*notch;
  double amount=.12+.28*fabs(pg->smooth[HEIGHT_P])/90+.16*(v->y>0?1:0);
  wet[c]+=weight[j]*(sample+(notch-sample)*amount)*v->gain[c];
 }
 double rv[8],matrix[8];
 for(int j=0;j<8;j++){
  double v=read_delay(&pg->reverb[j],pg->delay_samples[j]);pg->reverb[j].lp+=pg->damp_coeff*(v-pg->reverb[j].lp);
  rv[j]=matrix[j]=pg->reverb[j].lp;
 }
 for(int step=1;step<8;step*=2)for(int group=0;group<8;group+=2*step)for(int j=0;j<step;j++){
  double a=matrix[group+j],b=matrix[group+j+step];matrix[group+j]=a+b;matrix[group+j+step]=a-b;
 }
 double feed=read_delay(&pg->direct,(.008+.07*pg->smooth[ROOM])*p->sr)*.27;
 for(int j=0;j<8;j++){
  Delay*d=&pg->reverb[j];double v=feed*(j&1?-1:1)+matrix[j]*.3535533905932738*pg->feedback[j];
  if(fabs(v)<1e-25)v=0;d->data[d->pos]=(float)v;d->pos=(d->pos+1)%d->size;
 }
 double room_send=weight[0]*pg->voice[0].room_send+weight[1]*pg->voice[1].room_send;
 wet[0]+=room_send*(rv[0]+rv[2]-rv[4]+rv[6])*.50;
 wet[1]+=room_send*(rv[1]-rv[3]+rv[5]+rv[7])*.50;
 pg->direct.pos=(pg->direct.pos+1)%pg->direct.size;
 double mix=pg->smooth[MIX];
 *outl=(l*(1-mix)+wet[0]*mix)*pg->out_gain;*outr=(r*(1-mix)+wet[1]*mix)*pg->out_gain;
 /* Transparent below 0.98; prevent accidental overload when close to an ear. */
 if(fabs(*outl)>.98)*outl=copysign(.98+.02*tanh((fabs(*outl)-.98)/.02),*outl);
 if(fabs(*outr)>.98)*outr=copysign(.98+.02*tanh((fabs(*outr)-.98)/.02),*outr);
 if(!isfinite(*outl))*outl=0;if(!isfinite(*outr))*outr=0;
}

static bool CLAP_ABI plugin_init(const clap_plugin_t*clap){
 Plugin*p=clap->plugin_data;
 if(p->host->get_extension){p->host_params=p->host->get_extension(p->host,CLAP_EXT_PARAMS);p->host_state=p->host->get_extension(p->host,CLAP_EXT_STATE);p->host_gui=p->host->get_extension(p->host,CLAP_EXT_GUI);}
 if(p->host->get_extension){p->host_menu=p->host->get_extension(p->host,CLAP_EXT_CONTEXT_MENU);if(!p->host_menu)p->host_menu=p->host->get_extension(p->host,CLAP_EXT_CONTEXT_MENU_COMPAT);}
 return true;
}
static void gui_free(Plugin*p);
static void CLAP_ABI deactivate(const clap_plugin_t*clap){Plugin*p=clap->plugin_data;for(int page=0;page<2;page++){Page*pg=&p->page[page];release_delay(&pg->direct);for(int j=0;j<8;j++)release_delay(&pg->reverb[j]);}p->active=false;p->processing=false;}
static void CLAP_ABI destroy(const clap_plugin_t*clap){Plugin*p=clap->plugin_data;gui_free(p);deactivate(clap);free(p);}
static bool CLAP_ABI activate(const clap_plugin_t*clap,double sr,uint32_t min,uint32_t max){
 (void)min;(void)max;Plugin*p=clap->plugin_data;if(!isfinite(sr)||sr<1000||sr>768000||p->active)return false;
 p->sr=sr;for(int page=0;page<2;page++){Page*pg=&p->page[page];
  if(!alloc_delay(&pg->direct,(uint32_t)(sr*.16)+8)){deactivate(clap);return false;}
  for(int j=0;j<8;j++)if(!alloc_delay(&pg->reverb[j],(uint32_t)(sr*.32)+8)){deactivate(clap);return false;}
 }
 p->active=true;clear_dsp(p);for(int page=0;page<2;page++)coefficients(p,&p->page[page]);return true;
}
static bool CLAP_ABI start(const clap_plugin_t*c){Plugin*p=c->plugin_data;p->processing=p->active;return p->processing;}
static void CLAP_ABI stop(const clap_plugin_t*c){((Plugin*)c->plugin_data)->processing=false;}
static void CLAP_ABI reset(const clap_plugin_t*c){Plugin*p=c->plugin_data;clear_dsp(p);if(p->active)for(int page=0;page<2;page++)coefficients(p,&p->page[page]);}
static double read_audio(const clap_audio_buffer_t*b,uint32_t c,uint32_t i){
 if(c>=b->channel_count)c=0;double v=b->data64?b->data64[c][i]:b->data32?b->data32[c][i]:0;return isfinite(v)?v:0;
}
static void step_page(Plugin*p,int page,const double*targets,double beat,bool has_timeline,bool playing,double tempo,double barbeats,double smooth_coeff,double room_coeff,double l,double r,double*outl,double*outr){
 Page*pg=&p->page[page];
 for(int k=0;k<PAGE_PARAMS;k++){
  int id=page_param(page,k);double a=k==ROOM?room_coeff:smooth_coeff;
  if(definitions[id].stepped)pg->smooth[k]=targets[id];else pg->smooth[k]+=a*(targets[id]-pg->smooth[k]);
 }
 bool dual=targets[TWO_PAGES]>=.5,doubled=page==0&&!dual&&targets[DOUBLE_MODE]>=.5;
 double duration=fmax(1,targets[page_param(page,BARS)])*barbeats;
 double clock_phase=uni_tempo_step(&pg->tempo_clock,beat,has_timeline,playing,tempo,pg->smooth[CUSTOM_BPM],targets[page_param(page,TEMPO_SOURCE)]>=.5,pg->smooth[SPEED],duration,p->sr);
 double phase=targets[page_param(page,MOTION)]>=.5?(dual||doubled?wrap_phase(clock_phase+pg->smooth[PHASE]):clock_phase):pg->smooth[PHASE];
 double phase_b=targets[MOTION]>=.5?wrap_phase(clock_phase+targets[POINT_B_PHASE]):targets[POINT_B_PHASE];
 for(int j=0;j<(doubled||pg->double_mix>0?2:1);j++){
  double active_phase=j?phase_b:phase;
  Point pt=targets[page_param(page,POSITION_MODE)]>=.5?xy_point(pg->smooth[SOURCE_X],pg->smooth[SOURCE_Y]):position(pg->cached,pg->cached_count,path_phase(p,page,active_phase));
  Voice*v=&pg->voice[j];v->x+=smooth_coeff*(pt.x-v->x);v->y+=smooth_coeff*(pt.y-v->y);
 }
 double target_double=doubled?1.0:0.0;pg->double_mix+=smooth_coeff*(target_double-pg->double_mix);
 if(fabs(pg->double_mix-target_double)<1e-6)pg->double_mix=target_double;
 if((pg->tick++&31)==0)coefficients(p,pg);
 sample_dsp(p,pg,l,r,outl,outr);
 if(targets[page_param(page,BYPASS)]>=.5){*outl=l;*outr=r;}
 pg->phase_seen=phase;pg->phase_b_seen=phase_b;
}
static clap_process_status CLAP_ABI process(const clap_plugin_t*c,const clap_process_t*pr){
 Plugin*p=c->plugin_data;if(!p->active||!pr||pr->audio_outputs_count!=1||pr->audio_outputs[0].channel_count<2)return CLAP_PROCESS_ERROR;
 clap_audio_buffer_t*out=&pr->audio_outputs[0];if(!out->data32&&!out->data64)return CLAP_PROCESS_ERROR;
 const clap_audio_buffer_t*in=pr->audio_inputs_count?&pr->audio_inputs[0]:NULL;
 send_ui(p,pr->out_events);for(int page=0;page<2;page++){snapshot_path(&p->page[page]);apply_orbit(p,page);}
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
   double old_orbit[2]={targets[ORBIT_SIZE],targets[B_ORBIT_SIZE]},old_dir[2]={targets[ORBIT_DIR],targets[B_ORBIT_DIR]};
   for(int k=0;k<NPARAM;k++)targets[k]=value(p,k);
   for(int page=0;page<2;page++)if(targets[page_param(page,ORBIT_SIZE)]!=old_orbit[page]||targets[page_param(page,ORBIT_DIR)]!=old_dir[page])apply_orbit(p,page);
  }
  double l=0,r=0,a=0,b=0,al=0,ar=0,bl=0,br=0;if(in&&in->channel_count&&(in->data32||in->data64)){l=read_audio(in,0,i);r=read_audio(in,1,i);}
  bool has_timeline=tr&&(tr->flags&CLAP_TRANSPORT_HAS_BEATS_TIMELINE);
  step_page(p,0,targets,beat,has_timeline,playing,tempo,barbeats,smooth_coeff,room_coeff,l,r,&al,&ar);
  double dual_target=targets[TWO_PAGES]>=.5?1.0:0.0;p->dual_mix+=smooth_coeff*(dual_target-p->dual_mix);
  if(fabs(p->dual_mix-dual_target)<1e-6)p->dual_mix=dual_target;
  if(p->dual_mix>0)step_page(p,1,targets,beat,has_timeline,playing,tempo,barbeats,smooth_coeff,room_coeff,l,r,&bl,&br);
  a=(1-.5*p->dual_mix)*al+.5*p->dual_mix*bl;b=(1-.5*p->dual_mix)*ar+.5*p->dual_mix*br;
  if(out->data64){out->data64[0][i]=a;out->data64[1][i]=b;}else{out->data32[0][i]=(float)a;out->data32[1][i]=(float)b;}
  if(playing)beat+=tempo/(60*p->sr);tempo=clampd(tempo+tempo_inc,1,999);
 }
 _mm_setcsr(oldcsr);p->free_beats=beat;out->constant_mask=0;
 for(int page=0;page<2;page++){Page*pg=&p->page[page];atomic_store_explicit(&pg->display_phase,float_bits((float)pg->phase_seen),memory_order_relaxed);atomic_store_explicit(&pg->display_phase_b,float_bits((float)pg->phase_b_seen),memory_order_relaxed);}
 atomic_store_explicit(&p->display_bpm,float_bits((float)tempo),memory_order_relaxed);
 atomic_store_explicit(&p->display_tempo_valid,tr&&(tr->flags&CLAP_TRANSPORT_HAS_TEMPO)&&isfinite(tr->tempo)&&tr->tempo>0,memory_order_relaxed);
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
 (void)c;if(index>=NPARAM)return false;memset(info,0,sizeof(*info));info->id=index;info->flags=CLAP_PARAM_IS_AUTOMATABLE|CLAP_PARAM_REQUIRES_PROCESS;
 if(definitions[index].stepped)info->flags|=CLAP_PARAM_IS_STEPPED;if(index==BYPASS)info->flags|=CLAP_PARAM_IS_BYPASS;
 snprintf(info->name,sizeof(info->name),"%s",definitions[index].name);snprintf(info->module,sizeof(info->module),"UNI");
 info->min_value=definitions[index].min;info->max_value=definitions[index].max;info->default_value=definitions[index].def;return true;
}
static bool CLAP_ABI params_value(const clap_plugin_t*c,clap_id id,double*out){if(id>=NPARAM)return false;*out=value(c->plugin_data,id);return true;}
static bool CLAP_ABI params_text(const clap_plugin_t*c,clap_id id,double v,char*b,uint32_t n){
 (void)c;if(id>=NPARAM||!b||!n)return false;
 if(id>=B_BASE)id-=B_BASE;
 if(id==REACH)snprintf(b,n,"%.1f m",v);else if(id==BARS)snprintf(b,n,"%.0f bars",v);else if(id==HEIGHT_P)snprintf(b,n,"%+.0f deg",v);
 else if(id==OUTPUT)snprintf(b,n,"%+.1f dB",v);else if(id==MOTION)snprintf(b,n,"%s",v>=.5?"Follow":"Manual");else if(id==ORBIT_DIR)snprintf(b,n,"%s",v>=.5?"Reverse / CCW":"Forward / CW");
 else if(id==TEMPO_SOURCE)snprintf(b,n,"%s",v>=.5?"Custom BPM":"Host Sync");
 else if(id==CUSTOM_BPM)snprintf(b,n,"%.1f BPM",v);
 else if(id==SPEED){if(fabs(v-.0625)<.00001)snprintf(b,n,"1/16x");else if(fabs(v-.125)<.00001)snprintf(b,n,"1/8x");else if(fabs(v-.25)<.00001)snprintf(b,n,"1/4x");else if(fabs(v-.5)<.00001)snprintf(b,n,"1/2x");else snprintf(b,n,"%.2fx",v);}
 else if(id==POSITION_MODE)snprintf(b,n,"%s",v>=.5?"XY":"Path");
 else if(id==DOUBLE_MODE)snprintf(b,n,"%s",v>=.5?"Double":"Single");
 else if(id==TWO_PAGES)snprintf(b,n,"%s",v>=.5?"Two pages":"One page");
 else if(id==SOURCE_X||id==SOURCE_Y)snprintf(b,n,"%+.0f %%",v*100);
 else if(id==BYPASS)snprintf(b,n,"%s",v>=.5?"Bypass":"Active");else snprintf(b,n,"%.0f %%",v*100);return true;
}
static bool CLAP_ABI params_parse(const clap_plugin_t*c,clap_id id,const char*b,double*v){
 (void)c;if(id>=NPARAM||!b||!v)return false;clap_id raw_id=id;if(id>=B_BASE)id-=B_BASE;char*end;double d=strtod(b,&end);
 if(end==b){if(id==MOTION){if(!_stricmp(b,"Follow"))d=1;else if(!_stricmp(b,"Manual"))d=0;else return false;}
 else if(id==BYPASS){if(!_stricmp(b,"Bypass"))d=1;else if(!_stricmp(b,"Active"))d=0;else return false;}
 else if(id==ORBIT_DIR){if(!_stricmp(b,"Counter-clockwise")||!_stricmp(b,"Reverse")||!_stricmp(b,"Reverse / CCW"))d=1;else if(!_stricmp(b,"Clockwise")||!_stricmp(b,"Forward")||!_stricmp(b,"Forward / CW"))d=0;else return false;}
 else if(id==TEMPO_SOURCE){if(!_stricmp(b,"Host Sync"))d=0;else if(!_stricmp(b,"Custom BPM"))d=1;else return false;}
 else if(id==POSITION_MODE){if(!_stricmp(b,"XY"))d=1;else if(!_stricmp(b,"Path"))d=0;else return false;}
 else if(id==DOUBLE_MODE){if(!_stricmp(b,"Double")||!_stricmp(b,"On"))d=1;else if(!_stricmp(b,"Single")||!_stricmp(b,"Off"))d=0;else return false;}
 else if(id==TWO_PAGES){if(!_stricmp(b,"Two pages")||!_stricmp(b,"On"))d=1;else if(!_stricmp(b,"One page")||!_stricmp(b,"Off"))d=0;else return false;}else return false;}
 else if(strchr(end,'%')&&(id==ROOM||id==AMBIENCE||id==PHASE||id==POINT_B_PHASE||id==MIX||id==ORBIT_SIZE||id==SOURCE_X||id==SOURCE_Y))d/=100;
 else if(id==SPEED&&*end=='/'){char*den_end;double denominator=strtod(end+1,&den_end);if(den_end==end+1||!isfinite(denominator)||denominator<=0)return false;d/=denominator;}
 if(!isfinite(d))return false;*v=clampd(d,definitions[raw_id].min,definitions[raw_id].max);return true;
}
static void CLAP_ABI params_flush(const clap_plugin_t*c,const clap_input_events_t*in,const clap_output_events_t*out){
 Plugin*p=c->plugin_data;send_ui(p,out);if(in)for(uint32_t i=0;i<in->size(in);i++)receive_event(p,in->get(in,i));
}
static const clap_plugin_params_t params={params_count,params_info,params_value,params_text,params_parse,params_flush};

typedef struct {uint32_t magic,version,count,reserved;float params[NPARAM];uint32_t pts[POINTS];uint32_t count_b,orbit_b,pts_b[POINTS];} State;
static bool write_all(const clap_ostream_t*s,const void*data,uint64_t n){const char*p=data;while(n){int64_t k=s->write(s,p,n);if(k<=0||(uint64_t)k>n)return false;p+=k;n-=k;}return true;}
static bool read_all(const clap_istream_t*s,void*data,uint64_t n){char*p=data;while(n){int64_t k=s->read(s,p,n);if(k<=0||(uint64_t)k>n)return false;p+=k;n-=k;}return true;}
static bool CLAP_ABI state_save(const clap_plugin_t*c,const clap_ostream_t*s){
 Plugin*p=c->plugin_data;State st={0};st.magic=0x4f4e4953;st.version=7;
 for(int k=0;k<NPARAM;k++)st.params[k]=(float)value(p,k);
 for(int page=0;page<2;page++){
  Page*pg=&p->page[page];Point live[POINTS];uint32_t orbit=atomic_load(&pg->orbit)?1:0;
  uint32_t count=orbit?ORBIT_POINTS:atomic_load(&pg->count);
  if(!count||count>POINTS)return false;
  if(orbit)orbit_points(page_value(p,page,ORBIT_SIZE),page_value(p,page,ORBIT_DIR)>=.5,live);
  else for(uint32_t i=0;i<count;i++)live[i]=unpack_point(atomic_load(&pg->path[i]));
  if(page){st.count_b=count;st.orbit_b=orbit;for(uint32_t i=0;i<count;i++)st.pts_b[i]=pack_point(live[i]);}
  else{st.count=count;st.reserved=orbit;for(uint32_t i=0;i<count;i++)st.pts[i]=pack_point(live[i]);}
 }
 return write_all(s,&st,sizeof(st));
}
static bool CLAP_ABI state_load(const clap_plugin_t*c,const clap_istream_t*s){
 Plugin*p=c->plugin_data;State st={0};
 if(!read_all(s,&st,16)||st.magic!=0x4f4e4953||(st.version<3||st.version>7)||st.reserved>1||!st.count||st.count>POINTS)return false;
 uint32_t nparams=st.version==3?12:st.version==4?15:st.version==5?18:st.version==6?20:NPARAM;
 for(int k=0;k<NPARAM;k++)st.params[k]=(float)definitions[k].def;
 if(!read_all(s,st.params,nparams*sizeof(float))||!read_all(s,st.pts,sizeof(st.pts)))return false;
 if(st.version==7){if(!read_all(s,&st.count_b,sizeof(st.count_b)+sizeof(st.orbit_b)+sizeof(st.pts_b)))return false;}
 else{Point live[POINTS];st.count_b=ORBIT_POINTS;st.orbit_b=1;orbit_points(st.params[B_ORBIT_SIZE],st.params[B_ORBIT_DIR]>=.5,live);for(uint32_t i=0;i<ORBIT_POINTS;i++)st.pts_b[i]=pack_point(live[i]);}
 if(st.orbit_b>1||!st.count_b||st.count_b>POINTS)return false;
 for(int k=0;k<NPARAM;k++)if(!isfinite(st.params[k])||st.params[k]<definitions[k].min||st.params[k]>definitions[k].max)return false;
 Point pts[POINTS],pts_b[POINTS];for(uint32_t i=0;i<st.count;i++)pts[i]=unpack_point(st.pts[i]);for(uint32_t i=0;i<st.count_b;i++)pts_b[i]=unpack_point(st.pts_b[i]);
 /* Validate the complete state before replacing anything. */
 for(int k=0;k<NPARAM;k++)set_value(p,k,st.params[k]);
 atomic_fetch_add(&p->generation,1);
 store_path(&p->page[0],pts,st.count);atomic_store(&p->page[0].orbit,st.reserved);p->page[0].undo_count=0;
 store_path(&p->page[1],pts_b,st.count_b);atomic_store(&p->page[1].orbit,st.orbit_b);p->page[1].undo_count=0;
 if(p->host_params)p->host_params->rescan(p->host,CLAP_PARAM_RESCAN_VALUES);
 if(p->window)InvalidateRect(p->window,NULL,FALSE);return true;
}
static const clap_plugin_state_t state={state_save,state_load};
static uint32_t CLAP_ABI tail_get(const clap_plugin_t*c){Plugin*p=c->plugin_data;return(uint32_t)(p->sr*9);}
static const clap_plugin_tail_t tail={tail_get};
static uint32_t CLAP_ABI latency_get(const clap_plugin_t*c){(void)c;return 0;}
static const clap_plugin_latency_t latency={latency_get};

/* Native, embedded Win32 GUI. All coordinates below are logical pixels. */
static COLORREF bg=RGB(12,18,25),panel=RGB(19,29,38),line=RGB(42,62,72),text=RGB(227,237,239),muted=RGB(133,156,165),mint=RGB(87,222,186),orange=RGB(255,175,91),blue=RGB(117,178,255);
static void rect_fill(HDC dc,int x,int y,int w,int h,COLORREF color){RECT r={x,y,x+w,y+h};HBRUSH b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);}
static void draw_line(HDC dc,int x,int y,int xx,int yy,COLORREF col,int width){HPEN pen=CreatePen(PS_SOLID,width,col);HGDIOBJ old=SelectObject(dc,pen);MoveToEx(dc,x,y,NULL);LineTo(dc,xx,yy);SelectObject(dc,old);DeleteObject(pen);}
static void ellipse(HDC dc,int x,int y,int w,int h,COLORREF stroke,COLORREF fill,int width){HPEN pen=CreatePen(PS_SOLID,width,stroke);HBRUSH brush=CreateSolidBrush(fill);HGDIOBJ a=SelectObject(dc,pen),b=SelectObject(dc,brush);Ellipse(dc,x,y,x+w,y+h);SelectObject(dc,a);SelectObject(dc,b);DeleteObject(pen);DeleteObject(brush);}
static void label(HDC dc,HFONT font,int x,int y,const char*s,COLORREF color){HGDIOBJ old=SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);TextOutA(dc,x,y,s,(int)strlen(s));SelectObject(dc,old);}
static void button(HDC dc,Plugin*p,int x,int y,int w,const char*s,bool selected){rect_fill(dc,x,y,w,32,selected?mint:panel);label(dc,p->small,x+12,y+7,s,selected?bg:text);}
static double normalized(Plugin*p,int id){double v=ui_value(p,id);if(id==REACH)return log(v)/log(100);if(id==SPEED)return (log2(v)+4)/8;return (v-definitions[id].min)/(definitions[id].max-definitions[id].min);}
static int slider_y(int slot){return 135+slot*43;}
static void draw_gui(Plugin*p,HDC dc){
 rect_fill(dc,0,0,WIDTH,HEIGHT,bg);
 label(dc,p->title,28,22,"UNI",text);label(dc,p->small,29,60,"DRAW THE SPACE. HEAR THE DISTANCE.",muted);
 bool dual=ui_value(p,TWO_PAGES)>=.5,doubled=!dual&&ui_value(p,DOUBLE_MODE)>=.5;
 button(dc,p,295,27,105,"SINGLE",!dual&&!doubled);
 button(dc,p,408,27,140,"DOUBLE ORBIT",doubled);
 button(dc,p,556,27,150,"TWO PAGES",dual);
 if(dual)label(dc,p->small,461,65,ui_page(p)?"EDITING PAGE B":"EDITING PAGE A",blue);
 label(dc,p->small,751,29,"HEADPHONE SPATIAL EFFECT",mint);label(dc,p->small,751,53,"WINDOWS 64 / VST3 / 0.8.0",muted);
 button(dc,p,28,92,85,"DRAW",!selected(p)->place_mode);button(dc,p,121,92,85,"PLACE",selected(p)->place_mode);
 button(dc,p,224,92,65,"UNDO",false);button(dc,p,305,92,86,"ORBIT",false);button(dc,p,399,92,130,"FAR > EAR",false);button(dc,p,537,92,91,"WHISPER",false);button(dc,p,636,92,70,"CLEAR",false);
 rect_fill(dc,28,139,678,475,panel);
 const int cx=367,cy=377,rad=206;
 for(int ring=5;ring>=1;ring--){int r=rad*ring/5;ellipse(dc,cx-r,cy-r,2*r,2*r,line,panel,1);}
 draw_line(dc,cx-rad,cy,cx+rad,cy,line,1);draw_line(dc,cx,cy-rad,cx,cy+rad,line,1);
 label(dc,p->small,cx-21,149,"FRONT",muted);label(dc,p->small,cx-18,591,"BACK",muted);
 label(dc,p->small,49,cy-8,"LEFT",muted);label(dc,p->small,650,cy-8,"RIGHT",muted);
 char s[80];for(int ring=1;ring<=5;ring++){
  double d=.15*pow(ui_value(p,REACH)/.15,ring/5.0);if(d>=10)snprintf(s,sizeof(s),"%.0f m",d);else if(d>=1)snprintf(s,sizeof(s),"%.1f m",d);else snprintf(s,sizeof(s),"%.2f m",d);
  label(dc,p->small,cx+5,cy-rad*ring/5+5,s,muted);
 }
 Point pts[POINTS];uint32_t n;
 if(selected(p)->drawing){memcpy(pts,selected(p)->draft,selected(p)->draft_count*sizeof(Point));n=selected(p)->draft_count;}
 else n=current_path(p,pts);
 for(uint32_t i=1;i<n;i++)draw_line(dc,cx+(int)(pts[i-1].x*rad),cy+(int)(pts[i-1].y*rad),cx+(int)(pts[i].x*rad),cy+(int)(pts[i].y*rad),mint,3);
 if(n){uint32_t begin=!atomic_load(&selected(p)->orbit)&&ui_value(p,ORBIT_DIR)>=.5?n-1:0;ellipse(dc,cx+(int)(pts[begin].x*rad)-5,cy+(int)(pts[begin].y*rad)-5,10,10,mint,panel,2);label(dc,p->small,cx+(int)(pts[begin].x*rad)+8,cy+(int)(pts[begin].y*rad)-8,"START",mint);}
 ellipse(dc,cx-15,cy-18,30,37,muted,bg,2);ellipse(dc,cx-21,cy-5,7,14,muted,bg,1);ellipse(dc,cx+14,cy-5,7,14,muted,bg,1);
 draw_line(dc,cx-6,cy-19,cx,cy-27,mint,2);draw_line(dc,cx,cy-27,cx+6,cy-19,mint,2);label(dc,p->small,cx-12,cy+24,"YOU",text);
 double phase=bits_float(atomic_load(&selected(p)->display_phase)),phase_b=bits_float(atomic_load(&selected(p)->display_phase_b));
 if(!p->active){phase=ui_value(p,PHASE);phase_b=ui_value(p,POINT_B_PHASE);}
 Point cur=position(pts,n,path_phase(p,ui_page(p),phase));int xx=cx+(int)(cur.x*rad),yy=cy+(int)(cur.y*rad);
 ellipse(dc,xx-11,yy-11,22,22,orange,panel,2);ellipse(dc,xx-5,yy-5,10,10,orange,orange,1);
 if(doubled){Point second=position(pts,n,path_phase(p,ui_page(p),phase_b));int bx=cx+(int)(second.x*rad),by=cy+(int)(second.y*rad);
  ellipse(dc,bx-11,by-11,22,22,blue,panel,2);ellipse(dc,bx-5,by-5,10,10,blue,blue,1);
  label(dc,p->small,bx+13,by-11,"B",blue);label(dc,p->small,xx+13,yy-11,"A",orange);
 }
 double r=clampd(hypot(cur.x,cur.y),0,1),d=.15*pow(ui_value(p,REACH)/.15,r);
 snprintf(s,sizeof(s),"%.2f m",d);label(dc,p->font,49,164,s,orange);label(dc,p->small,49,188,doubled?"POINT A DISTANCE":"SOURCE DISTANCE",muted);
 snprintf(s,sizeof(s),"%u path points",n);label(dc,p->small,567,586,s,muted);
 rect_fill(dc,732,92,280,522,panel);
 const char* names[NSLIDERS]={"REACH","ORBIT SIZE","ROOM SIZE","AMBIENCE","CYCLE LENGTH",doubled?(ui_value(p,MOTION)>=.5?"POINT A OFFSET":"POINT A POSITION"):dual&&ui_value(p,MOTION)>=.5?"PATH OFFSET":"PATH POSITION",ui_value(p,MOTION)>=.5?"POINT B OFFSET":"POINT B POSITION","HEIGHT","MIX","OUTPUT"};
 for(int slot=0;slot<NSLIDERS;slot++){
  if(dual&&slider_ids[slot]==POINT_B_PHASE)continue;
  int k=slider_ids[slot],y=slider_y(slot);label(dc,p->small,751,y-25,names[slot],k==POINT_B_PHASE&&!doubled?line:muted);params_text(&p->clap,k,ui_value(p,k),s,sizeof(s));label(dc,p->small,912,y-25,s,text);
  rect_fill(dc,751,y,240,4,line);double t=normalized(p,k);rect_fill(dc,751,y,(int)(240*t),4,k==PHASE?orange:k==POINT_B_PHASE?blue:mint);
  ellipse(dc,751+(int)(240*t)-5,y-3,10,10,text,text,1);
 }
 if(dual)label(dc,p->small,751,376,"A and B have separate settings",blue);
 label(dc,p->small,751,556,"DIRECTION: DRAW + ORBIT",muted);
 button(dc,p,751,578,118,"FORWARD / CW",ui_value(p,ORBIT_DIR)<.5);button(dc,p,877,578,114,"REVERSE / CCW",ui_value(p,ORBIT_DIR)>=.5);
 button(dc,p,28,631,154,"FOLLOW TRANSPORT",ui_value(p,MOTION)>.5);button(dc,p,190,631,131,"MANUAL / SCRUB",ui_value(p,MOTION)<.5);
 button(dc,p,888,631,124,ui_value(p,BYPASS)>.5?"BYPASSED":"ACTIVE",ui_value(p,BYPASS)<.5);
 if(dual){button(dc,p,340,631,110,"PAGE A",ui_page(p)==0);button(dc,p,458,631,110,"PAGE B",ui_page(p)==1);
  rect_fill(dc,590,642,268,5,line);rect_fill(dc,590,642,(int)(268*clampd(phase,0,1)),5,orange);}
 else{rect_fill(dc,340,642,518,5,line);rect_fill(dc,340,642,(int)(518*clampd(phase,0,1)),5,orange);}
 label(dc,p->small,29,685,selected(p)->drawing?"Release to commit the path. Playback uses equal distance along your drawing.":selected(p)->place_mode?"Drag in the map to record Source X / Source Y automation. Select PATH / ORBIT to return to your path.":dual?"TWO PAGES: each page has its own path, location, room, tempo and output. Both play together.":doubled?"DOUBLE ORBIT: A and B move on one path, each with an automatable position.":"Draw a path. In Manual, automate Path position to control its timing. PLACE records X / Y movement.",muted);
 rect_fill(dc,28,722,984,82,panel);
 label(dc,p->small,48,733,"AUTOMATION POSITION MODE",muted);
 button(dc,p,48,759,145,"PATH / ORBIT",ui_value(p,POSITION_MODE)<.5);
 button(dc,p,205,759,70,"XY",ui_value(p,POSITION_MODE)>=.5);
 const int xyids[2]={SOURCE_X,SOURCE_Y},starts[2]={320,678};
 for(int j=0;j<2;j++){
  int k=xyids[j],sx=starts[j];double t=normalized(p,k);
  label(dc,p->small,sx,737,j?"SOURCE Y / FRONT - BACK":"SOURCE X / LEFT - RIGHT",muted);
  params_text(&p->clap,k,ui_value(p,k),s,sizeof(s));label(dc,p->small,sx+238,737,s,text);
  rect_fill(dc,sx,775,280,4,line);rect_fill(dc,sx,775,(int)(280*t),4,mint);
  draw_line(dc,sx+140,770,sx+140,784,muted,1);ellipse(dc,sx+(int)(280*t)-5,772,10,10,text,text,1);
 }
 rect_fill(dc,28,814,984,82,panel);
 label(dc,p->small,48,823,"TEMPO / SYNC",muted);
 button(dc,p,48,847,125,"HOST SYNC",ui_value(p,TEMPO_SOURCE)<.5);
 button(dc,p,181,847,137,"CUSTOM BPM",ui_value(p,TEMPO_SOURCE)>=.5);
 label(dc,p->small,350,823,"CUSTOM BPM",muted);
 rect_fill(dc,350,849,100,30,bg);snprintf(s,sizeof(s),"%.1f",ui_value(p,CUSTOM_BPM));label(dc,p->font,359,851,s,text);
 bool custom=ui_value(p,TEMPO_SOURCE)>=.5,valid=atomic_load_explicit(&p->display_tempo_valid,memory_order_relaxed)!=0;
 double bpm=custom?ui_value(p,CUSTOM_BPM):bits_float(atomic_load_explicit(&p->display_bpm,memory_order_relaxed));
 if(custom)snprintf(s,sizeof(s),"%.1f BPM",bpm);else if(valid)snprintf(s,sizeof(s),"HOST %.1f",bpm);else snprintf(s,sizeof(s),"HOST: waiting");
 label(dc,p->small,471,851,s,custom||valid?mint:muted);
 label(dc,p->small,620,823,"SPEED: DRAW + ORBIT",muted);params_text(&p->clap,SPEED,ui_value(p,SPEED),s,sizeof(s));label(dc,p->small,850,823,s,text);
 double speed_t=normalized(p,SPEED);rect_fill(dc,620,860,260,4,line);rect_fill(dc,620,860,(int)(260*speed_t),4,mint);
 draw_line(dc,750,855,750,869,muted,1);ellipse(dc,620+(int)(260*speed_t)-5,857,10,10,text,text,1);
 button(dc,p,888,847,44,"-",false);button(dc,p,940,847,44,"+",false);
 uint32_t incoming=atomic_load_explicit(&p->automation_received,memory_order_relaxed),applied=atomic_load_explicit(&p->automation_applied,memory_order_relaxed),last=atomic_load_explicit(&p->automation_last_id,memory_order_relaxed);
 if(!incoming)snprintf(s,sizeof(s),"HOST AUTOMATION: waiting for parameter changes");
 else snprintf(s,sizeof(s),"AUTOMATION IN: %u  APPLIED: %u  LAST: %s",incoming,applied,last<NPARAM?definitions[last].name:"unknown parameter");
 label(dc,p->small,29,908,s,incoming&&applied<incoming?orange:muted);
}
static void refresh(Plugin*p){if(p->window)InvalidateRect(p->window,NULL,FALSE);}
static Point mouse_point(int x,int y){double a=(x-367)/206.0,b=(y-377)/206.0,r=hypot(a,b);if(r>1){a/=r;b/=r;}return (Point){(float)a,(float)b};}
static bool in_map(int x,int y){return x>=28&&x<=706&&y>=139&&y<=614;}
static void append_point(Plugin*p,Point pt){
 if(selected(p)->draft_count&&hypot(pt.x-selected(p)->draft[selected(p)->draft_count-1].x,pt.y-selected(p)->draft[selected(p)->draft_count-1].y)<.008)return;
 if(selected(p)->draft_count>=POINTS){for(uint32_t i=0;i<POINTS/2;i++)selected(p)->draft[i]=selected(p)->draft[i*2];selected(p)->draft_count=POINTS/2;}
 selected(p)->draft[selected(p)->draft_count++]=pt;
}
static void commit_draw(Plugin*p){
 if(!selected(p)->draft_count)return;
 if(selected(p)->draft_count<2){publish_path(p,selected(p)->draft,selected(p)->draft_count);return;}
 double lengths[POINTS];lengths[0]=0;for(uint32_t i=1;i<selected(p)->draft_count;i++)lengths[i]=lengths[i-1]+hypot(selected(p)->draft[i].x-selected(p)->draft[i-1].x,selected(p)->draft[i].y-selected(p)->draft[i-1].y);
 Point pts[POINTS];uint32_t n=128,index=1;double total=lengths[selected(p)->draft_count-1];
 if(total<1e-8){publish_path(p,selected(p)->draft,1);return;}
 for(uint32_t j=0;j<n;j++){
  double target=total*j/(n-1);while(index<selected(p)->draft_count-1&&lengths[index]<target)index++;
  double span=lengths[index]-lengths[index-1],t=span>0?(target-lengths[index-1])/span:0;
  pts[j]=(Point){(float)(selected(p)->draft[index-1].x+t*(selected(p)->draft[index].x-selected(p)->draft[index-1].x)),(float)(selected(p)->draft[index-1].y+t*(selected(p)->draft[index].y-selected(p)->draft[index-1].y))};
 }
 publish_path(p,pts,n);
}
static void slider_update(Plugin*p,int id,int x){
 double start=id==SPEED?620:id==SOURCE_X?320:id==SOURCE_Y?678:751,width=id==SPEED?260:(id==SOURCE_X||id==SOURCE_Y)?280:240;
 double t=clampd((x-start)/width,0,1);double v=id==REACH?pow(100,t):id==SPEED?pow(2,-4+8*t):definitions[id].min+t*(definitions[id].max-definitions[id].min);ui_set(p,id,v);refresh(p);
}
static void follow_path(Plugin*p){if(ui_value(p,POSITION_MODE)>=.5)ui_triplet(p,POSITION_MODE,0);if(ui_value(p,MOTION)<.5)ui_triplet(p,MOTION,1);}
static void tempo_select(Plugin*p,double custom){follow_path(p);ui_triplet(p,TEMPO_SOURCE,custom);refresh(p);}
#define BPM_EDIT_ID 2001
static void bpm_refresh_text(Plugin*p){if(p->bpm_edit&&GetFocus()!=p->bpm_edit){char b[32];snprintf(b,sizeof(b),"%.1f",ui_value(p,CUSTOM_BPM));SetWindowTextA(p->bpm_edit,b);}}
static void bpm_commit(Plugin*p){
 if(!p->bpm_edit)return;char b[32];GetWindowTextA(p->bpm_edit,b,sizeof(b));char*end;double v=strtod(b,&end);while(*end==' '||*end=='\t')end++;
 if(end!=b&&!*end&&isfinite(v)){tempo_select(p,1);ui_triplet(p,CUSTOM_BPM,clampd(v,20,999));}
 bpm_refresh_text(p);refresh(p);
}
static LRESULT CALLBACK bpm_edit_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 Plugin*p=(Plugin*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);if(!p)return DefWindowProcW(hwnd,msg,wp,lp);
 if(msg==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
 if(msg==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_ESCAPE)){
  if(wp==VK_RETURN)bpm_commit(p);
  p->bpm_cancel=true;char b[32];snprintf(b,sizeof(b),"%.1f",ui_value(p,CUSTOM_BPM));SetWindowTextA(hwnd,b);
  SetFocus(p->window);return 0;
 }
 if(msg==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE))return 0;
 return CallWindowProcW(p->bpm_proc,hwnd,msg,wp,lp);
}
static void bpm_layout(Plugin*p,int width,int height){
 if(!p->bpm_edit)return;
 SetWindowPos(p->bpm_edit,NULL,350*width/WIDTH,849*height/HEIGHT,100*width/WIDTH,30*height/HEIGHT,SWP_NOZORDER|SWP_NOACTIVATE);
 HFONT f=CreateFontW(-(int)fmax(10,18.0*height/HEIGHT),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 if(f){SendMessageW(p->bpm_edit,WM_SETFONT,(WPARAM)f,TRUE);if(p->bpm_font)DeleteObject(p->bpm_font);p->bpm_font=f;}
}
static void xy_update(Plugin*p,Point q){ui_set(p,SOURCE_X,q.x);ui_set(p,SOURCE_Y,q.y);refresh(p);}
static void release_mouse(Plugin*p){
 if(selected(p)->drawing){commit_draw(p);selected(p)->drawing=false;ui_triplet(p,MOTION,1);}
 if(selected(p)->xy_drag){ui_queue(p,CLAP_EVENT_PARAM_GESTURE_END,SOURCE_X,0);ui_queue(p,CLAP_EVENT_PARAM_GESTURE_END,SOURCE_Y,0);selected(p)->xy_drag=false;}
 if(p->drag_param>=0){ui_queue(p,CLAP_EVENT_PARAM_GESTURE_END,p->drag_param,0);p->drag_param=-1;}
 refresh(p);
}
/* Ask the DAW for its own automation/controller menu, when it provides one. */
static int parameter_at(Plugin*p,int x,int y){
 if(y>=27&&y<=59){if(x>=408&&x<=548)return DOUBLE_MODE;if(x>=556&&x<=706)return TWO_PAGES;}
 if(y>=814&&y<=891){if(x>=48&&x<=318)return TEMPO_SOURCE;if(x>=350&&x<=450)return CUSTOM_BPM;if(x>=610&&x<=991)return SPEED;}
 if(x>=741&&x<=1003)for(int i=0;i<NSLIDERS;i++)if(!(ui_value(p,TWO_PAGES)>=.5&&slider_ids[i]==POINT_B_PHASE)&&y>=slider_y(i)-28&&y<=slider_y(i)+14)return slider_ids[i];
 if(y>=733&&y<=791){if(x>=310&&x<=610)return SOURCE_X;if(x>=668&&x<=978)return SOURCE_Y;if(x>=48&&x<=275)return POSITION_MODE;}
 if(y>=578&&y<=610&&x>=751&&x<=991)return ORBIT_DIR;
 if(y>=631&&y<=663){if(x>=28&&x<=321)return MOTION;if(x>=888&&x<=1012)return BYPASS;}
 return -1;
}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 Plugin*p=(Plugin*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
 if(msg==WM_NCCREATE){p=((CREATESTRUCTW*)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)p);}
 if(!p)return DefWindowProcW(hwnd,msg,wp,lp);
 RECT cr;GetClientRect(hwnd,&cr);int width=cr.right?cr.right:WIDTH,height=cr.bottom?cr.bottom:HEIGHT;
 int x=(int)((double)GET_X_LPARAM(lp)*WIDTH/width),y=(int)((double)GET_Y_LPARAM(lp)*HEIGHT/height);
 switch(msg){
 case WM_CTLCOLOREDIT:
  if((HWND)lp==p->bpm_edit){SetTextColor((HDC)wp,text);SetBkColor((HDC)wp,bg);return (LRESULT)p->bpm_brush;}break;
 case WM_ERASEBKGND:return 1;
 case WM_PRINTCLIENT:{int saved=SaveDC((HDC)wp);SetMapMode((HDC)wp,MM_ANISOTROPIC);SetWindowExtEx((HDC)wp,WIDTH,HEIGHT,NULL);SetViewportExtEx((HDC)wp,width,height,NULL);draw_gui(p,(HDC)wp);RestoreDC((HDC)wp,saved);return 0;}
 case WM_PAINT:{
  PAINTSTRUCT ps;HDC dst=BeginPaint(hwnd,&ps),dc=CreateCompatibleDC(dst);HBITMAP bmp=CreateCompatibleBitmap(dst,width,height);HGDIOBJ old=SelectObject(dc,bmp);
  SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,WIDTH,HEIGHT,NULL);SetViewportExtEx(dc,width,height,NULL);draw_gui(p,dc);
  SetMapMode(dc,MM_TEXT);BitBlt(dst,0,0,width,height,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);EndPaint(hwnd,&ps);return 0;}
 case WM_SIZE:bpm_layout(p,width,height);return 0;
 case WM_COMMAND:
  if(LOWORD(wp)==BPM_EDIT_ID&&HIWORD(wp)==EN_KILLFOCUS){if(p->bpm_cancel){p->bpm_cancel=false;bpm_refresh_text(p);}else bpm_commit(p);return 0;}break;
 case WM_TIMER:bpm_refresh_text(p);refresh(p);return 0;
 case WM_RBUTTONUP:{
  int k=parameter_at(p,x,y);const clap_host_context_menu_t*m=p->host_menu;
  if(k>=0&&m&&m->can_popup&&m->popup&&m->can_popup(p->host)){
   clap_context_menu_target_t target={CLAP_CONTEXT_MENU_TARGET_KIND_PARAM,(clap_id)ui_param(p,k)};POINT pos={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
   if(p->floating)ClientToScreen(hwnd,&pos);m->popup(p->host,&target,-1,pos.x,pos.y);return 0;
  }break;
 }
 case WM_LBUTTONDOWN:
  if(GetFocus()==p->bpm_edit)SetFocus(hwnd);
  if(y>=27&&y<=59){
   if(x>=295&&x<=400){ui_triplet(p,TWO_PAGES,0);ui_triplet(p,DOUBLE_MODE,0);p->selected_page=0;}
   else if(x>=408&&x<=548){ui_triplet(p,TWO_PAGES,0);p->selected_page=0;ui_triplet(p,POSITION_MODE,0);ui_triplet(p,DOUBLE_MODE,1);}
   else if(x>=556&&x<=706){ui_triplet(p,DOUBLE_MODE,0);ui_triplet(p,TWO_PAGES,1);p->selected_page=0;}
   bpm_refresh_text(p);refresh(p);return 0;
  }
  if(y>=847&&y<=879){
   if(x>=48&&x<=173){tempo_select(p,0);return 0;}
   if(x>=181&&x<=318){tempo_select(p,1);return 0;}
   if(x>=888&&x<=984){follow_path(p);ui_triplet(p,SPEED,ui_value(p,SPEED)*(x<936?.5:2));refresh(p);return 0;}
  }
  if(x>=610&&x<=880&&y>=846&&y<=874){follow_path(p);p->drag_param=SPEED;ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,SPEED,0);slider_update(p,SPEED,x);SetCapture(hwnd);return 0;}
  if(y>=759&&y<=791){
   if(x>=48&&x<=193){ui_triplet(p,POSITION_MODE,0);refresh(p);return 0;}
   if(x>=205&&x<=275){ui_triplet(p,DOUBLE_MODE,0);ui_triplet(p,POSITION_MODE,1);refresh(p);return 0;}
  }
  if(y>=761&&y<=789&&((x>=310&&x<=610)||(x>=668&&x<=968))){
   int k=x<640?SOURCE_X:SOURCE_Y;ui_triplet(p,DOUBLE_MODE,0);ui_triplet(p,POSITION_MODE,1);p->drag_param=k;
   ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,k,0);slider_update(p,k,x);SetCapture(hwnd);return 0;
  }
  if(y>=92&&y<=124){
   if(x>=28&&x<=113)selected(p)->place_mode=false;else if(x>=121&&x<=206)selected(p)->place_mode=true;
   else if(x>=224&&x<=289&&selected(p)->undo_count){
    Point old[POINTS];uint32_t n=atomic_load(&selected(p)->count);for(uint32_t i=0;i<n;i++)old[i]=unpack_point(atomic_load(&selected(p)->path[i]));
    bool orb=atomic_load(&selected(p)->orbit)!=0;double mode=ui_value(p,POSITION_MODE),sx=ui_value(p,SOURCE_X),sy=ui_value(p,SOURCE_Y);
    publish_path(p,selected(p)->undo,selected(p)->undo_count);atomic_store(&selected(p)->orbit,selected(p)->undo_orbit);
    ui_triplet(p,POSITION_MODE,selected(p)->undo_mode);ui_triplet(p,SOURCE_X,selected(p)->undo_x);ui_triplet(p,SOURCE_Y,selected(p)->undo_y);
    memcpy(selected(p)->undo,old,n*sizeof(Point));selected(p)->undo_count=n;selected(p)->undo_orbit=orb;selected(p)->undo_mode=mode;selected(p)->undo_x=sx;selected(p)->undo_y=sy;
   }
   else if(x>=305&&x<=391){save_undo(p);preset(p,0,true);}else if(x>=399&&x<=529){save_undo(p);preset(p,1,true);}
   else if(x>=537&&x<=628){save_undo(p);preset(p,2,true);}else if(x>=636&&x<=706){save_undo(p);Point q={0,-.3f};publish_path(p,&q,1);ui_triplet(p,MOTION,0);}
   refresh(p);return 0;
  }
  if(y>=631&&y<=663){if(x>=28&&x<=182){follow_path(p);}else if(x>=190&&x<=321){if(ui_value(p,POSITION_MODE)>=.5)ui_triplet(p,POSITION_MODE,0);ui_triplet(p,MOTION,0);}else if(ui_value(p,TWO_PAGES)>=.5&&x>=340&&x<=568){p->selected_page=x>=458?1:0;bpm_refresh_text(p);}else if(x>=888&&x<=1012)ui_triplet(p,BYPASS,ui_value(p,BYPASS)<.5?1:0);refresh(p);return 0;}
  if(y>=578&&y<=610&&x>=751&&x<=991){if(x<=869)ui_triplet(p,ORBIT_DIR,0);else if(x>=877)ui_triplet(p,ORBIT_DIR,1);refresh(p);return 0;}
  if(x>=741&&x<=1003)for(int slot=0;slot<NSLIDERS;slot++)if(!(ui_value(p,TWO_PAGES)>=.5&&slider_ids[slot]==POINT_B_PHASE)&&abs(y-slider_y(slot))<=14){
   int k=slider_ids[slot];if(k==PHASE&&ui_value(p,DOUBLE_MODE)<.5&&ui_value(p,TWO_PAGES)<.5){ui_triplet(p,POSITION_MODE,0);ui_triplet(p,MOTION,0);}p->drag_param=k;ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,k,0);slider_update(p,k,x);SetCapture(hwnd);return 0;
  }
  if(in_map(x,y)){
   save_undo(p);SetCapture(hwnd);Point q=mouse_point(x,y);
   if(selected(p)->place_mode){ui_triplet(p,DOUBLE_MODE,0);ui_triplet(p,POSITION_MODE,1);selected(p)->xy_drag=true;ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,SOURCE_X,0);ui_queue(p,CLAP_EVENT_PARAM_GESTURE_BEGIN,SOURCE_Y,0);xy_update(p,q);}
   else{selected(p)->drawing=true;selected(p)->draft_count=0;append_point(p,q);}refresh(p);return 0;
  }break;
 case WM_MOUSEMOVE:
  if(GetCapture()==hwnd&&(wp&MK_LBUTTON)){
   if(p->drag_param>=0)slider_update(p,p->drag_param,x);
   else if(selected(p)->drawing){append_point(p,mouse_point(x,y));refresh(p);}
   else if(selected(p)->xy_drag)xy_update(p,mouse_point(x,y));
  }return 0;
 case WM_LBUTTONUP:if(GetCapture()==hwnd){release_mouse(p);ReleaseCapture();}return 0;
 case WM_CAPTURECHANGED:release_mouse(p);return 0;
 case WM_CLOSE:release_mouse(p);if(GetCapture()==hwnd)ReleaseCapture();ShowWindow(hwnd,SW_HIDE);if(p->host_gui)p->host_gui->closed(p->host,false);return 0;
 case WM_DESTROY:KillTimer(hwnd,1);return 0;
 }return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool register_window(void){
 WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.hInstance=module_instance;wc.lpfnWndProc=window_proc;wc.lpszClassName=L"UNI.CLAP.0.2";wc.hCursor=LoadCursor(NULL,IDC_ARROW);
 return RegisterClassExW(&wc)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
}
static void gui_free(Plugin*p){if(p->window){if(GetFocus()==p->bpm_edit)SetFocus(p->window);release_mouse(p);if(GetCapture()==p->window)ReleaseCapture();DestroyWindow(p->window);p->window=NULL;p->bpm_edit=NULL;}if(p->font)DeleteObject(p->font);if(p->small)DeleteObject(p->small);if(p->title)DeleteObject(p->title);if(p->bpm_font)DeleteObject(p->bpm_font);if(p->bpm_brush)DeleteObject(p->bpm_brush);p->bpm_brush=NULL;p->font=p->small=p->title=p->bpm_font=NULL;}
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
 p->window=CreateWindowExW(0,L"UNI.CLAP.0.2",L"UNI",style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,module_instance,p);
 if(!p->window){gui_free(p);return false;}
 p->bpm_brush=CreateSolidBrush(bg);p->bpm_edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"120.0",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_CENTER|ES_AUTOHSCROLL,350,849,100,30,p->window,(HMENU)(INT_PTR)BPM_EDIT_ID,module_instance,NULL);
 if(!p->bpm_edit){gui_free(p);return false;}SetWindowLongPtrW(p->bpm_edit,GWLP_USERDATA,(LONG_PTR)p);p->bpm_proc=(WNDPROC)SetWindowLongPtrW(p->bpm_edit,GWLP_WNDPROC,(LONG_PTR)bpm_edit_proc);SendMessageW(p->bpm_edit,EM_SETLIMITTEXT,12,0);bpm_layout(p,WIDTH,HEIGHT);bpm_refresh_text(p);SetTimer(p->window,1,33,NULL);return true;
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
static bool CLAP_ABI gui_set_size(const clap_plugin_t*c,uint32_t w,uint32_t h){Plugin*p=c->plugin_data;if(!p->window||w<WIDTH/2||h<HEIGHT/2||w>WIDTH*3||h>HEIGHT*3)return false;return SetWindowPos(p->window,NULL,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER)!=0;}
static bool CLAP_ABI gui_parent(const clap_plugin_t*c,const clap_window_t*parent){
 Plugin*p=c->plugin_data;if(!p->window||p->floating||!parent||strcmp(parent->api,CLAP_WINDOW_API_WIN32)||!parent->win32)return false;
 SetWindowLongPtrW(p->window,GWL_STYLE,WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN);SetLastError(0);SetParent(p->window,parent->win32);return GetLastError()==0;
}
static bool CLAP_ABI gui_transient(const clap_plugin_t*c,const clap_window_t*parent){Plugin*p=c->plugin_data;if(!p->window||!p->floating||!parent||strcmp(parent->api,CLAP_WINDOW_API_WIN32))return false;SetWindowLongPtrW(p->window,GWLP_HWNDPARENT,(LONG_PTR)parent->win32);return true;}
static void CLAP_ABI gui_title(const clap_plugin_t*c,const char*title){(void)c;(void)title;}
static bool CLAP_ABI gui_show(const clap_plugin_t*c){Plugin*p=c->plugin_data;if(!p->window)return false;ShowWindow(p->window,SW_SHOWNOACTIVATE);return true;}
static bool CLAP_ABI gui_hide(const clap_plugin_t*c){Plugin*p=c->plugin_data;if(!p->window)return false;release_mouse(p);if(GetCapture()==p->window)ReleaseCapture();ShowWindow(p->window,SW_HIDE);return true;}
static const clap_plugin_gui_t gui={gui_supported,gui_preferred,gui_create,gui_destroy,gui_scale,gui_size,gui_can_resize,gui_hints,gui_adjust,gui_set_size,gui_parent,gui_transient,gui_title,gui_show,gui_hide};
static const void*CLAP_ABI extension(const clap_plugin_t*c,const char*id){
 (void)c;if(!strcmp(id,CLAP_EXT_AUDIO_PORTS))return &ports;if(!strcmp(id,CLAP_EXT_PARAMS))return &params;if(!strcmp(id,CLAP_EXT_STATE))return &state;
 if(!strcmp(id,CLAP_EXT_GUI))return &gui;if(!strcmp(id,CLAP_EXT_TAIL))return &tail;if(!strcmp(id,CLAP_EXT_LATENCY))return &latency;return NULL;
}
static void CLAP_ABI main_thread(const clap_plugin_t*c){refresh(c->plugin_data);}
static const char*const features[]={CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,CLAP_PLUGIN_FEATURE_MULTI_EFFECTS,CLAP_PLUGIN_FEATURE_REVERB,CLAP_PLUGIN_FEATURE_STEREO,NULL};
static const clap_plugin_descriptor_t descriptor={CLAP_VERSION_INIT,"local.spatialink.orbit","UNI","UNI","","","","0.8.0","Two independent spatial pages with separate paths and controls, plus the optional Double Orbit mode.",features};
static uint32_t CLAP_ABI factory_count(const clap_plugin_factory_t*f){(void)f;return 1;}
static const clap_plugin_descriptor_t*CLAP_ABI factory_desc(const clap_plugin_factory_t*f,uint32_t index){(void)f;return index?NULL:&descriptor;}
static const clap_plugin_t*CLAP_ABI factory_create(const clap_plugin_factory_t*f,const clap_host_t*host,const char*id){
 (void)f;if(!host||!clap_version_is_compatible(host->clap_version)||strcmp(id,descriptor.id))return NULL;
 Plugin*p=calloc(1,sizeof(*p));if(!p)return NULL;p->host=host;p->scale=1;p->drag_param=-1;
 p->clap=(clap_plugin_t){&descriptor,p,plugin_init,destroy,activate,deactivate,start,stop,reset,process,extension,main_thread};
 for(int k=0;k<NPARAM;k++)atomic_init(&p->param[k],float_bits((float)definitions[k].def));
 for(int page=0;page<2;page++){Page*pg=&p->page[page];
  for(int k=0;k<PAGE_PARAMS;k++)pg->smooth[k]=page_value(p,page,k);
  for(int k=0;k<POINTS;k++)atomic_init(&pg->path[k],0);
  atomic_init(&pg->count,1);atomic_init(&pg->sequence,0);atomic_init(&pg->orbit,0);
  atomic_init(&pg->display_phase,float_bits(page?.5f:0));atomic_init(&pg->display_phase_b,float_bits(.5f));atomic_init(&pg->display_distance,0);
 }
 atomic_init(&p->qr,0);atomic_init(&p->qw,0);atomic_init(&p->generation,0);
 atomic_init(&p->automation_received,0);atomic_init(&p->automation_applied,0);atomic_init(&p->automation_last_id,CLAP_INVALID_ID);atomic_init(&p->automation_last_value,0);atomic_init(&p->automation_selectors,0);
 atomic_init(&p->display_bpm,float_bits(120));atomic_init(&p->display_tempo_valid,0);
 preset(p,0,false);snapshot_path(&p->page[0]);
 Point orbit[ORBIT_POINTS];orbit_points(page_value(p,1,ORBIT_SIZE),false,orbit);store_path(&p->page[1],orbit,ORBIT_POINTS);atomic_store(&p->page[1].orbit,1);snapshot_path(&p->page[1]);
 return &p->clap;
}
static const clap_plugin_factory_t factory={factory_count,factory_desc,factory_create};
static bool CLAP_ABI entry_init(const char*path){(void)path;if(!module_instance)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)entry_init,&module_instance);InterlockedIncrement(&entry_refs);return true;}
static void CLAP_ABI entry_deinit(void){if(InterlockedDecrement(&entry_refs)==0)UnregisterClassW(L"UNI.CLAP.0.2",module_instance);}
static const void*CLAP_ABI entry_factory(const char*id){return !strcmp(id,CLAP_PLUGIN_FACTORY_ID)?&factory:NULL;}
CLAP_EXPORT const clap_plugin_entry_t clap_entry={CLAP_VERSION_INIT,entry_init,entry_deinit,entry_factory};
#ifndef SPATIALINK_VST3
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){(void)reserved;if(reason==DLL_PROCESS_ATTACH){module_instance=instance;DisableThreadLibraryCalls(instance);}return TRUE;}

#endif
