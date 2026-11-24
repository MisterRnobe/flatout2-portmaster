#pragma once
/* Only boot movies are optional; menu and race video players keep their state. */
static inline int fo2_video_path_is(const char* path,const char* expected){
    if(!path)return 0;
    for(;*path&&*expected;++path,++expected){unsigned char c=(unsigned char)*path;if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';if(c!=(unsigned char)*expected)return 0;}
    return !*path&&!*expected;
}
static inline int fo2_video_skip_startup(const char* path,int logos,int intro){
    return (logos&&fo2_video_path_is(path,"data/video/nvidia.avi"))||(intro&&fo2_video_path_is(path,"data/video/introinl.avi"));
}
