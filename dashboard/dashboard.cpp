#include "ultimate_tracker.h"
#include <openvr.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <fstream>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t W=1024, H=900;
constexpr uint32_t BG=0xFF020306u, PANEL=0xFF04080Du, PANEL2=0xFF08101Au;
constexpr uint32_t WHITE=0xFFD0DAE4u, MUTED=0xFF60768Au, ACCENT=0xFF3979B0u;
constexpr uint32_t GREEN=0xFF58D68Du, AMBER=0xFFF5B544u, RED=0xFFFF6B6Bu;
std::vector<uint32_t> px(W*H, BG);
struct Row { std::string name; vive::TrackerData d{}; bool has=false; std::chrono::steady_clock::time_point lastUpdate{}; };
std::vector<Row> rows;
std::string banner="CONNECTING TO TRACKER SERVICE";
uint32_t bannerColor=MUTED;
bool serverSeen=false, dirty=true;
const auto dashboardStartedAt=std::chrono::steady_clock::now();
bool startupWarningShown=false;
int expandedTracker=-1; // actual SDK tracker ID, -1 means closed
vr::VROverlayHandle_t overlay=vr::k_ulOverlayHandleInvalid, thumb=vr::k_ulOverlayHandleInvalid;


struct GPURenderer {
    int fd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint texture = 0;
    std::vector<uint32_t> flipped;

    bool init() {
        fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if(fd < 0) { perror("GPU render node"); return false; }

        gbm = gbm_create_device(fd);
        if(!gbm) { std::cerr << "GBM creation failed\n"; return false; }

        auto getDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));

        if(!getDisplay) return false;

        display = getDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);

        EGLint major=0,minor=0;
        if(display==EGL_NO_DISPLAY ||
           !eglInitialize(display,&major,&minor)) {
            std::cerr << "EGL init failed: 0x"
                      << std::hex << eglGetError() << std::dec << "\n";
            return false;
        }

        EGLint attrs[] = {
            EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };

        EGLConfig config{};
        EGLint count=0;
        if(!eglChooseConfig(display,attrs,&config,1,&count) || !count)
            return false;

        if(!eglBindAPI(EGL_OPENGL_ES_API)) return false;

        EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        context = eglCreateContext(display,config,EGL_NO_CONTEXT,ctxAttrs);

        if(context==EGL_NO_CONTEXT ||
           !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))
            return false;

        std::cout << "GPU renderer: " << glGetString(GL_RENDERER) << "\n";

        glGenTextures(1,&texture);
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,
                     GL_RGBA,GL_UNSIGNED_BYTE,nullptr);

        flipped.resize((size_t)W*H);

        return glGetError()==GL_NO_ERROR;
    }

    void upload() {
        for(uint32_t y=0;y<H;++y)
            for(uint32_t x=0;x<W;++x) {
                uint32_t c=px[(size_t)y*W+x];
                flipped[(size_t)(H-1-y)*W+x] =
                    (c&0xFF00FF00u) |
                    ((c&0x00FF0000u)>>16) |
                    ((c&0x000000FFu)<<16);
            }

        glBindTexture(GL_TEXTURE_2D,texture);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,W,H,
                        GL_RGBA,GL_UNSIGNED_BYTE,flipped.data());
        glFinish();

        GLenum err=glGetError();
        if(err!=GL_NO_ERROR)
            std::cerr << "GPU upload error: 0x"
                      << std::hex << err << std::dec << "\n";
    }

    void shutdown() {
        if(texture) {
            glDeleteTextures(1,&texture);
            texture=0;
        }
        if(display!=EGL_NO_DISPLAY) {
            eglMakeCurrent(display,EGL_NO_SURFACE,
                           EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(context!=EGL_NO_CONTEXT)
                eglDestroyContext(display,context);
            eglTerminate(display);
        }
        if(gbm) gbm_device_destroy(gbm);
        if(fd>=0) close(fd);
    }
};


FT_Library ftLibrary = nullptr;
FT_Face ftFace = nullptr;

bool initFonts() {
  if(FT_Init_FreeType(&ftLibrary)) return false;
  if(FT_New_Face(ftLibrary,
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",0,&ftFace))
    return false;
  return true;
}

void shutdownFonts() {
  if(ftFace) FT_Done_Face(ftFace);
  if(ftLibrary) FT_Done_FreeType(ftLibrary);
}

void blendPixel(int x,int y,uint32_t color,uint8_t alpha) {
  if(x<0||y<0||x>=(int)W||y>=(int)H)return;

  uint32_t &dst=px[(size_t)y*W+x];

  auto blend=[alpha](uint32_t a,uint32_t b) {
    return (a*alpha+b*(255-alpha))/255;
  };

  uint32_t r=blend((color>>16)&255,(dst>>16)&255);
  uint32_t g=blend((color>>8)&255,(dst>>8)&255);
  uint32_t b=blend(color&255,dst&255);

  dst=0xFF000000u|(r<<16)|(g<<8)|b;
}

void smoothText(int x,int y,const std::string&str,
                uint32_t color,int size=22) {
  if(!ftFace)return;

  FT_Set_Pixel_Sizes(ftFace,0,size);
  int penX=x;

  for(unsigned char ch:str) {
    if(FT_Load_Char(ftFace,ch,FT_LOAD_RENDER))continue;

    FT_GlyphSlot g=ftFace->glyph;
    FT_Bitmap &bm=g->bitmap;

    for(unsigned int row=0;row<bm.rows;++row)
      for(unsigned int col=0;col<bm.width;++col) {
        int index=(int)row*bm.pitch+(int)col;
        blendPixel(penX+g->bitmap_left+(int)col,
                   y-g->bitmap_top+(int)row,
                   color,bm.buffer[index]);
      }

    penX+=g->advance.x>>6;
  }
}

void roundedRect(int x,int y,int w,int h,int radius,uint32_t color) {
  for(int yy=0;yy<h;++yy)
    for(int xx=0;xx<w;++xx) {
      int cx=std::clamp(xx,radius,w-radius-1);
      int cy=std::clamp(yy,radius,h-radius-1);
      int dx=xx-cx,dy=yy-cy;
      if(dx*dx+dy*dy<=radius*radius)
        blendPixel(x+xx,y+yy,color,255);
    }
}

void rect(int x,int y,int w,int h,uint32_t c) {
  int x0=std::max(0,x), y0=std::max(0,y), x1=std::min((int)W,x+w), y1=std::min((int)H,y+h);
  for(int yy=y0;yy<y1;++yy) std::fill(px.begin()+(size_t)yy*W+x0,px.begin()+(size_t)yy*W+x1,c);
}
const uint8_t* glyph(char ch) {
  static const uint8_t blank[7]={};
  static const uint8_t f[37][7]={
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},{0,0,0,31,0,0,0}};
  ch=(char)std::toupper((unsigned char)ch);
  if(ch>='A'&&ch<='Z') return f[ch-'A'];
  if(ch>='0'&&ch<='9') return f[26+ch-'0'];
  if(ch=='-') return f[36];
  return blank;
}
void text(int x,int y,const std::string&s,uint32_t c,int z=3) {
  int sx=x;
  for(char ch:s) {
    if(ch=='\n'){y+=9*z;x=sx;continue;}
    auto g=glyph(ch);
    for(int yy=0;yy<7;++yy) for(int xx=0;xx<5;++xx)
      if(g[yy]&(1u<<(4-xx))) rect(x+xx*z,y+yy*z,z,z,c);
    x+=6*z;
  }
}
std::string stateName(uint8_t s) {
  static const char* n[]={"UNPAIRED","PAIRING","PAIRED","CONNECTING","CONNECTED","INITIALIZED","SYNCING","BUILD MAP","READY"};
  return s<9?n[s]:"UNKNOWN";
}
uint32_t stateColor(uint8_t s) { return s==vive::Ready?GREEN:(s==vive::BuildMap||s==vive::Pairing||s==vive::Syncing?AMBER:(s<=vive::Connecting?RED:ACCENT)); }
// The SDK explicitly exposes RotationOnly; pose-state mask values are not
// documented in this header, so do not guess their meaning.
struct PoseLabel { const char* text; uint32_t color; };
PoseLabel trackerPoseLabel(const Row& r) {
  if(!r.has) return {"Awaiting data",MUTED};
  if(r.d.State!=vive::Ready) return {"Not tracking",MUTED};
  const auto now=std::chrono::steady_clock::now();
  if(r.lastUpdate.time_since_epoch().count()==0 ||
     now-r.lastUpdate>std::chrono::seconds(3))
    return {"Tracking lost",RED};
  if((r.d.Flags & vive::TrackerFlag_RotationOnly)!=0)
    return {"Rotation only",AMBER};
  return {"Full pose",GREEN};
}

void button(int x,int y,int w,int h,const std::string&s,uint32_t c) {
  rect(x,y,w,h,PANEL2);
  rect(x,y,4,h,c);
  text(x+std::max(12,(w-(int)s.size()*18)/2),
       y+(h-21)/2,s,WHITE,3);
}

int hoveredButton=-1;
int flashedButton=-1;
std::chrono::steady_clock::time_point flashUntil{};

int activeTab=0;
// OSC tracker settings are local to this overlay; slots map to OpenVR GenericTracker devices.
struct TrackerOscSlot { bool enabled=false; int role=0; };
constexpr const char* roleNames[]={"HIP","CHEST","LEFT FOOT","RIGHT FOOT","LEFT KNEE","RIGHT KNEE","LEFT ELBOW","RIGHT ELBOW"};
constexpr int roleIds[]={1,2,3,4,5,6,7,8};
std::array<TrackerOscSlot,5> trackerOsc{{{true,0},{true,2},{true,3},{false,4},{false,5}}};
bool trackerOscEnabled=false, trackerHeadEnabled=false;
int trackerRate=60;
std::array<bool,3> flipRotation{{false,false,false}};
int trackerVisible=0, trackerSent=0;
std::chrono::steady_clock::time_point lastTrackerSend{};
void saveTrackerSettings(){
  const char* home=getenv("HOME");if(!home)return;
  std::ofstream f(std::string(home)+"/.vive-ultimate-osc.conf");
  if(!f)return;
  f<<trackerOscEnabled<<' '<<trackerHeadEnabled<<' '<<trackerRate<<'\n';
  for(auto& t:trackerOsc)f<<t.enabled<<' '<<t.role<<'\n';
  f<<flipRotation[0]<<' '<<flipRotation[1]<<' '<<flipRotation[2]<<'\n';
}
void loadTrackerSettings(){
  const char* home=getenv("HOME");if(!home)return;
  std::ifstream f(std::string(home)+"/.vive-ultimate-osc.conf");
  int master=0,head=0,rate=60;
  if(!(f>>master>>head>>rate))return;
  trackerOscEnabled=master!=0;trackerHeadEnabled=head!=0;
  trackerRate=(rate==30||rate==60||rate==90)?rate:60;
  for(auto& t:trackerOsc){int en=0,role=0;if(!(f>>en>>role))break;t.enabled=en!=0;t.role=std::clamp(role,0,7);}
  int fx=0,fy=0,fz=0;
  if(f>>fx>>fy>>fz){flipRotation={{fx!=0,fy!=0,fz!=0}};}
}

int buttonAt(float x,float y){
  if(x>=928 && x<984 && y>=35 && y<91)return 4;
  if(activeTab==1)return -1;
  if(y<790||y>=850)return -1;
  if(x>=40&&x<258)return 0;
  if(x>=274&&x<492)return 1;
  if(x>=508&&x<726)return 2;
  if(x>=742&&x<984)return 3;
  return -1;
}

// FRAME_OSC_TAB_V1 -- localhost-only VRChat OSC, no external dependencies.
int oscSocket=-1;
unsigned long oscPackets=0;
std::string oscStatus="NOT LISTENING", oscLast="No packets received";
std::chrono::steady_clock::time_point oscLastAt{};

std::string oscReadString(const unsigned char* data,size_t n,size_t& offset){
  if(offset>=n)return {};
  size_t start=offset;
  while(offset<n && data[offset])++offset;
  if(offset>=n)return {};
  std::string out((const char*)data+start,offset-start);
  offset=(offset+4)&~size_t(3);
  return out;
}
void oscInit(){
  oscSocket=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
  if(oscSocket<0){oscStatus="SOCKET ERROR";return;}
  sockaddr_in addr{};addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(9001);
  if(bind(oscSocket,(sockaddr*)&addr,sizeof(addr))<0){
    oscStatus="PORT 9001 BUSY";close(oscSocket);oscSocket=-1;return;
  }
  oscStatus="LISTENING ON 127.0.0.1:9001";
}
void oscPoll(){
  if(oscSocket<0)return;
  unsigned char data[4096];
  for(int i=0;i<32;++i){
    sockaddr_in from{};socklen_t fromLen=sizeof(from);
    ssize_t n=recvfrom(oscSocket,data,sizeof(data),0,(sockaddr*)&from,&fromLen);
    if(n<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK)oscStatus="RECEIVE ERROR";break;}
    if(from.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))continue;
    size_t offset=0;
    std::string address=oscReadString(data,(size_t)n,offset);
    std::string types=oscReadString(data,(size_t)n,offset);
    if(address.empty()||address[0]!='/')continue;
    oscLast=address;
    if(types.size()>1)oscLast+=" ["+types.substr(1,24)+"]";
    if(oscLast.size()>70)oscLast.resize(70);
    ++oscPackets;oscLastAt=std::chrono::steady_clock::now();dirty=true;
  }
}
void oscWriteString(std::vector<unsigned char>& out,const std::string& value){
  out.insert(out.end(),value.begin(),value.end());out.push_back(0);
  while(out.size()%4)out.push_back(0);
}
void oscSendTest(){
  // /chatbox/input ,sT = message string + send-immediately boolean true.
  std::vector<unsigned char> packet;
  oscWriteString(packet,"/chatbox/input");
  oscWriteString(packet,",sT");
  oscWriteString(packet,"Steam Frame OSC test");
  int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
  if(fd<0){oscStatus="SEND SOCKET ERROR";dirty=true;return;}
  sockaddr_in dest{};dest.sin_family=AF_INET;
  dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dest.sin_port=htons(9000);
  ssize_t sent=sendto(fd,packet.data(),packet.size(),0,(sockaddr*)&dest,sizeof(dest));
  close(fd);
  oscStatus=sent==(ssize_t)packet.size()?"TEST SENT TO 127.0.0.1:9000":"SEND FAILED";
  dirty=true;
}
// OSC float payloads are big-endian IEEE-754; no third-party OSC library needed.
void oscWriteFloat(std::vector<unsigned char>& out,float f){
  uint32_t u;static_assert(sizeof(u)==sizeof(f),"float size");std::memcpy(&u,&f,4);
  for(int k=3;k>=0;--k)out.push_back((unsigned char)(u>>(k*8)));
}
void oscSendVec(int fd,int id,const char* kind,float x,float y,float z){
  std::vector<unsigned char> p;
  oscWriteString(p,"/tracking/trackers/"+std::to_string(id)+"/"+kind);
  oscWriteString(p,",fff");oscWriteFloat(p,x);oscWriteFloat(p,y);oscWriteFloat(p,z);
  sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_port=htons(9000);
  dst.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  sendto(fd,p.data(),p.size(),0,(sockaddr*)&dst,sizeof(dst));
}
// OpenVR's right-handed tracking space -> Unity's left-handed space (flip Z).
// Euler Z-X-Y decomposition in degrees; gimbal-lock fallback is deterministic.
void steamVRToUnityEuler(const vr::HmdMatrix34_t& m,float& ex,float& ey,float& ez){
  // Unity Euler order Z-X-Y: R = Ry(y) * Rx(x) * Rz(z).
  // Convert OpenVR right-handed basis to Unity left-handed basis with S*R*S,
  // where S = diag(1,1,-1).
  const float r00=m.m[0][0], r02=-m.m[0][2];
  const float r10=m.m[1][0], r11=m.m[1][1], r12=-m.m[1][2];
  const float r20=-m.m[2][0], r22=m.m[2][2];
  const float x=std::asin(std::clamp(-r12,-1.0f,1.0f));
  float y,z;
  if(std::abs(std::cos(x))>0.0001f){
    y=std::atan2(r02,r22);
    z=std::atan2(r10,r11);
  } else {
    // Gimbal lock: Y and Z are not independently recoverable.
    y=std::atan2(-r20,r00);
    z=0.0f;
  }
  constexpr float deg=57.29577951308232f;
  ex=x*deg;ey=y*deg;ez=z*deg;
}
void trackerOscTick(vr::IVRSystem* sys){
  if(!sys)return;
  auto now=std::chrono::steady_clock::now();
  if(lastTrackerSend.time_since_epoch().count() &&
     now-lastTrackerSend<std::chrono::microseconds(1000000/trackerRate))return;
  lastTrackerSend=now;
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
  sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,0,poses,vr::k_unMaxTrackedDeviceCount);
  std::array<int,5> devices{};devices.fill(-1);
  int found=0;
  for(uint32_t i=0;i<vr::k_unMaxTrackedDeviceCount;++i){
    if(sys->GetTrackedDeviceClass(i)!=vr::TrackedDeviceClass_GenericTracker)continue;
    if(found<5)devices[found]=int(i);
    ++found;
  }
  int active=0;
  int fd=-1;
  if(trackerOscEnabled)fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
  for(int j=0;j<5;++j){
    if(devices[j]<0||!poses[devices[j]].bPoseIsValid||!poses[devices[j]].bDeviceIsConnected)continue;
    ++active;
    if(fd<0||!trackerOsc[j].enabled)continue;
    // Prevent multiple trackers from driving the same OSC role.
    bool duplicate=false;
    for(int k=0;k<j;++k)if(trackerOsc[k].enabled&&trackerOsc[k].role==trackerOsc[j].role)duplicate=true;
    if(duplicate)continue;
    auto& m=poses[devices[j]].mDeviceToAbsoluteTracking;
    float rx,ry,rz;steamVRToUnityEuler(m,rx,ry,rz);
    if(flipRotation[0])rx=-rx;
    if(flipRotation[1])ry=-ry;
    if(flipRotation[2])rz=-rz;
    int role=roleIds[trackerOsc[j].role];
    oscSendVec(fd,role,"position",m.m[0][3],m.m[1][3],-m.m[2][3]);
    oscSendVec(fd,role,"rotation",rx,ry,rz);
  }
  if(fd>=0)close(fd);
  if(found!=trackerVisible||active!=trackerSent){trackerVisible=found;trackerSent=active;dirty=true;}
}
void drawTabs(){
  roundedRect(40,135,190,42,9,activeTab==0?0xFF245A88u:PANEL2);
  roundedRect(244,135,190,42,9,activeTab==1?0xFF245A88u:PANEL2);
  smoothText(94,163,"Trackers",WHITE,19);
  smoothText(316,163,"OSC",WHITE,19);
}
void renderOSC(){
  smoothText(46,206,"VRCHAT OSC / FULL BODY TRACKING",WHITE,25);
  roundedRect(40,223,944,85,10,PANEL);
  smoothText(58,252,"OSC streaming",WHITE,21);
  smoothText(58,282,"OpenVR devices: "+std::to_string(trackerVisible)+"   Valid poses: "+std::to_string(trackerSent),MUTED,17);
  roundedRect(770,237,190,54,9,trackerOscEnabled?0xFF174D38u:PANEL2);
  smoothText(838,271,trackerOscEnabled?"ON":"OFF",trackerOscEnabled?GREEN:AMBER,22);
  smoothText(58,340,"TRACKER",MUTED,16);
  smoothText(450,340,"BODY ASSIGNMENT - TAP TO CHANGE",MUTED,16);
  smoothText(850,340,"SEND",MUTED,16);
  for(int i=0;i<5;++i){
    int y=350+i*72;
    roundedRect(40,y,944,64,9,PANEL);
    smoothText(58,y+40,"Tracker "+std::to_string(i+1),WHITE,20);
    roundedRect(438,y+9,363,47,8,PANEL2);
    smoothText(457,y+39,roleNames[trackerOsc[i].role],ACCENT,19);
    roundedRect(830,y+9,128,47,8,trackerOsc[i].enabled?0xFF174D38u:PANEL2);
    smoothText(864,y+39,trackerOsc[i].enabled?"ON":"OFF",trackerOsc[i].enabled?GREEN:AMBER,19);
  }
  roundedRect(40,722,270,52,8,PANEL2);
  smoothText(60,754,"RATE: "+std::to_string(trackerRate)+" HZ",WHITE,19);
  roundedRect(615,722,344,52,8,PANEL2);
  smoothText(630,754,"Send chatbox test",WHITE,19);
  smoothText(46,800,"FLIP ROTATION AXIS",MUTED,17);
  for(int i=0;i<3;++i){
    int x=355+i*211;
    roundedRect(x,783,194,55,9,flipRotation[i]?0xFF174D38u:PANEL2);
    smoothText(x+20,818,std::string("FLIP ")+"XYZ"[i]+(flipRotation[i]?"  ON":"  OFF"),flipRotation[i]?GREEN:WHITE,19);
  }
  smoothText(46,864,"OSC TX 127.0.0.1:9000  /  Only valid tracker poses are sent",MUTED,16);
}

void drawActionButton(int index,int x,int w){
  bool hover=hoveredButton==index;
  bool flash=flashedButton==index &&
    std::chrono::steady_clock::now()<flashUntil;

  uint32_t fill=PANEL2;
  if(hover)fill=0xFF142C44u;
  if(flash)fill=0xFF245A88u;
  if(index==2)fill=hover?0xFF303842u:0xFF262C34u;

  if(index==2) {
    if(hover) roundedRect(x-2,788,w+4,64,12,0xFF48525Du);
  } else if(flash)
    roundedRect(x-3,787,w+6,66,12,0xFF438FCEu);
  else if(hover)
    roundedRect(x-2,788,w+4,64,12,0xFF254B6Du);

  roundedRect(x,790,w,60,10,fill);

  if(flash)
    rect(x+12,796,w-24,3,0xFF73BDF2u);
}

// Clickable GitHub watermark, drawn on both dashboard tabs.
constexpr int GITHUB_X0=766, GITHUB_Y0=867, GITHUB_X1=985, GITHUB_Y1=899;
void drawGithubWatermark() {
  // The overlay uses 1024x900 pixel coordinates.
  smoothText(781,889,"RayneVR  -  GitHub",MUTED,16);
  rect(781,894,174,1,MUTED);
}
void openGithubProfile() {
  // Open the real profile, not a placeholder repository URL.
  // xdg-open may be unavailable in SteamVR's environment; fail without
  // interrupting tracker controls.
  pid_t pid=fork();
  if(pid==0) {
    execlp("xdg-open","xdg-open","https://github.com/RayneVR",(char*)nullptr);
    _exit(127);
  }
  if(pid>0) {
    std::thread([pid](){ int status=0; waitpid(pid,&status,0); }).detach();
  }
}

void render() {
  std::fill(px.begin(),px.end(),BG);

  // Header
  smoothText(44,66,"VIVE Ultimate Manager",WHITE,32);
  smoothText(46,94,"Tracker Settings",MUTED,19);

  roundedRect(720,37,190,44,12,PANEL2);
  smoothText(735,66,"5 TRACKER SLOTS",ACCENT,17);

  rect(40,115,944,1,PANEL2);

  // Close button
  uint32_t closeFill = PANEL2;
  if(hoveredButton==4) closeFill=0xFF142C44u;
  roundedRect(928,35,56,56,10,closeFill);
  smoothText(947,72,"X",WHITE,28);

  drawTabs();
  if(activeTab==1){renderOSC();drawGithubWatermark();dirty=false;return;}
  // Tracker activity is observable; dongle USB status is not exposed by this SDK.
  size_t ready=0;
  bool haveTrackerData=false;
  for(const auto&r:rows) {
    if(r.has) {
      haveTrackerData=true;
      if(r.d.State==vive::Ready) ++ready;
    }
  }
  const bool startupTimedOut =
      std::chrono::steady_clock::now()-dashboardStartedAt > std::chrono::seconds(15);
  const bool serviceUnavailable = !serverSeen && !haveTrackerData && startupTimedOut;
  const char* linkStatus = ready>0 ? "Active" :
      haveTrackerData ? "Trackers found" :
      serverSeen ? "Waiting" :
      serviceUnavailable ? "Unavailable" : "Starting...";
  const uint32_t linkColor = (ready>0 || haveTrackerData) ? GREEN :
      serviceUnavailable ? RED : AMBER;
  smoothText(46,213,"Tracker link",WHITE,23);
  smoothText(770,213,linkStatus,linkColor,20);

  smoothText(46,235,
      serviceUnavailable ? "Make sure trackers are on and paired" :
      "Ready trackers",
      serviceUnavailable ? AMBER : MUTED,17);

  if(serviceUnavailable) {
    smoothText(46,257,
      "Check dongle, then restart SteamVR",RED,16);
  }
  smoothText(878,251,std::to_string(ready)+" / 5",WHITE,20);

  rect(40,270,944,1,PANEL2);

  smoothText(46,297,"DEVICES",MUTED,17);

  // Five tracker slots
  for(size_t i=0;i<5;++i) {
    int y=309+(int)i*87;
    bool occupied=i<rows.size();
    const Row* r=occupied?&rows[i]:nullptr;

    roundedRect(40,y,944,78,12,
      (occupied && expandedTracker==(int)r->d.Id) ? PANEL2 : PANEL);

    // Tracker icon background
    roundedRect(56,y+12,54,54,11,PANEL2);
    smoothText(74,y+47,"T",ACCENT,25);

    std::string number=std::to_string(i+1);
    if(number.size()<2) number="0"+number;

    smoothText(130,y+32,"Tracker "+number,WHITE,24);

    if(occupied) {
      smoothText(130,y+59,"SN: "+r->name,MUTED,17);
    } else {
      smoothText(130,y+59,"Not connected",MUTED,17);
    }

    std::string status="-";
    uint32_t statusColor=MUTED;

    if(occupied) {
      status=r->has?stateName((uint8_t)r->d.State):"Discovered";
      statusColor=r->has?stateColor((uint8_t)r->d.State):MUTED;
    }

    smoothText(645,y+32,status,statusColor,19);
    if(occupied) {
      const auto pose=trackerPoseLabel(*r);
      smoothText(645,y+59,pose.text,pose.color,16);
    }


    if(occupied&&r->has) {
      smoothText(833,y+32,
        std::to_string((int)r->d.Battery)+"%",WHITE,20);
    } else {
      smoothText(833,y+32,"--",MUTED,20);
    }

    smoothText(948,y+49,
      (occupied && expandedTracker==(int)r->d.Id) ? "v" : "v",
      occupied ? ACCENT : MUTED,24);
  }

  // Management actions
  rect(40,752,944,1,PANEL2);
  smoothText(46,780,"TRACKER MANAGEMENT",MUTED,17);

  drawActionButton(0,40,218);
  drawActionButton(1,274,218);
  drawActionButton(2,508,218);
  drawActionButton(3,742,242);

  smoothText(91,828,"Pair",WHITE,23);
  smoothText(320,828,"Unpair",WHITE,23);
  smoothText(hoveredButton==2?548:545,828,
    hoveredButton==2?"In progress":"Room setup",MUTED,21);
  smoothText(812,828,"Refresh",WHITE,23);

  // Floating tracker actions are rendered last, above the other controls.
  auto expandedIt=std::find_if(rows.begin(),rows.end(),[](const Row& r){
    return expandedTracker>=0 && (int)r.d.Id==expandedTracker;
  });
  if(expandedIt!=rows.end()) {
    const int index=(int)std::distance(rows.begin(),expandedIt);
    if(index<5) {
      const int menuX=650;
      const int menuY=std::min(309+index*87+64, 900-116);
      roundedRect(menuX,menuY,330,112,12,PANEL2);
      roundedRect(menuX+6,menuY+6,318,47,9,PANEL);
      roundedRect(menuX+6,menuY+59,318,47,9,PANEL);
      smoothText(menuX+22,menuY+37,"Identify tracker",WHITE,20);
      smoothText(menuX+22,menuY+90,"Turn off tracker",RED,20);
    }
  }

  drawGithubWatermark();
  dirty=false;
}
void say(std::string s,uint32_t c=MUTED){banner=std::move(s);bannerColor=c;dirty=true;}

class Client final:public vive::ITrackerIPCClient {
 public:
  void ServerStart() override {serverSeen=true;say("TRACKER SERVER IS ONLINE",GREEN);}
  void ServerStop() override {serverSeen=false;say("TRACKER SERVER STOPPED",RED);}
  void Add(uint32_t id,char const*n) override {
    auto it=std::find_if(rows.begin(),rows.end(),[id](const Row&r){return r.d.Id==id;});
    if(it==rows.end()){Row r;r.name=n?n:("TRACKER "+std::to_string(id));r.d.Id=id;rows.push_back(r);}
    else if(n)it->name=n;
    dirty=true;
  }
  void Update(vive::TrackerData const&d) override {
    auto it=std::find_if(rows.begin(),rows.end(),[&d](const Row&r){return r.d.Id==d.Id;});
    if(it==rows.end()){Row r;r.name="TRACKER "+std::to_string(d.Id);r.d=d;r.has=true;r.lastUpdate=std::chrono::steady_clock::now();rows.push_back(r);}
    else {it->d=d;it->has=true;it->lastUpdate=std::chrono::steady_clock::now();}
    dirty=true;
  }
  void Click(uint32_t,int,int) override {}
  void Remove(uint32_t id) override {
    rows.erase(std::remove_if(rows.begin(),rows.end(),[id](const Row&r){return (r.has&&r.d.Id==id)||r.name=="TRACKER "+std::to_string(id);}),rows.end());
    dirty=true;
  }
  void CommandResponse(vive::TrackerCommandResponse const&r) override {
    std::string c=r.Command?r.Command:"COMMAND", result=r.Result?r.Result:"NO RESPONSE TEXT";
    say(c+": "+result,GREEN);
    if(r.TrackerStates) for(uint32_t i=0;i<r.NumTrackers;++i)
      for(auto&row:rows) if(row.has&&row.d.Id==i) row.d.State=r.TrackerStates[i];
    dirty=true;
  }
};
void command(Client&c,const char*s){
  bool ok = c.SendCommand(s);
  if(ok) say(std::string("SENT: ")+s,ACCENT);
  else say(std::string("FAILED TO SEND: ")+s,RED);
}
void click(Client&c,float x,float y){
  if(x>=GITHUB_X0 && x<GITHUB_X1 && y>=GITHUB_Y0 && y<GITHUB_Y1) {
    openGithubProfile();
    return;
  }
  if(y>=135&&y<177){
    if(x>=40&&x<230){activeTab=0;dirty=true;return;}
    if(x>=244&&x<434){activeTab=1;dirty=true;return;}
  }
  if(activeTab==1){
    expandedTracker=-1;
    bool changed=false;
    if(x>=770&&x<960&&y>=237&&y<291){trackerOscEnabled=!trackerOscEnabled;changed=true;}
    for(int i=0;i<5;++i){
      int rowY=350+i*72;
      if(y>=rowY+9&&y<rowY+56){
        if(x>=438&&x<801){trackerOsc[i].role=(trackerOsc[i].role+1)%8;changed=true;}
        else if(x>=830&&x<958){trackerOsc[i].enabled=!trackerOsc[i].enabled;changed=true;}
      }
    }
    if(x>=40&&x<310&&y>=722&&y<774){trackerRate=trackerRate==30?60:(trackerRate==60?90:30);changed=true;}
    if(x>=615&&x<959&&y>=722&&y<774)oscSendTest();
    if(y>=783&&y<838){
      for(int i=0;i<3;++i){int bx=355+i*211;
        if(x>=bx&&x<bx+194){flipRotation[i]=!flipRotation[i];changed=true;break;}
      }
    }
    if(changed){saveTrackerSettings();dirty=true;}
    return;
  }
  // Floating menu takes precedence over rows and management buttons.
  auto selected=std::find_if(rows.begin(),rows.end(),[](const Row& r){
    return expandedTracker>=0 && (int)r.d.Id==expandedTracker;
  });
  if(selected!=rows.end()) {
    int index=(int)std::distance(rows.begin(),selected);
    if(index<5) {
      int menuY=std::min(309+index*87+64, 900-116);
      if(x>=650&&x<980&&y>=menuY&&y<menuY+112) {
        uint32_t id=selected->d.Id;
        if(y<menuY+56) {
          std::string cmd="Echo "+std::to_string(id);
          if(c.SendCommand(cmd.c_str()))
            say("ECHO SENT TO TRACKER ID "+std::to_string(id)+" (PHYSICAL ALERT UNCONFIRMED)",AMBER);
          else say("FAILED TO SEND: "+cmd,RED);
        } else {
          std::string cmd="PowerOff "+std::to_string(id);
          command(c,cmd.c_str());
        }
        expandedTracker=-1;
        dirty=true;
        return;
      }
    }
  }
  for(size_t i=0;i<rows.size()&&i<5;++i) {
    int rowY=309+(int)i*87;
    if(x>=40&&x<984&&y>=rowY&&y<rowY+78) {
      int id=(int)rows[i].d.Id;
      expandedTracker=(expandedTracker==id ? -1 : id);
      dirty=true;
      return;
    }
  }
  if(expandedTracker!=-1){expandedTracker=-1;dirty=true;return;}
  if(y<790||y>=850)return;
  if(x>=40&&x<258)command(c,"Pairing all");
  else if(x>=274&&x<492)command(c,"Unpair all");
  else if(x>=508&&x<726){ /* Room setup coming soon: intentionally disabled. */ }
  else if(x>=742&&x<984)command(c,"Echo");
}
}

int main(){
  if(!initFonts()) {
    std::cerr << "FreeType initialization failed\\n";
    return 1;
  }

  GPURenderer gpu;
  if(!gpu.init()){
    std::cerr << "GPU initialization failed\n";
    gpu.shutdown();
    return 1;
  }

  vr::EVRInitError e=vr::VRInitError_None;
  vr::IVRSystem* sys=vr::VR_Init(&e,vr::VRApplication_Overlay);
  if(!sys||e!=vr::VRInitError_None){std::cerr<<"VR_Init failed: "<<vr::VR_GetVRInitErrorAsEnglishDescription(e)<<"\n";return 2;}
  vr::IVROverlay* ov=vr::VROverlay();
  if(!ov){std::cerr<<"IVROverlay unavailable\n";vr::VR_Shutdown();return 3;}
  auto err=ov->CreateDashboardOverlay("vive_ultimate_tracker_dashboard","VIVE Ultimate Manager",&overlay,&thumb);
  if(err!=vr::VROverlayError_None){std::cerr<<"CreateDashboardOverlay failed: "<<(int)err<<"\n";vr::VR_Shutdown();return 4;}
  auto iconError = ov->SetOverlayFromFile(
      thumb,
      "/home/steamos/vive-ultimate/resources/icons/tracker_status_off@2x.png"
  );

  std::cout << "Dashboard icon: "
            << ov->GetOverlayErrorNameFromEnum(iconError)
            << std::endl;

  ov->SetOverlayInputMethod(overlay,vr::VROverlayInputMethod_Mouse);
  vr::HmdVector2_t mouseScale = {{1024.0f, 900.0f}};
  ov->SetOverlayMouseScale(overlay, &mouseScale);
  ov->SetOverlayWidthInMeters(overlay,1.35f);
  ov->SetOverlayAlpha(overlay,1.0f);
  Client client;
  loadTrackerSettings();
  oscInit();
  render();
  gpu.upload();

  vr::Texture_t gpuTexture{};
  gpuTexture.handle =
      reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.texture));
  gpuTexture.eType=vr::TextureType_OpenGL;
  gpuTexture.eColorSpace=vr::ColorSpace_Gamma;

  auto textureError=ov->SetOverlayTexture(overlay,&gpuTexture);

  std::cout << "GPU overlay submission: "
            << ov->GetOverlayErrorNameFromEnum(textureError)
            << std::endl;

  if(textureError!=vr::VROverlayError_None){
    ov->DestroyOverlay(overlay);
    ov->DestroyOverlay(thumb);
    vr::VR_Shutdown();
    gpu.shutdown();
    return 5;
  }
  std::cout<<"Dashboard ready. Open SteamVR Dashboard > VIVE Ultimate.\n";
  bool running=true;
  auto last=std::chrono::steady_clock::now();
  while(running){
    client.Poll();
    static auto lastPoseRefresh=std::chrono::steady_clock::now();
    if(std::chrono::steady_clock::now()-lastPoseRefresh>std::chrono::seconds(1)) {
      lastPoseRefresh=std::chrono::steady_clock::now();
      if(!rows.empty()) dirty=true;
    }

    if (!startupWarningShown && !serverSeen && rows.empty() &&
        std::chrono::steady_clock::now()-dashboardStartedAt > std::chrono::seconds(15)) {
      startupWarningShown=true;
      dirty=true;
    }
    oscPoll();
    trackerOscTick(sys);
    vr::VREvent_t ev{};
    while(ov->PollNextOverlayEvent(overlay,&ev,sizeof(ev))){
      if(ev.eventType==vr::VREvent_MouseMove){
        int next=buttonAt(ev.data.mouse.x,900.0f-ev.data.mouse.y);
        if(next!=hoveredButton){
          if(next>=0 && next!=2){
            auto hapticError=ov->TriggerLaserMouseHapticVibration(
              overlay,
              0.004f,  // 4 ms
              200.0f,  // frequency
              0.01f    // 1% amplitude
            );
            if(hapticError!=vr::VROverlayError_None){
              std::cerr << "Hover haptic error: "
                        << (int)hapticError << std::endl;
            }
          }
          hoveredButton=next;
          dirty=true;
        }
      }
      if(ev.eventType==vr::VREvent_FocusLeave){
        hoveredButton=-1;
        dirty=true;
      }
      if(ev.eventType==vr::VREvent_MouseButtonDown &&
         ev.data.mouse.button==vr::VRMouseButton_Left){
        int pressed=buttonAt(ev.data.mouse.x,900.0f-ev.data.mouse.y);
        if(pressed>=0 && pressed!=2){
          hoveredButton=pressed;
          flashedButton=pressed;
          flashUntil=std::chrono::steady_clock::now()+
                     std::chrono::milliseconds(220);
          dirty=true;
        }
      }


      if(ev.eventType==vr::VREvent_Quit)running=false;

    if(ev.eventType==vr::VREvent_MouseButtonDown &&
         ev.data.mouse.button==vr::VRMouseButton_Left) {
        float x = ev.data.mouse.x;
        float y = 900.0f - ev.data.mouse.y;




        if(buttonAt(x,y)==4) {
          running=false;
        } else {
          click(client,x,y);
        }
      }
    }
    auto now=std::chrono::steady_clock::now();
    if(flashedButton>=0){
      if(now<flashUntil){
        dirty=true;
      }else{
        flashedButton=-1;
        dirty=true;
      }
    }

    if(dirty && now-last>=std::chrono::milliseconds(100)){
      render();
      gpu.upload();

      auto updateError=ov->SetOverlayTexture(overlay,&gpuTexture);
      if(updateError!=vr::VROverlayError_None)
        std::cerr << "GPU refresh failed: "
                  << ov->GetOverlayErrorNameFromEnum(updateError)
                  << std::endl;

      last=now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  ov->ClearOverlayTexture(overlay);
  ov->DestroyOverlay(overlay);
  ov->DestroyOverlay(thumb);
  vr::VR_Shutdown();
  if(oscSocket>=0)close(oscSocket);
  gpu.shutdown();
  return 0;
}
