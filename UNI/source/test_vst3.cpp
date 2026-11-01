/* UNI - native VST3 integration host. MIT, see package LICENSE.txt.
   Loads the actual VST3 binary; no CLAP loader or separately installed CLAP is used. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
#define CHECK(x,msg) do {if(!(x)){std::fprintf(stderr,"FAIL: %s (line %d)\n",msg,__LINE__);std::exit(1);}}while(0)
static bool ok(tresult r){return r==kResultOk||r==kResultTrue;}
static bool isid(const TUID a,const FUID&b){return FUnknownPrivate::iidEqual(a,b);}
static std::string ascii(const char16*s){std::string r;while(*s)r.push_back(*s++<128?(char)s[-1]:'?');return r;}
template<class T> static void text16(T*out,const char*in){while(*in)*out++=(T)*in++;*out=0;}
struct Counts {unsigned begin=0,value=0,end=0;double last=0;};

class Host : public IHostApplication,public IComponentHandler,public IPlugFrame {
 uint32 refs=1;
public:
 std::array<Counts,39> edits{};unsigned restarts=0;
 tresult PLUGIN_API queryInterface(const TUID id,void**obj) override {
  if(!obj)return kInvalidArgument;*obj=nullptr;
  if(isid(id,FUnknown::iid)||isid(id,IHostApplication::iid))*obj=static_cast<IHostApplication*>(this);
  else if(isid(id,IComponentHandler::iid))*obj=static_cast<IComponentHandler*>(this);
  else if(isid(id,IPlugFrame::iid))*obj=static_cast<IPlugFrame*>(this);
  if(*obj){addRef();return kResultOk;}return kNoInterface;
 }
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 tresult PLUGIN_API getName(String128 name) override {text16(name,"UNI VST3 integration host");return kResultOk;}
 tresult PLUGIN_API createInstance(TUID,TUID,void**obj) override {if(obj)*obj=nullptr;return kNoInterface;}
 tresult PLUGIN_API beginEdit(ParamID id) override {if(id<39)edits[id].begin++;return kResultOk;}
 tresult PLUGIN_API performEdit(ParamID id,ParamValue v) override {CHECK(std::isfinite(v)&&v>=0&&v<=1,"GUI edit has valid normalized value");if(id<39){edits[id].value++;edits[id].last=v;}return kResultOk;}
 tresult PLUGIN_API endEdit(ParamID id) override {if(id<39)edits[id].end++;return kResultOk;}
 tresult PLUGIN_API restartComponent(int32) override {restarts++;return kResultOk;}
 tresult PLUGIN_API resizeView(IPlugView*view,ViewRect*rect) override {return view->onSize(rect);}
};

class MemoryStream : public IBStream {
 uint32 refs=1;
public:
 std::vector<uint8_t> bytes;int64 pos=0;
 tresult PLUGIN_API queryInterface(const TUID id,void**obj) override {if(!obj)return kInvalidArgument;*obj=nullptr;if(isid(id,FUnknown::iid)||isid(id,IBStream::iid)){*obj=this;addRef();return kResultOk;}return kNoInterface;}
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 tresult PLUGIN_API read(void*buf,int32 n,int32*got) override {if(n<0||!buf)return kInvalidArgument;int32 amount=(int32)std::min<int64>(n,(int64)bytes.size()-pos);if(amount<0)return kResultFalse;std::memcpy(buf,bytes.data()+pos,amount);pos+=amount;if(got)*got=amount;return amount==n?kResultOk:kResultFalse;}
 tresult PLUGIN_API write(void*buf,int32 n,int32*put) override {if(n<0||!buf||pos+n>1024*1024)return kInvalidArgument;if(pos+n>(int64)bytes.size())bytes.resize((size_t)(pos+n));std::memcpy(bytes.data()+pos,buf,n);pos+=n;if(put)*put=n;return kResultOk;}
 tresult PLUGIN_API seek(int64 offset,int32 mode,int64*result) override {int64 target=(mode==kIBSeekSet?0:mode==kIBSeekCur?pos:(int64)bytes.size())+offset;if(target<0||target>1024*1024)return kInvalidArgument;pos=target;if(result)*result=pos;return kResultOk;}
 tresult PLUGIN_API tell(int64*result) override {if(!result)return kInvalidArgument;*result=pos;return kResultOk;}
};

class Queue final : public IParamValueQueue {
 uint32 refs=1;
public:
 ParamID id;std::vector<std::pair<int32,ParamValue>> points;
 explicit Queue(ParamID p):id(p){}
 tresult PLUGIN_API queryInterface(const TUID i,void**o) override {if(!o)return kInvalidArgument;*o=nullptr;if(isid(i,FUnknown::iid)||isid(i,IParamValueQueue::iid)){*o=this;addRef();return kResultOk;}return kNoInterface;}
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 ParamID PLUGIN_API getParameterId() override{return id;}
 int32 PLUGIN_API getPointCount() override{return (int32)points.size();}
 tresult PLUGIN_API getPoint(int32 index,int32&offset,ParamValue&value) override {if(index<0||index>=(int32)points.size())return kInvalidArgument;offset=points[index].first;value=points[index].second;return kResultOk;}
 tresult PLUGIN_API addPoint(int32 offset,ParamValue value,int32&index) override {index=(int32)points.size();points.emplace_back(offset,value);return kResultOk;}
};
class Changes : public IParameterChanges {
 uint32 refs=1;
public:
 std::vector<std::unique_ptr<Queue>> queues;
 tresult PLUGIN_API queryInterface(const TUID i,void**o) override {if(!o)return kInvalidArgument;*o=nullptr;if(isid(i,FUnknown::iid)||isid(i,IParameterChanges::iid)){*o=this;addRef();return kResultOk;}return kNoInterface;}
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 int32 PLUGIN_API getParameterCount() override{return (int32)queues.size();}
 IParamValueQueue* PLUGIN_API getParameterData(int32 i) override {return i<0||i>=(int32)queues.size()?nullptr:queues[i].get();}
 IParamValueQueue* PLUGIN_API addParameterData(const ParamID&id,int32&index) override {for(size_t i=0;i<queues.size();i++)if(queues[i]->id==id){index=(int32)i;return queues[i].get();}index=(int32)queues.size();queues.emplace_back(std::make_unique<Queue>(id));return queues.back().get();}
 void set(ParamID id,double value,int32 offset=0){int32 index;auto*q=addParameterData(id,index);q->addPoint(offset,value,index);}
};

static void pump(unsigned milliseconds=45){DWORD until=GetTickCount()+milliseconds;do{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while((int32)(GetTickCount()-until)<0);}
struct Expected {const char*name;double min,max;bool stepped;};
static const Expected expected[39]={
 {"Maximum distance",1,100,false},{"Room size",0,1,false},{"Ambience",0,1,false},{"Cycle length (bars)",1,16,true},
 {"Path position",0,1,false},{"Height (degrees)",-90,90,false},{"Mix",0,1,false},{"Output (dB)",-24,6,false},
 {"Follow transport",0,1,true},{"Bypass",0,1,true},{"Orbit size",.05,.95,false},{"Direction",0,1,true},
 {"Source X (left-right)",-1,1,false},{"Source Y (front-back)",-1,1,false},{"Position mode",0,1,true},
 {"Tempo source",0,1,true},{"Custom tempo (BPM)",20,999,false},{"Speed multiplier",.0625,16,false},
 {"Double",0,1,true},{"Point B position",0,1,false},{"Two pages",0,1,true},
 {"Page B: Maximum distance",1,100,false},{"Page B: Room size",0,1,false},
 {"Page B: Ambience",0,1,false},{"Page B: Cycle length (bars)",1,16,true},
 {"Page B: Path position",0,1,false},{"Page B: Height (degrees)",-90,90,false},
 {"Page B: Mix",0,1,false},{"Page B: Output (dB)",-24,6,false},
 {"Page B: Follow transport",0,1,true},{"Page B: Bypass",0,1,true},
 {"Page B: Orbit size",.05,.95,false},{"Page B: Direction",0,1,true},
 {"Page B: Source X (left-right)",-1,1,false},{"Page B: Source Y (front-back)",-1,1,false},
 {"Page B: Position mode",0,1,true},{"Page B: Tempo source",0,1,true},
 {"Page B: Custom tempo (BPM)",20,999,false},{"Page B: Speed multiplier",.0625,16,false}
};
static bool close_value(double a,double b){return std::fabs(a-b)<2e-5*std::max(1.,std::fabs(b));}
static std::array<float,39> plain_state(const MemoryStream&s){
 /* Validate the preserved engine state inside the actual VST3 component state.
    This observes the audio engine values, rather than only the controller cache. */
 for(size_t off=0;off+16+39*4+256*4+8+256*4<=s.bytes.size();off++){
  uint32_t magic,version;std::memcpy(&magic,s.bytes.data()+off,4);std::memcpy(&version,s.bytes.data()+off+4,4);
  if(magic==0x4f4e4953&&version==7){std::array<float,39> out;std::memcpy(out.data(),s.bytes.data()+off+16,out.size()*sizeof(float));return out;}
 }
 CHECK(false,"engine state is present in VST3 state");return {};
}

class Instance {
public:
 Host host;IComponent*component=nullptr;IAudioProcessor*processor=nullptr;IEditController*controller=nullptr;
 bool separateController=false,active=false;std::array<ParamID,39> ids{};
 explicit Instance(IPluginFactory*factory,const TUID cid){
  CHECK(ok(factory->createInstance(cid,IComponent::iid,(void**)&component))&&component,"VST3 component creation");
  CHECK(ok(component->initialize(static_cast<IHostApplication*>(&host))),"VST3 component initialization");
  CHECK(ok(component->queryInterface(IAudioProcessor::iid,(void**)&processor))&&processor,"audio processor interface");
  if(!ok(component->queryInterface(IEditController::iid,(void**)&controller))){TUID controllerID;CHECK(ok(component->getControllerClassId(controllerID)),"controller class ID");CHECK(ok(factory->createInstance(controllerID,IEditController::iid,(void**)&controller)),"separate controller creation");CHECK(ok(controller->initialize(static_cast<IHostApplication*>(&host))),"controller initialization");separateController=true;}
  CHECK(controller&&ok(controller->setComponentHandler(&host)),"component handler connection");
  if(separateController){IConnectionPoint*a=nullptr,*b=nullptr;if(ok(component->queryInterface(IConnectionPoint::iid,(void**)&a))&&ok(controller->queryInterface(IConnectionPoint::iid,(void**)&b))){CHECK(ok(a->connect(b))&&ok(b->connect(a)),"processor/controller connection");}if(a)a->release();if(b)b->release();}
  ids.fill(kNoParamId);
  for(int32 i=0;i<controller->getParameterCount();i++){ParameterInfo info{};CHECK(ok(controller->getParameterInfo(i,info)),"parameter info");std::string name=ascii(info.title);for(int k=0;k<39;k++)if(name==expected[k].name){ids[k]=info.id;CHECK((info.flags&ParameterInfo::kCanAutomate)&&!(info.flags&ParameterInfo::kIsReadOnly),"all engine parameters are VST3 automatable");}}
  for(int k=0;k<39;k++)CHECK(ids[k]!=kNoParamId,"all 39 engine parameters exposed");
 }
 ~Instance(){if(active)stop();if(controller){controller->setComponentHandler(nullptr);if(separateController)controller->terminate();controller->release();}if(processor)processor->release();if(component){component->terminate();component->release();}}
 void start(int sampleSize=kSample32,double sr=48000){CHECK(!active,"not already active");CHECK(component->getBusCount(kAudio,kInput)==1&&component->getBusCount(kAudio,kOutput)==1,"one stereo input/output bus");BusInfo bi{};CHECK(ok(component->getBusInfo(kAudio,kInput,0,bi))&&bi.channelCount==2,"stereo input");CHECK(ok(component->getBusInfo(kAudio,kOutput,0,bi))&&bi.channelCount==2,"stereo output");SpeakerArrangement arr=SpeakerArr::kStereo;CHECK(ok(processor->setBusArrangements(&arr,1,&arr,1)),"stereo arrangements");CHECK(ok(processor->canProcessSampleSize(sampleSize)),"sample format supported");ProcessSetup setup{kRealtime,sampleSize,512,sr};CHECK(ok(processor->setupProcessing(setup)),"processing setup");CHECK(ok(component->activateBus(kAudio,kInput,0,true))&&ok(component->activateBus(kAudio,kOutput,0,true)),"activate audio buses");CHECK(ok(component->setActive(true))&&ok(processor->setProcessing(true)),"activate/start");active=true;}
 void stop(){CHECK(ok(processor->setProcessing(false))&&ok(component->setActive(false)),"stop/deactivate");active=false;}
 MemoryStream save(){MemoryStream s;CHECK(ok(component->getState(&s))&&!s.bytes.empty(),"VST3 state save");return s;}
 void restore(const MemoryStream&s){MemoryStream copy;copy.bytes=s.bytes;CHECK(ok(component->setState(&copy)),"VST3 processor state load");copy.pos=0;CHECK(ok(controller->setComponentState(&copy)),"VST3 controller state load");}
 void process_context(float*l,float*r,float*ol,float*orr,int n,Changes*changes,ProcessContext*context){
  float*in[2]={l,r},*out[2]={ol,orr};AudioBusBuffers ib{},ob{};ib.numChannels=ob.numChannels=2;ib.channelBuffers32=in;ob.channelBuffers32=out;
  ProcessData d{};d.processMode=kRealtime;d.symbolicSampleSize=kSample32;d.numSamples=n;d.numInputs=d.numOutputs=1;d.inputs=&ib;d.outputs=&ob;d.inputParameterChanges=changes;d.processContext=context;
  CHECK(ok(processor->process(d)),"VST3 audio process");for(int i=0;i<n;i++)CHECK(std::isfinite(ol[i])&&std::isfinite(orr[i]),"finite audio output");
 }
 void process(float*l,float*r,float*ol,float*orr,int n,Changes*changes=nullptr){
  ProcessContext ctx{};ctx.state=ProcessContext::kPlaying|ProcessContext::kTempoValid|ProcessContext::kTimeSigValid;ctx.sampleRate=48000;ctx.tempo=120;ctx.timeSigNumerator=ctx.timeSigDenominator=4;
  process_context(l,r,ol,orr,n,changes,&ctx);
 }
 void parameter(int k,double normalized,int offset=0){float zero[512]={0},ol[512]={0},orr[512]={0};Changes c;c.set(ids[k],normalized,offset);process(zero,zero,ol,orr,512,&c);}
};

static HWND child=nullptr;
static BOOL CALLBACK enum_child(HWND hwnd,LPARAM){wchar_t name[128];GetClassNameW(hwnd,name,128);if(!wcscmp(name,L"UNI.CLAP.0.2")){child=hwnd;return FALSE;}return TRUE;}
static void click(HWND hwnd,int x,int y){SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));SendMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));}
static void snapshot(HWND hwnd,const char*path){HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=1040;bi.bmiHeader.biHeight=-930;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;void*bits=nullptr;HBITMAP bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);CHECK(bm,"snapshot bitmap");auto old=SelectObject(dc,bm);SendMessageW(hwnd,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);FILE*f=std::fopen(path,"wb");CHECK(f,"snapshot output");BITMAPFILEHEADER h{};h.bfType=0x4d42;h.bfOffBits=sizeof(h)+sizeof(bi.bmiHeader);h.bfSize=h.bfOffBits+1040*930*4;std::fwrite(&h,sizeof(h),1,f);std::fwrite(&bi.bmiHeader,sizeof(bi.bmiHeader),1,f);std::fwrite(bits,1040*930*4,1,f);std::fclose(f);SelectObject(dc,old);DeleteObject(bm);DeleteDC(dc);}

static double normalized(int k,double plain){return (plain-expected[k].min)/(expected[k].max-expected[k].min);}
static size_t engine_state_offset(const MemoryStream&s,uint32_t expectedVersion,uint32_t parameterCount){
 for(size_t off=0;off+16+parameterCount*4+256*4<=s.bytes.size();off++){
  uint32_t magic,version;std::memcpy(&magic,s.bytes.data()+off,4);std::memcpy(&version,s.bytes.data()+off+4,4);
  if(magic==0x4f4e4953&&version==expectedVersion)return off;
 }
 std::fprintf(stderr,"State bytes=%zu, expected version=%u count=%u\n",s.bytes.size(),expectedVersion,parameterCount);
 for(size_t off=0;off+8<=s.bytes.size();off++){uint32_t magic,version;std::memcpy(&magic,s.bytes.data()+off,4);if(magic==0x4f4e4953){std::memcpy(&version,s.bytes.data()+off+4,4);std::fprintf(stderr,"Found engine magic at %zu version %u\n",off,version);}}
 CHECK(false,"requested engine state format found");return 0;
}
static void check_legacy_vst3_migration(IPluginFactory*currentFactory,const TUID currentCid,const char*legacyPath){
 HMODULE legacyModule=LoadLibraryA(legacyPath);CHECK(legacyModule,"load actual previous UNI VST3 DLL");
 auto legacyInit=(bool(*)())GetProcAddress(legacyModule,"InitDll");auto legacyExit=(bool(*)())GetProcAddress(legacyModule,"ExitDll");if(legacyInit)CHECK(legacyInit(),"legacy VST3 InitDll");
 auto legacyGetFactory=(IPluginFactory*(*)())GetProcAddress(legacyModule,"GetPluginFactory");CHECK(legacyGetFactory,"legacy VST3 factory export");IPluginFactory*legacyFactory=legacyGetFactory();CHECK(legacyFactory,"legacy factory");
 TUID legacyCid{};bool found=false;for(int32 i=0;i<legacyFactory->countClasses();i++){PClassInfo info{};CHECK(ok(legacyFactory->getClassInfo(i,&info)),"legacy factory class info");if(!std::strcmp(info.category,kVstAudioEffectClass)){CHECK(!found,"one legacy effect class");found=true;std::memcpy(legacyCid,info.cid,sizeof(legacyCid));}}
 CHECK(found&&!std::memcmp(legacyCid,currentCid,sizeof(legacyCid)),"renamed/upgraded VST3 keeps the legacy class identity");
 {
  Host legacyHost;IComponent*legacyComponent=nullptr;IEditController*legacyController=nullptr;IAudioProcessor*legacyProcessor=nullptr;
  CHECK(ok(legacyFactory->createInstance(legacyCid,IComponent::iid,(void**)&legacyComponent))&&legacyComponent,"legacy component instance");CHECK(ok(legacyComponent->initialize(static_cast<IHostApplication*>(&legacyHost))),"legacy component initialize");
  CHECK(ok(legacyComponent->queryInterface(IEditController::iid,(void**)&legacyController))&&legacyController,"legacy united controller");CHECK(ok(legacyController->setComponentHandler(&legacyHost)),"legacy component handler");
  CHECK(ok(legacyComponent->queryInterface(IAudioProcessor::iid,(void**)&legacyProcessor))&&legacyProcessor,"previous audio processor");
  MemoryStream initial;CHECK(ok(legacyComponent->getState(&initial)),"state saved by actual legacy component");
  uint32_t oldVersion=0;CHECK(initial.bytes.size()>=8,"previous state header");std::memcpy(&oldVersion,initial.bytes.data()+4,4);
  CHECK(oldVersion==5||oldVersion==6,"previous UNI state is v5 or v6");uint32_t oldCount=oldVersion==5?18:20;
  size_t oldOffset=engine_state_offset(initial,oldVersion,oldCount);
  const float oldParameters[20]={42,.31f,.27f,7,.37f,25,.67f,-8,0,0,.73f,1,-.4f,.2f,0,1,145,2,1,.25f};
  Instance current(currentFactory,currentCid);
  for(int shape=0;shape<2;shape++){
   MemoryStream seeded;seeded.bytes=initial.bytes;std::memcpy(seeded.bytes.data()+oldOffset+16,oldParameters,oldCount*4);
   if(shape){
    const uint32_t count=4,orbit=0,points[4]={0xc000a000u,0x1000d000u,0x60002000u,0xe0006000u};std::memcpy(seeded.bytes.data()+oldOffset+8,&count,4);std::memcpy(seeded.bytes.data()+oldOffset+12,&orbit,4);
    std::memset(seeded.bytes.data()+oldOffset+16+oldCount*4,0,256*4);std::memcpy(seeded.bytes.data()+oldOffset+16+oldCount*4,points,sizeof(points));
   }
   CHECK(ok(legacyComponent->setState(&seeded)),"legacy component validates seeded orbit/custom state");seeded.pos=0;CHECK(ok(legacyController->setComponentState(&seeded)),"legacy controller recall");
   MemoryStream oldSaved;CHECK(ok(legacyComponent->getState(&oldSaved)),"actual previous save after recall");oldOffset=engine_state_offset(oldSaved,oldVersion,oldCount);
   current.restore(oldSaved);MemoryStream newSaved=current.save();size_t newOffset=engine_state_offset(newSaved,7,39);
   CHECK(!std::memcmp(oldSaved.bytes.data()+oldOffset+16,newSaved.bytes.data()+newOffset+16,oldCount*4),"migration preserves all original parameter values");
   CHECK(!std::memcmp(oldSaved.bytes.data()+oldOffset+8,newSaved.bytes.data()+newOffset+8,8),"migration preserves path count and orbit flag");
   CHECK(!std::memcmp(oldSaved.bytes.data()+oldOffset+16+oldCount*4,newSaved.bytes.data()+newOffset+16+39*4,256*4),"migration preserves all original packed path points");
   auto values=plain_state(newSaved);CHECK(values[18]==(oldCount==20?1:0)&&values[19]==(oldCount==20?.25f:.5f)&&values[20]==0&&values[25]==.5f,"migration retains Double and seeds second-page defaults");
   for(int k=0;k<39;k++)CHECK(close_value(current.controller->getParamNormalized(current.ids[k]),normalized(k,values[k])),"controller cache matches migrated engine values");
   SpeakerArrangement arr=SpeakerArr::kStereo;CHECK(ok(legacyProcessor->setBusArrangements(&arr,1,&arr,1)),"previous stereo arrangements");
   ProcessSetup setup{kRealtime,kSample32,512,48000};CHECK(ok(legacyProcessor->setupProcessing(setup)),"previous processing setup");
   CHECK(ok(legacyComponent->activateBus(kAudio,kInput,0,true))&&ok(legacyComponent->activateBus(kAudio,kOutput,0,true)),"previous stereo buses");
   CHECK(ok(legacyComponent->setActive(true))&&ok(legacyProcessor->setProcessing(true)),"previous plugin active");current.start();
   std::array<float,512> inputL{},inputR{},oldL{},oldR{},newL{},newR{};
   ProcessContext context{};context.state=ProcessContext::kPlaying|ProcessContext::kTempoValid|ProcessContext::kTimeSigValid;context.sampleRate=48000;context.tempo=120;context.timeSigNumerator=context.timeSigDenominator=4;
   for(int block=0;block<12;block++){
    for(int i=0;i<512;i++){inputL[i]=(float)(.11*std::sin(2*3.141592653589793*440*(i+512*block)/48000));inputR[i]=(float)(.08*std::sin(2*3.141592653589793*700*(i+512*block)/48000));}
    float*ins[2]={inputL.data(),inputR.data()},*outs[2]={oldL.data(),oldR.data()};AudioBusBuffers ib{},ob{};ib.numChannels=ob.numChannels=2;ib.channelBuffers32=ins;ob.channelBuffers32=outs;
    ProcessData d{};d.processMode=kRealtime;d.symbolicSampleSize=kSample32;d.numSamples=512;d.numInputs=d.numOutputs=1;d.inputs=&ib;d.outputs=&ob;d.processContext=&context;
    CHECK(ok(legacyProcessor->process(d)),"previous plugin audio render");current.process_context(inputL.data(),inputR.data(),newL.data(),newR.data(),512,nullptr,&context);
    for(int i=0;i<512;i++)CHECK(std::fabs(oldL[i]-newL[i])<2e-5&&std::fabs(oldR[i]-newR[i])<2e-5,"Single/Double Orbit audio matches the previous UNI engine");
   }
   current.stop();CHECK(ok(legacyProcessor->setProcessing(false))&&ok(legacyComponent->setActive(false)),"previous plugin stopped");
   MemoryStream truncated;truncated.bytes=oldSaved.bytes;truncated.bytes.pop_back();CHECK(!ok(current.component->setState(&truncated)),"truncated previous project state is rejected");CHECK(current.save().bytes==newSaved.bytes,"failed recall does not partially overwrite current state");
  }
  legacyController->setComponentHandler(nullptr);legacyController->release();legacyProcessor->release();legacyComponent->terminate();legacyComponent->release();
 }
 legacyFactory->release();if(legacyExit)CHECK(legacyExit(),"legacy VST3 ExitDll");FreeLibrary(legacyModule);
 std::printf("PASS previous VST3 v5/v6 state migration, original Single/Double Orbit audio, preserved parameters/path/CID and truncated-state rejection\n");
}
static void check_context_requirements(IPluginFactory*factory,const TUID cid){
 Host host;IComponent*component=nullptr;
 CHECK(ok(factory->createInstance(cid,IComponent::iid,(void**)&component))&&component,"context probe component");
 CHECK(ok(component->initialize(static_cast<IHostApplication*>(&host))),"context probe initialize");
 IProcessContextRequirements*requirements=nullptr;
 CHECK(ok(component->queryInterface(IProcessContextRequirements::iid,(void**)&requirements))&&requirements,"VST3 timing requirements interface");
 const uint32 required=IProcessContextRequirements::kNeedTempo|IProcessContextRequirements::kNeedProjectTimeMusic|IProcessContextRequirements::kNeedTimeSignature|IProcessContextRequirements::kNeedTransportState;
 uint32 flags=requirements->getProcessContextRequirements();std::printf("VST3 requested timing flags: 0x%X\n",flags);
 CHECK((flags&required)==required,"host is explicitly asked for tempo, musical position, meter and transport state");
 requirements->release();component->terminate();component->release();
 std::printf("PASS explicit VST3 tempo/music-position/meter/transport requirements\n");
}
struct RenderedGui {
 HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;
 explicit RenderedGui(HWND hwnd){dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1040;info.bmiHeader.biHeight=-930;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void*bits=nullptr;bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);CHECK(bitmap,"GUI probe bitmap");old=SelectObject(dc,bitmap);SendMessageW(hwnd,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);}
 ~RenderedGui(){SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);}
 double phase(bool dual=false){int start=dual?590:340,length=dual?268:518,width=0;while(width<length&&GetPixel(dc,start+width,644)==RGB(255,175,91))width++;return width/(double)length;}
 POINT source(){int minx=1040,maxx=0,miny=930,maxy=0;for(int y=160;y<590;y++)for(int x=140;x<620;x++)if(GetPixel(dc,x,y)==RGB(255,175,91)){minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);}CHECK(maxx>=minx&&maxy>=miny,"source marker found in rendered map");return POINT{(minx+maxx)/2,(miny+maxy)/2};}
 POINT source_b(){int minx=1040,maxx=0,miny=930,maxy=0;for(int y=160;y<590;y++)for(int x=140;x<620;x++)if(GetPixel(dc,x,y)==RGB(117,178,255)){minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);}CHECK(maxx>=minx&&maxy>=miny,"point B marker found in rendered map");return POINT{(minx+maxx)/2,(miny+maxy)/2};}
};
static ProcessContext context_at(double bpm,double beat,bool playing=true,uint32 extra=ProcessContext::kProjectTimeMusicValid,int numerator=4,int denominator=4){
 ProcessContext context{};context.state=ProcessContext::kTempoValid|ProcessContext::kTimeSigValid|extra|(playing?ProcessContext::kPlaying:0);
 context.sampleRate=48000;context.tempo=bpm;context.projectTimeMusic=beat;context.timeSigNumerator=numerator;context.timeSigDenominator=denominator;
 double barbeats=numerator*4.0/denominator;context.barPositionMusic=std::floor(beat/barbeats)*barbeats;return context;
}
static void run_context(Instance&a,int samples,double bpm,uint32 extra=ProcessContext::kProjectTimeMusicValid,bool playing=true,int numerator=4,int denominator=4,double initialBeat=0,bool noContext=false){
 float input[512]={0},left[512]={0},right[512]={0};
 for(int at=0;at<samples;){int n=std::min(512,samples-at);double beat=initialBeat+(playing?at*bpm/(60.0*48000):0);auto context=context_at(bpm,beat,playing,extra,numerator,denominator);context.projectTimeSamples=at;a.process_context(input,input,left,right,n,nullptr,noContext?nullptr:&context);at+=n;}
}
static void timing_configuration(Instance&a,const MemoryStream&defaults,bool custom=false,double bpm=120,double speed=1,double bars=4){
 if(a.active)a.stop();a.restore(defaults);a.start();
 a.parameter(15,custom?1:0);a.parameter(16,normalized(16,bpm));a.parameter(17,normalized(17,speed));a.parameter(3,normalized(3,bars));
 /* Reactivation seeds parameter smoothing at the requested settings and resets the clock. */
 a.stop();a.start();
}
static void check_phase(HWND hwnd,double expectedPhase,const char*message){double observed=RenderedGui(hwnd).phase();if(std::fabs(observed-expectedPhase)>.006)std::fprintf(stderr,"Phase expected %.6f, rendered %.6f\n",expectedPhase,observed);CHECK(std::fabs(observed-expectedPhase)<.006,message);}
static void check_tempo_and_direction(Instance&a,const MemoryStream&defaults,HWND hwnd){
 for(double bpm:{60.,90.,120.,180.}){timing_configuration(a,defaults);run_context(a,48000,bpm);check_phase(hwnd,bpm/60.0/16,"HOST SYNC follows the actual host BPM");}
 for(double speed:{2.,4.,8.}){timing_configuration(a,defaults,false,120,speed);run_context(a,24000,120);check_phase(hwnd,speed/16,"SPEED multiplies the synchronized cycle rate");}
 timing_configuration(a,defaults,true,90);run_context(a,48000,180);check_phase(hwnd,90.0/60/16,"CUSTOM BPM ignores host tempo while retaining playback state");
 timing_configuration(a,defaults);run_context(a,144000,120,ProcessContext::kProjectTimeMusicValid,true,3,4);check_phase(hwnd,.5,"3/4 meter makes four bars equal twelve quarter notes");
 timing_configuration(a,defaults);run_context(a,48000,120,ProcessContext::kBarPositionValid);check_phase(hwnd,.125,"bar-position validity alone never falsely resets musical position");
 timing_configuration(a,defaults);run_context(a,48000,120,0,true,4,4,0,true);check_phase(hwnd,.125,"missing VST3 context advances fallback clock instead of freezing");
 timing_configuration(a,defaults,true,90);run_context(a,48000,180,0,true,4,4,0,true);check_phase(hwnd,90.0/60/16,"CUSTOM BPM also runs when the host supplies no context");
 timing_configuration(a,defaults);run_context(a,24000,60);run_context(a,24000,180,ProcessContext::kProjectTimeMusicValid,true,4,4,.5);check_phase(hwnd,.125,"live host tempo change preserves musical beat continuity");
 timing_configuration(a,defaults);run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,false,4,4,8);check_phase(hwnd,.5,"stopped seek updates the host-locked phase");
 run_context(a,48000,120,ProcessContext::kProjectTimeMusicValid,false,4,4,8);check_phase(hwnd,.5,"stopped transport does not advance the phase");
 run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,true,4,4,2);check_phase(hwnd,.125,"backward seek and loop re-anchor to the musical timeline");
 /* A speed edit at a continuous beat must not multiply the entire song position and jump. */
 timing_configuration(a,defaults);run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,true,4,4,100);double before=RenderedGui(hwnd).phase();
 float input[1]={0},left[1]={0},right[1]={0};Changes speedChange;speedChange.set(a.ids[17],normalized(17,4));auto continuous=context_at(120,100+120.0/(60*48000));a.process_context(input,input,left,right,1,&speedChange,&continuous);
 CHECK(std::fabs(RenderedGui(hwnd).phase()-before)<.006,"live SPEED automation does not jump path phase");
 std::printf("PASS HOST SYNC 60/90/120/180 BPM, custom BPM, speed, meter, missing/bar-only contexts, stop/seek/loop and continuous speed edits\n");
 timing_configuration(a,defaults);click(hwnd,68,106);SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(264,377));SendMessageW(hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(470,377));SendMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(470,377));pump();
 POINT drawn[2]{};for(int reverse=0;reverse<2;reverse++){a.parameter(11,reverse);run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,true,4,4,4);drawn[reverse]=RenderedGui(hwnd).source();}
 CHECK(drawn[0].x<350&&drawn[1].x>390&&std::abs(drawn[0].y-377)<4&&std::abs(drawn[1].y-377)<4,"Direction reverses the DRAW path from end to start");
 click(hwnd,340,106);pump();POINT orbit[2]{};for(int reverse=0;reverse<2;reverse++){a.parameter(11,reverse);run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,true,4,4,4);orbit[reverse]=RenderedGui(hwnd).source();}
 CHECK(orbit[0].x>400&&orbit[1].x<330,"Direction selects clockwise/counter-clockwise ORBIT motion");
 std::printf("PASS forward/reverse direction for both DRAW paths and ORBIT\n");
 a.stop();a.restore(defaults);a.start();
}
static void check_double(Instance&a,const MemoryStream&defaults,HWND hwnd,const char*screenshotPath){
 a.stop();a.restore(defaults);a.start();click(hwnd,340,106);
 a.parameter(18,1);a.parameter(4,0);a.parameter(19,.5);
 run_context(a,1,120,ProcessContext::kProjectTimeMusicValid,true,4,4,4);
 POINT first=RenderedGui(hwnd).source(),second=RenderedGui(hwnd).source_b();
 CHECK(first.x>410&&second.x<325,"both Double points orbit at opposite sides of the listener");
 if(screenshotPath)snapshot(hwnd,screenshotPath);
 a.parameter(19,.25);run_context(a,4800,120,ProcessContext::kProjectTimeMusicValid,true,4,4,4);
 POINT moved=RenderedGui(hwnd).source_b();std::printf("Double marker x: A=%ld B=%ld, B after automation=%ld\n",first.x,second.x,moved.x);
 CHECK(moved.x>second.x+70,"Point B automation moves its own orbit marker");
 a.parameter(4,.25);run_context(a,4800,120,ProcessContext::kProjectTimeMusicValid,true,4,4,4);
 CHECK(std::fabs(RenderedGui(hwnd).phase()-.5)<.01,"existing Path position lane automates Point A orbit offset");
 std::printf("PASS two orbit markers, independent A/B VST3 automation and host-clock movement\n");

 std::array<float,512> input{},left{},right{};for(int i=0;i<512;i++)input[i]=(float)(.12*std::sin(2*3.141592653589793*900*i/48000));
 double energy[2][2]{};
 for(int side=0;side<2;side++){
  a.stop();a.restore(defaults);a.start();a.parameter(8,0);a.parameter(2,0);a.parameter(4,.25);a.parameter(19,side?.75:.25);a.parameter(18,1);
  for(int block=0;block<56;block++){a.process(input.data(),input.data(),left.data(),right.data(),512);
   if(block>24)for(int i=0;i<512;i++){energy[side][0]+=left[i]*left[i];energy[side][1]+=right[i]*right[i];}}
 }
 CHECK(energy[0][1]>energy[0][0]*1.5,"two coincident right-side voices favor the right ear");
 CHECK(energy[1][0]>energy[0][0]*1.3,"automating B to the opposite side adds audible left-ear energy");
 std::printf("PASS duplicated audio follows both independently automated spatial positions\n");
 a.stop();a.restore(defaults);a.start();
}
static void check_two_pages(Instance&a,Instance&b,const MemoryStream&defaults,HWND hwnd,const char*screenshotPath){
 constexpr int B=21;
 std::array<float,512> input{},left{},right{};for(int i=0;i<512;i++)input[i]=(float)(.12*std::sin(2*3.141592653589793*900*i/48000));
 double energy[2][2]{};
 for(int side=0;side<2;side++){
  a.stop();a.restore(defaults);a.start();a.parameter(20,1);a.parameter(9,1);
  a.parameter(B+14,1);a.parameter(B+13,.5);a.parameter(B+2,0);a.parameter(B+6,1);
  a.parameter(B+12,side?.75:.25);
  for(int block=0;block<56;block++){a.process(input.data(),input.data(),left.data(),right.data(),512);
   if(block>24)for(int i=0;i<512;i++){energy[side][0]+=left[i]*left[i];energy[side][1]+=right[i]*right[i];}}
  auto values=plain_state(a.save());CHECK(values[12]==0&&values[B+12]==(side?.5f:-.5f),"Page B position automation leaves Page A settings unchanged");
 }
 CHECK(energy[0][0]>energy[0][1]*1.1&&energy[1][1]>energy[1][0]*1.1,"Page B changes audible stereo location while Page A is unchanged");
 a.parameter(B+9,1);a.process(input.data(),input.data(),left.data(),right.data(),512);
 for(int i=0;i<512;i++)CHECK(std::fabs(left[i]-input[i])<1e-6&&std::fabs(right[i]-input[i])<1e-6,"both page bypasses return the original input");
 std::printf("PASS two independent audio engines, Page B spatial automation, both outputs and per-page bypass\n");

 a.stop();a.restore(defaults);a.start();a.parameter(20,1);a.parameter(B+15,1);a.parameter(B+16,normalized(B+16,90));a.stop();a.start();
 run_context(a,48000,120);click(hwnd,393,647);double phaseA=RenderedGui(hwnd).phase(true);
 click(hwnd,511,647);double phaseB=RenderedGui(hwnd).phase(true);
 CHECK(std::fabs(phaseA-.125)<.01&&std::fabs(phaseB-(.5+90.0/60/16))<.01,"pages keep independent host/custom clocks and orbit positions");
 std::printf("PASS Page A host sync and Page B custom BPM run independently\n");

 click(hwnd,68,106);SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(264,377));SendMessageW(hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(470,377));SendMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(470,377));pump();
 MemoryStream saved=a.save();size_t off=engine_state_offset(saved,7,39);uint32_t aOrbit=1,bOrbit=1;
 std::memcpy(&aOrbit,saved.bytes.data()+off+12,4);std::memcpy(&bOrbit,saved.bytes.data()+off+16+39*4+256*4+4,4);
 CHECK(aOrbit==1&&bOrbit==0,"drawing on Page B does not replace Page A orbit path");
 b.restore(saved);CHECK(b.save().bytes==saved.bytes,"both page paths and all controls survive VST3 state recall");
 if(screenshotPath){std::string path=screenshotPath;size_t dot=path.find_last_of('.');if(dot!=std::string::npos)path.insert(dot,"-TwoPages");else path+="-TwoPages.bmp";snapshot(hwnd,path.c_str());}
 std::printf("PASS separate page paths and byte-identical two-page save/restore\n");
 a.stop();a.restore(defaults);a.start();
}

int main(int argc,char**argv){
 std::setvbuf(stdout,nullptr,_IONBF,0);CHECK(argc>=2,"usage: test_vst3.exe plugin.vst3 [screenshot.bmp] [legacy-0.5.1.vst3]");
 std::string loadPath=argv[1];DWORD attributes=GetFileAttributesA(loadPath.c_str());
 if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))loadPath+="\\Contents\\x86_64-win\\UNI.vst3";
 HMODULE module=LoadLibraryA(loadPath.c_str());if(!module){std::fprintf(stderr,"LoadLibrary error %lu\n",GetLastError());return 1;}auto init=(bool(*)())GetProcAddress(module,"InitDll");auto exitDll=(bool(*)())GetProcAddress(module,"ExitDll");if(init)CHECK(init(),"VST3 InitDll");
 auto getFactory=(IPluginFactory*(*)())GetProcAddress(module,"GetPluginFactory");CHECK(getFactory,"GetPluginFactory export");IPluginFactory*factory=getFactory();CHECK(factory&&factory->countClasses()>0,"VST3 factory scan");TUID cid{};bool found=false;
 for(int32 i=0;i<factory->countClasses();i++){PClassInfo info{};CHECK(ok(factory->getClassInfo(i,&info)),"class info");if(!std::strcmp(info.category,kVstAudioEffectClass)){CHECK(!found,"one audio effect class");std::memcpy(cid,info.cid,sizeof(cid));found=true;std::printf("VST3 class: %s\n",info.name);}}CHECK(found,"audio effect class found");
 check_context_requirements(factory,cid);
 if(argc>=4)check_legacy_vst3_migration(factory,cid,argv[3]);
 {
  Instance a(factory,cid),b(factory,cid);MemoryStream defaults=a.save();
  for(int k=0;k<39;k++){CHECK(close_value(a.controller->normalizedParamToPlain(a.ids[k],0),expected[k].min)&&close_value(a.controller->normalizedParamToPlain(a.ids[k],1),expected[k].max),"normalized/plain endpoint conversion");CHECK(close_value(a.controller->plainParamToNormalized(a.ids[k],expected[k].min),0)&&close_value(a.controller->plainParamToNormalized(a.ids[k],expected[k].max),1),"plain/normalized endpoint conversion");}
  std::printf("PASS factory, component/controller, stereo buses and all 39 automatable parameter ranges\n");
  std::array<float,512> input{},aL{},aR{},bL{},bR{};for(int i=0;i<512;i++)input[i]=(float)(.15*std::sin(2*3.141592653589793*900*i/48000));
  a.start();
  for(int k=0;k<39;k++)for(int endpoint=0;endpoint<2;endpoint++){
   Changes flush;flush.set(a.ids[k],endpoint);
   a.process(input.data(),input.data(),aL.data(),aR.data(),0,&flush);
   auto values=plain_state(a.save());CHECK(close_value(values[k],endpoint?expected[k].max:expected[k].min),"zero-sample VST3 parameter flush reaches actual engine");
  }
  a.stop();std::printf("PASS silent/no-audio zero-sample parameter flush for all 39 parameters\n");
  for(int k=0;k<39;k++){
   a.restore(defaults);b.restore(defaults);a.start();b.start();a.parameter(k,0);b.parameter(k,0);
   Changes c;c.set(a.ids[k],1,128);a.process(input.data(),input.data(),aL.data(),aR.data(),512,&c);
   b.process(input.data(),input.data(),bL.data(),bR.data(),128);CHECK(!std::memcmp(aL.data(),bL.data(),128*sizeof(float))&&!std::memcmp(aR.data(),bR.data(),128*sizeof(float)),"automation does not apply before sample 128");
   Changes reference;reference.set(b.ids[k],1);b.process(input.data()+128,input.data()+128,bL.data()+128,bR.data()+128,384,&reference);
   CHECK(!std::memcmp(aL.data(),bL.data(),512*sizeof(float))&&!std::memcmp(aR.data(),bR.data(),512*sizeof(float)),"normalized automation timestamp equals split-block render");
   auto values=plain_state(a.save());CHECK(close_value(values[k],expected[k].max),"actual engine receives maximum normalized automation");a.parameter(k,0);values=plain_state(a.save());CHECK(close_value(values[k],expected[k].min),"actual engine receives minimum normalized automation");a.stop();b.stop();
  }
  std::printf("PASS actual VST3 normalized min/max playback for all 39 parameters and sample-128 audio equivalence\n");
  a.restore(defaults);a.start();a.parameter(9,1);a.process(input.data(),input.data(),aL.data(),aR.data(),512);CHECK(!std::memcmp(input.data(),aL.data(),512*sizeof(float))&&!std::memcmp(input.data(),aR.data(),512*sizeof(float)),"VST3 bypass preserves stereo input");a.parameter(9,0);a.parameter(14,1);a.parameter(13,.5);a.parameter(2,0);a.parameter(7,.8);a.parameter(6,1);
  double energies[2][2]{};for(int side=0;side<2;side++){a.parameter(12,side?.75:.25);for(int block=0;block<48;block++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);if(block>20)for(int i=0;i<512;i++){energies[side][0]+=aL[i]*aL[i];energies[side][1]+=aR[i]*aR[i];}}}CHECK(energies[0][0]>energies[0][1]*2&&energies[1][1]>energies[1][0]*2,"normalized X automation audibly moves left/right");a.stop();
  MemoryStream modified=a.save();b.restore(modified);MemoryStream recalled=b.save();CHECK(modified.bytes==recalled.bytes,"component state byte-identical roundtrip across instances");
  std::printf("PASS exact bypass, audible XY direction and VST3 component/controller state recall\n");
  a.restore(defaults);a.start();IPlugView*view=a.controller->createView(ViewType::kEditor);CHECK(view&&ok(view->isPlatformTypeSupported(kPlatformTypeHWND)),"native VST3 editor");CHECK(ok(view->setFrame(&a.host)),"view frame");HWND parent=CreateWindowExW(0,L"STATIC",L"UNI VST3 hidden integration host",WS_POPUP,0,0,1040,930,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(parent,"host HWND");CHECK(ok(view->attached(parent,kPlatformTypeHWND)),"embed native VST3 editor");child=nullptr;EnumChildWindows(parent,enum_child,0);CHECK(child,"embedded SpatialInk native GUI child");ViewRect size{};CHECK(ok(view->getSize(&size))&&size.getWidth()==1040&&size.getHeight()==930,"editor dimensions");
  check_tempo_and_direction(a,defaults,child);
  check_double(a,defaults,child,argc>=3?argv[2]:nullptr);
  check_two_pages(a,b,defaults,child,argc>=3?argv[2]:nullptr);
  const int controlIds[]={0,10,1,2,3,4,19,5,6,7,8,18,9,11,12,13,14,15,16,17};
  const int xs[]={875,875,875,875,875,875,875,875,875,875,100,470,950,930,460,890,100,230,400,750};
  const int ys[]={135,178,221,264,307,350,393,436,479,522,647,43,647,594,775,775,775,862,862,860};
  HWND bpmEdit=GetDlgItem(child,2001);CHECK(bpmEdit,"native editable BPM field");
  for(int i=0;i<20;i++){
   a.host.edits={};
   if(controlIds[i]==16){SetWindowTextW(bpmEdit,L"175.5");SendMessageW(bpmEdit,WM_KEYDOWN,VK_RETURN,0);}else click(child,xs[i],ys[i]);
   for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}
   auto count=a.host.edits[a.ids[controlIds[i]]];CHECK(count.begin>0&&count.begin==count.end&&count.value>0,"every native control emits VST3 beginEdit/performEdit/endEdit");
  }
  auto committed=plain_state(a.save());CHECK(close_value(committed[16],175.5)&&committed[15]==1&&committed[8]==1&&committed[14]==0,"typed BPM is committed and activates custom path playback");
  SetWindowTextW(bpmEdit,L"222");SendMessageW(bpmEdit,WM_KEYDOWN,VK_ESCAPE,0);auto escaped=plain_state(a.save());CHECK(close_value(escaped[16],175.5),"Escape cancels an unfinished BPM edit");
  /* The speed buttons are exact bounded multipliers, with independent host gestures. */
  a.parameter(17,normalized(17,2));click(child,962,862);for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}CHECK(close_value(plain_state(a.save())[17],4),"speed plus button doubles the rate");
  click(child,910,862);for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}CHECK(close_value(plain_state(a.save())[17],2),"speed minus button halves the rate");
  /* Both timing-mode buttons also make their audible path/transport dependencies explicit. */
  a.parameter(14,1);a.parameter(8,0);click(child,100,862);for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}auto synced=plain_state(a.save());CHECK(synced[15]==0&&synced[14]==0&&synced[8]==1,"HOST SYNC selects host tempo, path mode and follow transport");
  a.parameter(14,1);a.parameter(8,0);click(child,230,862);for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}auto custom=plain_state(a.save());CHECK(custom[15]==1&&custom[14]==0&&custom[8]==1,"CUSTOM BPM selects custom tempo, path mode and follow transport");
  /* Capture loss closes both X/Y gestures, even without a mouse-up. */
  click(child,150,106);a.host.edits={};SendMessageW(child,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,300));ReleaseCapture();for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}for(int k:{12,13})CHECK(a.host.edits[a.ids[k]].begin==1&&a.host.edits[a.ids[k]].end==1&&a.host.edits[a.ids[k]].value>0,"XY gestures end after capture loss");
  auto beforeB=plain_state(a.save());a.host.edits={};click(child,630,43);for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}
  auto modeEdit=a.host.edits[a.ids[20]];CHECK(modeEdit.begin>0&&modeEdit.begin==modeEdit.end&&modeEdit.value>0,"Two pages mode emits host automation gesture");
  click(child,510,647);pump();
  const int bControlIds[]={21,31,22,23,24,25,26,27,28,29,30,32,33,34,35,36,37,38};
  const int bXs[]={875,875,875,875,875,875,875,875,875,100,950,930,460,890,100,230,400,750};
  const int bYs[]={135,178,221,264,307,350,436,479,522,647,647,594,775,775,775,862,862,860};
  for(int i=0;i<18;i++){
   a.host.edits={};if(bControlIds[i]==29)a.parameter(29,0);
   if(bControlIds[i]==37){SetWindowTextW(bpmEdit,L"165.5");SendMessageW(bpmEdit,WM_KEYDOWN,VK_RETURN,0);}else click(child,bXs[i],bYs[i]);
   for(int n=0;n<3;n++){a.process(input.data(),input.data(),aL.data(),aR.data(),512);pump(35);}
   auto count=a.host.edits[a.ids[bControlIds[i]]];if(!(count.begin>0&&count.begin==count.end&&count.value>0))std::fprintf(stderr,"Page B GUI control %d id %d gestures %u/%u/%u\n",i,bControlIds[i],count.begin,count.value,count.end);
   CHECK(count.begin>0&&count.begin==count.end&&count.value>0,"each Page B control emits its own host automation gesture");
  }
  auto afterB=plain_state(a.save());for(int k=0;k<18;k++)CHECK(close_value(afterB[k],beforeB[k]),"Page B GUI controls leave all Page A settings unchanged");
  a.parameter(9,0);a.parameter(6,1);a.parameter(7,normalized(7,-3));
  CHECK(ok(view->removed()),"remove editor");view->setFrame(nullptr);view->release();DestroyWindow(parent);a.stop();std::printf("PASS native VST3 GUI embedding, all 39 GUI automation gestures, typed BPM, speed buttons, mode activation and XY capture cleanup\n");
 }
 factory->release();if(exitDll)CHECK(exitDll(),"VST3 ExitDll");FreeLibrary(module);std::printf("ALL VST3 INTEGRATION TESTS PASSED\n");return 0;
}
