#import <AppKit/AppKit.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/clonefile.h>
namespace fs=std::filesystem;
static void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static void env(const char* key,const std::string& value){require(setenv(key,value.c_str(),1)==0,"Cannot set launch environment");}
int main(int argc,char** argv){@autoreleasepool{try{
  NSString* resourcePath=NSBundle.mainBundle.resourcePath;
  require(resourcePath!=nil,"Cannot locate application resources");
  fs::path resources=resourcePath.fileSystemRepresentation;
  fs::path engine=resources.parent_path()/"MacOS/fo2";
  require(fs::is_regular_file(engine),"The game executable is missing. Extract the complete application again.");
  const char* overridePath=std::getenv("FO2_USER_DATA_DIR");
  fs::path home=overridePath&&*overridePath?fs::path(overridePath):fs::path([NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES).firstObject fileSystemRepresentation])/"FlatOut2Metal";
  require(home.is_absolute(),"FO2_USER_DATA_DIR must be an absolute directory");
  fs::path game=home/"gamedata",saves=game/"Savegame";
  fs::create_directories(saves);
  for(const char* name:{"FlatOut2.exe","d3dx9_30.dll","filesystem","data.bfs","zoom_platform.bfs"}){
    auto source=resources/"gamedata"/name,target=game/name;
    require(fs::is_regular_file(source),std::string("Missing bundled game file: ")+name);
    // The guest filesystem intentionally rejects symlinks escaping its root.
    // Prepare owned regular files, using APFS copy-on-write where available.
    if(fs::is_symlink(target))fs::remove(target);
    if(!fs::exists(target)&&clonefile(source.c_str(),target.c_str(),0)!=0)fs::copy_file(source,target);
    require(fs::is_regular_file(target),std::string("Invalid game data path: ")+target.string());
  }
  if(!fs::exists(saves/"device.cfg"))fs::copy_file(resources/"defaults/device.cfg",saves/"device.cfg");
  NSDateFormatter* formatter=[NSDateFormatter new];formatter.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
  formatter.timeZone=[NSTimeZone timeZoneForSecondsFromGMT:0];formatter.dateFormat=@"yyyyMMdd'T'HHmmss'Z'";
  fs::path session=home/"logs"/(std::string([formatter stringFromDate:[NSDate date]].UTF8String)+"-"+std::to_string(getpid()));
  fs::create_directories(session);
  // Explicit defaults keep developer test/automation state out of this app.
  for(const char* key:{"FO2_TEST_LEVEL","FO2_SHADER_CAPTURE","FO2_PARTICLE_CAPTURE","FO2_HUD_TRACE","FO2_DEBUG_STATUS","FO2_NET_TRACE_DIR","FO2_RT_SCENE_DIR","FO2_AUTOPRESS","NFSU2_KEY_SCRIPT","FO2_AUTORESET","FO2_AUTODRIVE","FO2_GAME_DIR"})unsetenv(key);
  for(const char* key:{"FO2_RT_LIVE_DIAGNOSTIC","FO2_RT_GEOMETRY_CAPTURE","FO2_NET_TRACE","FO2_STARTUP_TRACE","MTL_DEBUG_LAYER","MTL_SHADER_VALIDATION","MTL_SHADER_VALIDATION_ABORT_ON_FAULT"})env(key,"0");
  env("FO2_TEST_SCENARIO","manual");env("FO2_DEBUG_VIEW","ssr");env("FO2_SSR_MATERIALS","1");env("FO2_SSR_CAPTURE","0");
  env("FO2_SKIP_LOGO","1");env("FO2_SKIP_INTRO","1");env("NFSU2_LANG","Russian");env("NFSU2_SHOW_FPS","1");
  env("NFSU2_SCREEN","1280x800");env("FO2_RENDER","1280x800");env("FO2_METAL_SESSION_DIR",session.string());
  std::ofstream config(session/"launch-info.txt");config<<"renderer=metal\nssr=on; F8 toggles\nssr_capture=0\nrt_updates=0\ngame_dir="<<game<<"\nresources="<<resources<<"\n";config.close();
  bool prepareOnly=argc>1&&std::string(argv[1])=="--prepare-only";
  if(prepareOnly){std::printf("Prepared FlatOut 2 data: %s\nSession: %s\n",home.c_str(),session.c_str());return 0;}
  require(std::freopen((session/"play.log").c_str(),"a",stdout)!=nullptr,"Cannot open game log");
  require(dup2(fileno(stdout),fileno(stderr))>=0,"Cannot redirect game errors");
  std::setvbuf(stdout,nullptr,_IOLBF,0);std::setvbuf(stderr,nullptr,_IOLBF,0);
  require(chdir(game.c_str())==0,"Cannot open game data directory");
  std::vector<std::string> arguments={engine.string(),game.string(),"--windowed","--cap60"};
  for(int i=1;i<argc;++i)if(std::string(argv[i]).rfind("-psn_",0)!=0)arguments.emplace_back(argv[i]);
  std::vector<char*> pointers;for(auto& s:arguments)pointers.push_back(s.data());pointers.push_back(nullptr);
  execv(engine.c_str(),pointers.data());throw std::runtime_error("Cannot start the game executable");
}catch(const std::exception& e){
  std::fprintf(stderr,"FlatOut 2: %s\n",e.what());
  if(!std::getenv("FO2_APP_HEADLESS")){[NSApplication sharedApplication];NSAlert* alert=[NSAlert new];alert.messageText=@"FlatOut 2 could not start";alert.informativeText=[NSString stringWithUTF8String:e.what()];[alert addButtonWithTitle:@"OK"];[alert runModal];}
  return 1;
}}}
