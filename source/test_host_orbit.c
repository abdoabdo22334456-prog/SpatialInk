#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <clap/clap.h>

#define CHECK(c,msg) do {if(!(c)){fprintf(stderr,"FAIL: %s (line %d)\n",msg,__LINE__);exit(1);}}while(0)
#define N 512
#define POINTS_IN_STATE 256
static int dirty_count,flush_count,out_count;
static void CLAP_ABI rescan(const clap_host_t*h,clap_param_rescan_flags f){}
static void CLAP_ABI clear(const clap_host_t*h,clap_id i,clap_param_clear_flags f){}
static void CLAP_ABI request_flush(const clap_host_t*h){flush_count++;}
static void CLAP_ABI dirty(const clap_host_t*h){dirty_count++;}
static bool CLAP_ABI resize(const clap_host_t*h,uint32_t w,uint32_t z){return true;}
static const clap_host_params_t hp={rescan,clear,request_flush};
static const clap_host_state_t hs={dirty};
static const clap_host_gui_t hg={.request_resize=resize};
static const void*CLAP_ABI host_ext(const clap_host_t*h,const char*id){if(!strcmp(id,CLAP_EXT_PARAMS))return &hp;if(!strcmp(id,CLAP_EXT_STATE))return &hs;if(!strcmp(id,CLAP_EXT_GUI))return &hg;return NULL;}
static void CLAP_ABI host_noop(const clap_host_t*h){}
static const clap_host_t host={CLAP_VERSION_INIT,NULL,"Spatial Ink test host","Local","","1",host_ext,host_noop,host_noop,host_noop};
typedef struct {const clap_event_header_t*events[32];uint32_t count;} EventList;
static uint32_t CLAP_ABI event_size(const clap_input_events_t*l){return ((EventList*)l->ctx)->count;}
static const clap_event_header_t*CLAP_ABI event_get(const clap_input_events_t*l,uint32_t i){return ((EventList*)l->ctx)->events[i];}
static bool CLAP_ABI event_push(const clap_output_events_t*l,const clap_event_header_t*e){out_count++;return true;}
static const clap_output_events_t out_events={NULL,event_push};
static void setp(const clap_plugin_t*p,clap_id id,double v){
 const clap_plugin_params_t*pa=p->get_extension(p,CLAP_EXT_PARAMS);
 clap_event_param_value_t e={0};e.header=(clap_event_header_t){sizeof(e),0,0,CLAP_EVENT_PARAM_VALUE,0};e.param_id=id;e.note_id=e.port_index=e.channel=e.key=-1;e.value=v;
 EventList list={{&e.header},1};clap_input_events_t in={&list,event_size,event_get};pa->flush(p,&in,&out_events);
}
typedef struct {uint8_t data[16384];size_t pos,size;size_t chunk;} Memory;
static int64_t CLAP_ABI memory_write(const clap_ostream_t*s,const void*buf,uint64_t size){Memory*m=s->ctx;if(size>m->chunk)size=m->chunk;if(m->pos+size>sizeof(m->data))return -1;memcpy(m->data+m->pos,buf,size);m->pos+=size;m->size=m->pos;return size;}
static int64_t CLAP_ABI memory_read(const clap_istream_t*s,void*buf,uint64_t size){Memory*m=s->ctx;if(size>m->chunk)size=m->chunk;if(size>m->size-m->pos)size=m->size-m->pos;memcpy(buf,m->data+m->pos,size);m->pos+=size;return size;}
static Memory save(const clap_plugin_t*p){Memory m={.chunk=17};clap_ostream_t os={&m,memory_write};const clap_plugin_state_t*s=p->get_extension(p,CLAP_EXT_STATE);CHECK(s->save(p,&os),"state save partial write");return m;}
static bool load(const clap_plugin_t*p,Memory*m){m->pos=0;m->chunk=13;clap_istream_t is={m,memory_read};const clap_plugin_state_t*s=p->get_extension(p,CLAP_EXT_STATE);return s->load(p,&is);}
static float left[N],right[N],ol[N],orr[N];
static clap_event_transport_t transport(double beat,double tempo,bool playing){clap_event_transport_t t={0};t.header=(clap_event_header_t){sizeof(t),0,0,CLAP_EVENT_TRANSPORT,0};t.flags=CLAP_TRANSPORT_HAS_TEMPO|CLAP_TRANSPORT_HAS_BEATS_TIMELINE|CLAP_TRANSPORT_HAS_TIME_SIGNATURE|(playing?CLAP_TRANSPORT_IS_PLAYING:0);t.tempo=tempo;t.song_pos_beats=(clap_beattime)llround(beat*CLAP_BEATTIME_FACTOR);t.tsig_num=4;t.tsig_denom=4;return t;}
static void block(const clap_plugin_t*p,int n,double beat,bool playing){
 float*ins[2]={left,right},*outs[2]={ol,orr};clap_audio_buffer_t in={.data32=ins,.channel_count=2},out={.data32=outs,.channel_count=2};
 clap_event_transport_t tr=transport(beat,120,playing);clap_process_t pr={.steady_time=-1,.frames_count=n,.transport=&tr,.audio_inputs=&in,.audio_outputs=&out,.audio_inputs_count=1,.audio_outputs_count=1,.out_events=&out_events};
 CHECK(p->process(p,&pr)!=CLAP_PROCESS_ERROR,"32-bit process");for(int i=0;i<n;i++)CHECK(isfinite(ol[i])&&isfinite(orr[i]),"finite outputs");
}
static void startup(const clap_plugin_t*p,double sr){CHECK(p->activate(p,sr,1,N),"activate");CHECK(p->start_processing(p),"start processing");}
static void shutdown(const clap_plugin_t*p){p->stop_processing(p);p->deactivate(p);}
static uint32_t noise_state=1731;
static double noise(void){noise_state=noise_state*1664525+1013904223;return ((double)(noise_state>>8)/16777216-.5)*.22;}
static void blank(void){memset(left,0,sizeof(left));memset(right,0,sizeof(right));}
static HWND found;
static BOOL CALLBACK enum_child(HWND window,LPARAM unused){wchar_t name[100];GetClassNameW(window,name,100);if(!wcscmp(name,L"SpatialInkOrbit.CLAP.0.2")){found=window;return FALSE;}return TRUE;}
static void click(HWND hwnd,int x,int y){SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));SendMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));}
static void place(HWND hwnd,int x,int y){click(hwnd,150,106);click(hwnd,x,y);}
static void print_gui(HWND hwnd,const char*path){
 HDC dc=CreateCompatibleDC(NULL);BITMAPINFO bi={0};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=1040;bi.bmiHeader.biHeight=-730;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;void*bits;
 HBITMAP bmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);CHECK(bmp,"snapshot bitmap");HGDIOBJ old=SelectObject(dc,bmp);SendMessageW(hwnd,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
 FILE*f=fopen(path,"wb");CHECK(f,"snapshot file");BITMAPFILEHEADER fh={0};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(bi.bmiHeader);fh.bfSize=fh.bfOffBits+1040*730*4;fwrite(&fh,sizeof(fh),1,f);fwrite(&bi.bmiHeader,sizeof(bi.bmiHeader),1,f);fwrite(bits,1040*730*4,1,f);fclose(f);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);
}
static COLORREF gui_pixel(HWND hwnd,int x,int y){
 HDC screen=GetDC(NULL),dc=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,1040,730);HGDIOBJ old=SelectObject(dc,bmp);
 SendMessageW(hwnd,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);COLORREF col=GetPixel(dc,x,y);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);ReleaseDC(NULL,screen);return col;
}
static void wave_header(FILE*f,uint32_t samples,uint32_t sr){uint32_t data=samples*4,riff=36+data,fmt=16;uint16_t pcm=1,channels=2,bits=16,align=4;uint32_t rate=sr*4;fwrite("RIFF",4,1,f);fwrite(&riff,4,1,f);fwrite("WAVEfmt ",8,1,f);fwrite(&fmt,4,1,f);fwrite(&pcm,2,1,f);fwrite(&channels,2,1,f);fwrite(&sr,4,1,f);fwrite(&rate,4,1,f);fwrite(&align,2,1,f);fwrite(&bits,2,1,f);fwrite("data",4,1,f);fwrite(&data,4,1,f);}

int main(int argc,char**argv){
 setvbuf(stdout,NULL,_IONBF,0);
 CHECK(argc>=2,"plugin path argument");HMODULE dll=LoadLibraryA(argv[1]);if(!dll){fprintf(stderr,"LoadLibrary error: %lu\n",GetLastError());return 1;}
 const clap_plugin_entry_t*entry=(const void*)GetProcAddress(dll,"clap_entry");CHECK(entry,"clap_entry exported");CHECK(entry->init(argv[1]),"entry init");CHECK(entry->init(argv[1]),"entry repeated init");
 const clap_plugin_factory_t*factory=entry->get_factory(CLAP_PLUGIN_FACTORY_ID);CHECK(factory&&factory->get_plugin_count(factory)==1,"factory count");
 const clap_plugin_descriptor_t*d=factory->get_plugin_descriptor(factory,0);CHECK(d&&clap_version_is_compatible(d->clap_version),"descriptor compatible");CHECK(!factory->get_plugin_descriptor(factory,1),"invalid descriptor index");
 const clap_plugin_t*p=factory->create_plugin(factory,&host,d->id);CHECK(p&&p->init(p),"create/init");
 const clap_plugin_params_t*pa=p->get_extension(p,CLAP_EXT_PARAMS);CHECK(pa&&pa->count(p)==12,"twelve host automation parameters");
 const clap_plugin_audio_ports_t*ap=p->get_extension(p,CLAP_EXT_AUDIO_PORTS);clap_audio_port_info_t port;CHECK(ap&&ap->get(p,0,true,&port)&&port.channel_count==2,"stereo input");CHECK(ap->get(p,0,false,&port)&&port.in_place_pair==0,"stereo in-place output");
 for(uint32_t i=0;i<10;i++){clap_param_info_t info;CHECK(pa->get_info(p,i,&info),"parameter info");double v;CHECK(pa->get_value(p,i,&v)&&v>=info.min_value&&v<=info.max_value,"parameter defaults");}
 CHECK(!p->activate(p,100,1,N),"unsupported sample rate rejected");
 const clap_plugin_gui_t*g=p->get_extension(p,CLAP_EXT_GUI);CHECK(g&&g->is_api_supported(p,CLAP_WINDOW_API_WIN32,false),"native embedded GUI API");CHECK(!g->is_api_supported(p,"cocoa",false),"unsupported GUI rejected");
 HWND parent=CreateWindowExW(0,L"STATIC",L"Test host",WS_POPUP,0,0,1040,730,NULL,NULL,GetModuleHandle(NULL),NULL);CHECK(parent,"hidden host window");
 CHECK(g->create(p,CLAP_WINDOW_API_WIN32,false),"create embedded GUI");clap_window_t pw={.api=CLAP_WINDOW_API_WIN32,.win32=parent};CHECK(g->set_parent(p,&pw),"embed GUI");found=NULL;EnumChildWindows(parent,enum_child,0);HWND ui=found;CHECK(ui,"find embedded child");
 uint32_t w,h;CHECK(g->get_size(p,&w,&h)&&w==1040&&h==730,"default UI size");CHECK(g->set_scale(p,1.25)&&g->get_size(p,&w,&h)&&w==1300,"DPI scaling");CHECK(g->set_scale(p,1),"DPI reset");
 click(ui,440,106);print_gui(ui,"work/spatial-ink/preview.bmp");
 printf("PASS scan, parameters, native GUI embedding and DPI scaling\n");

 /* Exact bypass, and exact dry signal for both sample formats. */
 const double rates[]={1000,8000,44100,48000,96000,192000,384000,768000};
 for(int sridx=0;sridx<8;sridx++){
  setp(p,9,1);startup(p,rates[sridx]);for(int i=0;i<N;i++){left[i]=(float)noise();right[i]=(float)noise();}block(p,N,0,true);
  CHECK(!memcmp(left,ol,sizeof(left))&&!memcmp(right,orr,sizeof(right)),"bypass preserves input exactly");shutdown(p);
 }
 setp(p,9,0);setp(p,6,0);setp(p,7,0);startup(p,48000);block(p,N,0,true);CHECK(!memcmp(left,ol,sizeof(left))&&!memcmp(right,orr,sizeof(right)),"dry mix preserves input");
 double dl[N],dr[N],do1[N],do2[N];for(int i=0;i<N;i++){dl[i]=noise();dr[i]=noise();}double*din[2]={dl,dr},*dout[2]={do1,do2};clap_audio_buffer_t ib={.data64=din,.channel_count=2},ob={.data64=dout,.channel_count=2};
 clap_process_t pr={.steady_time=-1,.frames_count=N,.audio_inputs=&ib,.audio_outputs=&ob,.audio_inputs_count=1,.audio_outputs_count=1,.out_events=&out_events};CHECK(p->process(p,&pr)!=CLAP_PROCESS_ERROR,"64-bit process");CHECK(!memcmp(dl,do1,sizeof(dl))&&!memcmp(dr,do2,sizeof(dr)),"64-bit dry exact");shutdown(p);
 printf("PASS 32/64-bit processing, exact dry/bypass, 1-768 kHz\n");

 /* Check the visible playhead against host beat position, seeks and meter. */
 click(ui,340,106);setp(p,3,4);startup(p,48000);blank();block(p,1,8,false);
 CHECK(gui_pixel(ui,367,480)==RGB(255,175,91),"4/4 half-cycle source marker");
 float*ti[2]={left,right},*to[2]={ol,orr};clap_audio_buffer_t tin={.data32=ti,.channel_count=2},tout={.data32=to,.channel_count=2};
 clap_event_transport_t three=transport(6,83,false);three.tsig_num=3;clap_process_t tpr={.frames_count=1,.transport=&three,.audio_inputs=&tin,.audio_outputs=&tout,.audio_inputs_count=1,.audio_outputs_count=1,.out_events=&out_events};
 CHECK(p->process(p,&tpr)!=CLAP_PROCESS_ERROR,"3/4 transport");CHECK(gui_pixel(ui,367,480)==RGB(255,175,91),"3/4 bar duration respects meter");
 block(p,1,16,false);CHECK(gui_pixel(ui,367,274)==RGB(255,175,91),"seek to next cycle returns to start");shutdown(p);
 printf("PASS host beat sync, 3/4 meter and cycle seek/loop\n");

 /* Left source has stronger left output. Far source is quieter. */
 setp(p,6,1);setp(p,2,0);setp(p,0,100);setp(p,7,0);setp(p,8,0);place(ui,280,377);startup(p,48000);
 double energy[2]={0};for(int b=0;b<64;b++){for(int i=0;i<N;i++)left[i]=right[i]=(float)(.2*sin(2*3.141592653589793*1000*(b*N+i)/48000));block(p,N,0,false);if(b>8)for(int i=0;i<N;i++){energy[0]+=ol[i]*ol[i];energy[1]+=orr[i]*orr[i];}}
 CHECK(energy[0]>energy[1]*4,"left-ear direction dominance");double direction_db=10*log10(energy[0]/energy[1]);shutdown(p);
 double dist_energy[2]={0};for(int pos=0;pos<2;pos++){
  place(ui,367-(pos?196:12),377);startup(p,48000);for(int b=0;b<50;b++){for(int i=0;i<N;i++)left[i]=right[i]=(float)noise();block(p,N,0,false);if(b>10)for(int i=0;i<N;i++)dist_energy[pos]+=ol[i]*ol[i]+orr[i]*orr[i];}shutdown(p);
 }
 double distance_db=10*log10(dist_energy[0]/dist_energy[1]);CHECK(distance_db>15,"far distance quieter by >15 dB");
 place(ui,563,377);startup(p,48000);blank();left[0]=right[0]=.5f;block(p,N,0,false);int first[2]={-1,-1};for(int i=0;i<N;i++){if(first[0]<0&&fabs(ol[i])>1e-6)first[0]=i;if(first[1]<0&&fabs(orr[i])>1e-6)first[1]=i;}CHECK(first[0]>first[1]+10,"right-ear arrival precedes left");shutdown(p);
 printf("PASS direction %.2f dB; near/far %.2f dB; right-ear lead %d samples\n",direction_db,distance_db,first[0]-first[1]);

 /* Freehand drawing persists, Undo restores, then state is deterministic. */
 click(ui,66,106);Memory before_draw=save(p);SendMessageW(ui,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(367,181));for(int i=1;i<=80;i++)SendMessageW(ui,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(367+(int)(140*sin(i*.055)),181+i*4));SendMessageW(ui,WM_LBUTTONUP,0,MAKELPARAM(240,501));
 Memory drawing=save(p);CHECK(memcmp(before_draw.data,drawing.data,drawing.size),"freehand changes persisted state");CHECK(*(uint32_t*)(drawing.data+8)==128,"drawing resampled to 128 points");CHECK(dirty_count>0&&flush_count>0,"host notified of edits");
 click(ui,255,106);Memory undone=save(p);CHECK(!memcmp(before_draw.data+56,undone.data+56,before_draw.size-56),"Undo restores point path");CHECK(load(p,&drawing),"restore drawn path");setp(p,8,0);setp(p,4,.42);setp(p,2,.6);setp(p,1,.9);Memory saved=save(p);
 const clap_plugin_t*p2=factory->create_plugin(factory,&host,d->id);CHECK(p2&&p2->init(p2),"second independent instance");CHECK(load(p2,&saved),"partial-read state recall");Memory recalled=save(p2);CHECK(saved.size==recalled.size&&!memcmp(saved.data,recalled.data,saved.size),"state byte-identical roundtrip");
 Memory invalid=saved;invalid.data[0]^=3;CHECK(!load(p2,&invalid),"corrupt state rejected");Memory intact=save(p2);CHECK(!memcmp(recalled.data,intact.data,intact.size),"invalid state has no partial effect");
 startup(p,48000);startup(p2,48000);for(int b=0;b<16;b++){
  for(int i=0;i<N;i++)left[i]=right[i]=(float)noise();block(p,N,0,false);float expect_l[N],expect_r[N];memcpy(expect_l,ol,sizeof(ol));memcpy(expect_r,orr,sizeof(orr));block(p2,N,0,false);CHECK(!memcmp(expect_l,ol,sizeof(ol))&&!memcmp(expect_r,orr,sizeof(orr)),"saved project audio recall deterministic");
 }
 p->reset(p);blank();block(p,N,0,false);for(int i=0;i<N;i++)CHECK(fabs(ol[i])+fabs(orr[i])<1e-10,"reset clears all DSP tails");shutdown(p);shutdown(p2);
 printf("PASS freehand, Undo, partial-stream state roundtrip, corrupt-state rejection, two-instance recall/reset\n");

 /* A parameter event at sample 128 must affect sample 128, including in-place audio. */
 setp(p,6,0);setp(p,7,-6);setp(p,9,0);startup(p,48000);for(int i=0;i<N;i++)left[i]=right[i]=.1f;
 clap_event_param_value_t ev={0};ev.header=(clap_event_header_t){sizeof(ev),128,0,CLAP_EVENT_PARAM_VALUE,0};ev.param_id=9;ev.value=1;ev.note_id=ev.port_index=ev.channel=ev.key=-1;
 EventList evlist={{&ev.header},1};clap_input_events_t ie={&evlist,event_size,event_get};float*same[2]={left,right};clap_audio_buffer_t samebuf={.data32=same,.channel_count=2};clap_process_t ip={.frames_count=N,.audio_inputs=&samebuf,.audio_outputs=&samebuf,.audio_inputs_count=1,.audio_outputs_count=1,.in_events=&ie,.out_events=&out_events};
 CHECK(p->process(p,&ip)!=CLAP_PROCESS_ERROR,"in-place sample accurate events");for(int i=0;i<128;i++)CHECK(left[i]>.049f&&left[i]<.052f,"pre-event output trim");for(int i=128;i<N;i++)CHECK(left[i]==.1f&&right[i]==.1f,"bypass event at sample 128");shutdown(p);setp(p,9,0);
 printf("PASS sample-offset parameter events and in-place audio\n");

 /* Reverb remains finite after a long tail and decays rather than running away. */
 click(ui,580,106);setp(p,1,1);setp(p,2,1);setp(p,6,1);setp(p,7,0);startup(p,48000);blank();left[0]=right[0]=.5f;block(p,N,0,false);double early=0,late=0,max=0;
 for(int b=1;b<1500;b++){blank();block(p,N,0,false);for(int i=0;i<N;i++){double e=ol[i]*ol[i]+orr[i]*orr[i];if(b<100)early+=e;if(b>1400)late+=e;if(fabs(ol[i])>max)max=fabs(ol[i]);if(fabs(orr[i])>max)max=fabs(orr[i]);}}
 printf("Tail metrics: early=%g late=%g peak=%g\n",early,late,max);CHECK(early>1e-8&&late<early*.001&&max<=1,"room tail stable and decays");shutdown(p);
 printf("PASS long-tail stability, late/early energy %.3g\n",late/early);

 /* Export a real processed stereo demo, synced to host time at 120 BPM. */
 click(ui,440,106);setp(p,6,1);setp(p,7,-3);setp(p,3,4);setp(p,8,1);startup(p,48000);
 FILE*f=fopen("outputs/SpatialInk/Headphone-demo.wav","wb");CHECK(f,"audio demo export");int total=48000*8;wave_header(f,total,48000);LARGE_INTEGER freq,t0,t1;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&t0);
 for(int at=0;at<total;at+=N){int n=total-at<N?total-at:N;for(int i=0;i<n;i++){
  double t=(at+i)/48000.0,pulse=fmod(t,.25),envelope=exp(-pulse*16);double snd=.27*envelope*(sin(2*3.141592653589793*220*t)+.35*sin(2*3.141592653589793*880*t))+.018*noise();left[i]=right[i]=(float)snd;
 }block(p,n,at/24000.0,true);for(int i=0;i<n;i++){int16_t v[2]={(int16_t)(fmax(-1,fmin(1,ol[i]))*32767),(int16_t)(fmax(-1,fmin(1,orr[i]))*32767)};fwrite(v,sizeof(v),1,f);}}
 QueryPerformanceCounter(&t1);double seconds=(double)(t1.QuadPart-t0.QuadPart)/freq.QuadPart;fclose(f);shutdown(p);
 printf("PASS host-synced 8-second headphone demo, render %.3f s (%.1f%% real-time)\n",seconds,seconds/8*100);
 /* ORBIT SIZE: slider widens the orbit live, hand-drawn paths ignore it, state round-trips. */
 {
  double v=0;
  click(ui,66,106);click(ui,340,106);setp(p,8,0);setp(p,4,0);setp(p,2,0);setp(p,6,1);setp(p,7,0);setp(p,0,30);
  CHECK(save(p).data[12]==1,"ORBIT button turns the live orbit on");
  /* Slider hit test: far right of the ORBIT SIZE slider (slot 1, y=193) gives the maximum, far left the minimum. */
  SendMessageW(ui,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(991,193));SendMessageW(ui,WM_LBUTTONUP,0,MAKELPARAM(991,193));
  CHECK(pa->get_value(p,10,&v)&&fabs(v-.95)<1e-3,"slider drag to the right edge sets ORBIT SIZE 95%");
  SendMessageW(ui,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(751,193));SendMessageW(ui,WM_LBUTTONUP,0,MAKELPARAM(751,193));
  CHECK(pa->get_value(p,10,&v)&&fabs(v-.05)<1e-3,"slider drag to the left edge sets ORBIT SIZE 5%");
  CHECK(save(p).data[12]==1,"moving the slider keeps the orbit on");
  /* Live audio: a small orbit sits close to the head, then automation widens it while processing. */
  setp(p,10,.1);startup(p,48000);double near_e=0,far_e=0;
  for(int b=0;b<40;b++){for(int i=0;i<N;i++)left[i]=right[i]=(float)noise();block(p,N,0,false);if(b>20)for(int i=0;i<N;i++)near_e+=ol[i]*ol[i]+orr[i]*orr[i];}
  setp(p,10,.9);
  for(int b=0;b<40;b++){for(int i=0;i<N;i++)left[i]=right[i]=(float)noise();block(p,N,0,false);if(b>20)for(int i=0;i<N;i++)far_e+=ol[i]*ol[i]+orr[i]*orr[i];}
  shutdown(p);double orbit_db=10*log10(near_e/far_e);CHECK(orbit_db>10,"widening ORBIT SIZE live moves the source away (quieter by >10 dB)");
  print_gui(ui,"work/spatial-ink/orbit-90.bmp");
  /* State: orbit flag and size round-trip, and the saved circle has the slider radius. */
  Memory orb=save(p);const uint32_t*pts=(const uint32_t*)(orb.data+16+4*12);int ry=(int16_t)(pts[0]>>16);CHECK(orb.data[12]==1&&abs(ry+(int)lround(.9*32767))<=2,"saved orbit points use the ORBIT SIZE radius");
  const clap_plugin_t*p3=factory->create_plugin(factory,&host,d->id);CHECK(p3&&p3->init(p3)&&load(p3,&orb),"orbit state load");Memory orb2=save(p3);CHECK(orb.size==orb2.size&&!memcmp(orb.data,orb2.data,orb.size),"orbit state byte-identical roundtrip");p3->destroy(p3);
  /* Direction: clockwise by default (a quarter of the way round is on the right), COUNTER-CW mirrors it live. */
  CHECK(pa->get_value(p,11,&v)&&v==0,"ORBIT DIRECTION defaults to clockwise");
  CHECK((int16_t)pts[32]>0,"clockwise orbit passes through the right side first");
  setp(p,4,.25);double lr[2][2]={{0}};
  for(int dir=0;dir<2;dir++){
   click(ui,dir?930:810,594);CHECK(pa->get_value(p,11,&v)&&v==(double)dir,"direction buttons set ORBIT DIRECTION");
   startup(p,48000);for(int b=0;b<30;b++){for(int i=0;i<N;i++)left[i]=right[i]=(float)noise();block(p,N,0,false);if(b>12)for(int i=0;i<N;i++){lr[dir][0]+=ol[i]*ol[i];lr[dir][1]+=orr[i]*orr[i];}}shutdown(p);
  }
  CHECK(lr[0][1]>lr[0][0]*2&&lr[1][0]>lr[1][1]*2,"quarter-way source is right for clockwise and left for counter-clockwise");
  Memory ccw=save(p);const uint32_t*cpts=(const uint32_t*)(ccw.data+16+4*12);CHECK(ccw.data[12]==1&&(int16_t)cpts[32]<0,"saved counter-clockwise orbit passes through the left side first");
  click(ui,930,594);print_gui(ui,"work/spatial-ink/orbit-ccw.bmp");click(ui,810,594);setp(p,4,0);
  /* A hand-drawn path is not an orbit: the slider must not change it. */
  click(ui,66,106);SendMessageW(ui,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(367,181));for(int i=1;i<=40;i++)SendMessageW(ui,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(367+(int)(100*sin(i*.1)),181+i*5));SendMessageW(ui,WM_LBUTTONUP,0,MAKELPARAM(300,380));
  Memory drawn=save(p);CHECK(drawn.data[12]==0,"drawing a path turns the orbit off");setp(p,10,.3);Memory drawn2=save(p);
  CHECK(!memcmp(drawn.data+16+4*12,drawn2.data+16+4*12,sizeof(uint32_t)*POINTS_IN_STATE),"slider leaves a hand-drawn path untouched");
  printf("PASS ORBIT SIZE slider + direction, live widening %.1f dB, state roundtrip, drawn paths unaffected\n",orbit_db);
 }
 p2->destroy(p2);g->destroy(p);CHECK(g->create(p,CLAP_WINDOW_API_WIN32,true),"floating GUI create after embedded destroy");g->destroy(p);p->destroy(p);DestroyWindow(parent);entry->deinit();entry->deinit();FreeLibrary(dll);
 printf("ALL TESTS PASSED; outgoing host events: %d\n",out_count);return 0;
}
