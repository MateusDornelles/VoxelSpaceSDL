#include "sprite_renderer.h"
#include "defines.h"
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef USE_APNG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#endif
#ifdef USE_OPENGL_RENDER
#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>
#include <stddef.h>
#endif

typedef struct { Uint32 *pixels; float durationMs; } SpriteFrame;
typedef struct { SpriteFrame *frames; char *path; int ownsFrames,frameCount,pixelWidth,pixelHeight,currentFrame; float x,y,z,width,height,elapsedMs; } WorldSprite;
static struct { WorldSprite *items; int count,capacity;
#ifdef USE_OPENGL_RENDER
GLuint program,vao,vbo,texture;const Uint32*uploadedPixels;int uploadedWidth,uploadedHeight;
#endif
} sprites;

static void FreeSprite(WorldSprite*s){if(s->ownsFrames){for(int i=0;i<s->frameCount;i++)SDL_free(s->frames[i].pixels);SDL_free(s->frames);}SDL_free(s->path);SDL_memset(s,0,sizeof(*s));}
#ifdef __EMSCRIPTEN__
EM_JS(void,BrowserAPNG_Start,(const char *path),{
	if(Module.vsAPNG)return;
	const name=UTF8ToString(path),bytes=FS.readFile(name);
	const image=new Image(),url=URL.createObjectURL(new Blob([bytes],{type:'image/png'}));
	Module.vsAPNG={image:image,canvas:null,context:null,ready:false,url:url};
	image.onload=()=>{const canvas=document.createElement('canvas');canvas.width=image.naturalWidth;canvas.height=image.naturalHeight;Module.vsAPNG.canvas=canvas;Module.vsAPNG.context=canvas.getContext('2d',{willReadFrequently:true});Module.vsAPNG.ready=true;};
	image.onerror=()=>console.error('Failed to decode APNG object: '+name);
	image.src=url;
});
EM_JS(int,BrowserAPNG_Width,(void),{return Module.vsAPNG&&Module.vsAPNG.ready?Module.vsAPNG.canvas.width:0;});
EM_JS(int,BrowserAPNG_Height,(void),{return Module.vsAPNG&&Module.vsAPNG.ready?Module.vsAPNG.canvas.height:0;});
EM_JS(void,BrowserAPNG_CopyBGRA,(Uint32 *pixels),{
	const a=Module.vsAPNG;if(!a||!a.ready)return;const c=a.canvas,x=a.context;x.clearRect(0,0,c.width,c.height);x.drawImage(a.image,0,0);const source=x.getImageData(0,0,c.width,c.height).data,destination=HEAPU8,o=pixels;
	for(let i=0;i<source.length;i+=4){destination[o+i]=source[i+2];destination[o+i+1]=source[i+1];destination[o+i+2]=source[i];destination[o+i+3]=source[i+3];}
});
#endif
#ifdef USE_APNG
static int AppendFrame(WorldSprite*s,AVFrame*src,struct SwsContext**converter,AVRational timeBase){
	SpriteFrame*g=SDL_realloc(s->frames,(size_t)(s->frameCount+1)*sizeof(*g));if(!g)return 0;s->frames=g;SpriteFrame*out=&g[s->frameCount];SDL_memset(out,0,sizeof(*out));
	out->pixels=SDL_malloc((size_t)s->pixelWidth*s->pixelHeight*sizeof(*out->pixels));if(!out->pixels)return 0;
	*converter=sws_getCachedContext(*converter,src->width,src->height,(enum AVPixelFormat)src->format,s->pixelWidth,s->pixelHeight,AV_PIX_FMT_BGRA,SWS_POINT,NULL,NULL,NULL);if(!*converter){SDL_free(out->pixels);out->pixels=NULL;return 0;}
	uint8_t*planes[4]={(uint8_t*)out->pixels,NULL,NULL,NULL};int strides[4]={s->pixelWidth*4,0,0,0};sws_scale(*converter,(const uint8_t*const*)src->data,src->linesize,0,src->height,planes,strides);
	const int64_t ticks=src->duration;out->durationMs=ticks>0?(float)(av_q2d(timeBase)*(double)ticks*1000.0):100.0f;if(out->durationMs<1||out->durationMs>60000)out->durationMs=100;s->frameCount++;return 1;
}
static int DecodeAPNG(const char*path,WorldSprite*s){AVFormatContext*f=NULL;AVCodecContext*c=NULL;AVPacket*p=NULL;AVFrame*frame=NULL;struct SwsContext*converter=NULL;int stream=-1,ok=0;
	if(avformat_open_input(&f,path,NULL,NULL)<0||avformat_find_stream_info(f,NULL)<0)goto done;stream=av_find_best_stream(f,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);if(stream<0)goto done;
	const AVCodec*decoder=avcodec_find_decoder(f->streams[stream]->codecpar->codec_id);if(!decoder||!(c=avcodec_alloc_context3(decoder))||avcodec_parameters_to_context(c,f->streams[stream]->codecpar)<0||avcodec_open2(c,decoder,NULL)<0)goto done;
	s->pixelWidth=c->width;s->pixelHeight=c->height;if(s->pixelWidth<=0||s->pixelHeight<=0||!(p=av_packet_alloc())||!(frame=av_frame_alloc()))goto done;
	while(av_read_frame(f,p)>=0){if(p->stream_index==stream&&avcodec_send_packet(c,p)>=0)while(avcodec_receive_frame(c,frame)==0)if(!AppendFrame(s,frame,&converter,f->streams[stream]->time_base))goto done;av_packet_unref(p);}
	avcodec_send_packet(c,NULL);while(avcodec_receive_frame(c,frame)==0)if(!AppendFrame(s,frame,&converter,f->streams[stream]->time_base))goto done;ok=s->frameCount>0;
done: if(p)av_packet_free(&p);if(frame)av_frame_free(&frame);if(converter)sws_freeContext(converter);if(c)avcodec_free_context(&c);if(f)avformat_close_input(&f);return ok;}
#endif

int SpriteRenderer_AddAPNG(const char*path,float x,float y,float z,float width,float height){
#if !defined(USE_APNG) && !defined(__EMSCRIPTEN__)
	(void)path;(void)x;(void)y;(void)z;(void)width;(void)height;SDL_LogWarn(0,"APNG objects disabled: FFmpeg development libraries were unavailable at build time");return -1;
#else
	if(!path||width<=0||height<=0)return -1;WorldSprite s;SDL_memset(&s,0,sizeof(s));s.x=x;s.y=y;s.z=z;s.width=width;s.height=height;s.path=SDL_strdup(path);if(!s.path)return -1;
	for(int i=0;i<sprites.count;i++)if(SDL_strcmp(sprites.items[i].path,path)==0){s.frames=sprites.items[i].frames;s.frameCount=sprites.items[i].frameCount;s.pixelWidth=sprites.items[i].pixelWidth;s.pixelHeight=sprites.items[i].pixelHeight;break;}
	#ifdef __EMSCRIPTEN__
	BrowserAPNG_Start(path);
	#else
	if(!s.frames){s.ownsFrames=1;if(!DecodeAPNG(path,&s)){SDL_LogError(0,"Failed to decode APNG object: %s",path);FreeSprite(&s);return -1;}}
	#endif
	if(sprites.count==sprites.capacity){int capacity=sprites.capacity?sprites.capacity*2:8;WorldSprite*g=SDL_realloc(sprites.items,(size_t)capacity*sizeof(*g));if(!g){FreeSprite(&s);return -1;}sprites.items=g;sprites.capacity=capacity;}
	sprites.items[sprites.count]=s;if(s.ownsFrames)SDL_Log("Loaded APNG object: %s (%d frame(s), %dx%d)",path,s.frameCount,s.pixelWidth,s.pixelHeight);return sprites.count++;
#endif
}
void SpriteRenderer_Update(float dt){
#ifdef __EMSCRIPTEN__
	(void)dt;if(!sprites.count)return;
	if(!sprites.items[0].frames){int width=BrowserAPNG_Width(),height=BrowserAPNG_Height();if(!width||!height)return;SpriteFrame*frame=SDL_calloc(1,sizeof(*frame));if(!frame)return;frame->pixels=SDL_malloc((size_t)width*height*sizeof(*frame->pixels));if(!frame->pixels){SDL_free(frame);return;}frame->durationMs=100;for(int i=0;i<sprites.count;i++){sprites.items[i].frames=frame;sprites.items[i].frameCount=1;sprites.items[i].pixelWidth=width;sprites.items[i].pixelHeight=height;}sprites.items[0].ownsFrames=1;SDL_Log("Browser decoded APNG object: %s (%dx%d, %d instances)",sprites.items[0].path,width,height,sprites.count);}
	BrowserAPNG_CopyBGRA(sprites.items[0].frames[0].pixels);
#else
	for(int i=0;i<sprites.count;i++){WorldSprite*s=&sprites.items[i];if(s->frameCount<2)continue;s->elapsedMs+=dt;while(s->elapsedMs>=s->frames[s->currentFrame].durationMs){s->elapsedMs-=s->frames[s->currentFrame].durationMs;s->currentFrame=(s->currentFrame+1)%s->frameCount;}}
#endif
}
int SpriteRenderer_HasObjects(void){return sprites.count>0;}
static Uint32 Blend(Uint32 s,Uint32 d){unsigned a=s>>24;if(a==255)return s;if(!a)return d;unsigned ia=255-a,r=(((s>>16)&255)*a+((d>>16)&255)*ia+127)/255,g=(((s>>8)&255)*a+((d>>8)&255)*ia+127)/255,b=((s&255)*a+(d&255)*ia+127)/255;return 0xff000000u|(r<<16)|(g<<8)|b;}
void SpriteRenderer_DrawSoftware(Map*m,Camera*c){if(!m->screen||!m->depth||!sprites.count)return;int w=0,h=0,pitchBytes=0;Uint32*pixels=NULL;SDL_QueryTexture((SDL_Texture*)m->screen,NULL,NULL,&w,&h);if(SDL_LockTexture((SDL_Texture*)m->screen,NULL,(void**)&pixels,&pitchBytes)!=0)return;int pitch=pitchBytes/4;float sn=SDL_sinf(c->angle),cs=SDL_cosf(c->angle),scale=(float)h/CAMERA_PROJECTION_SCALE;
	for(int i=0;i<sprites.count;i++){WorldSprite*s=&sprites.items[i];float dx=s->x-c->position.x,dy=s->y-c->position.y,depth=-sn*dx-cs*dy,lateral=cs*dx-sn*dy;if(depth<=1||depth>=c->distance)continue;float cx=(lateral/depth+1)*(float)w*.5f,half=s->width/depth*(float)w*.5f,top=(c->height-(s->z+s->height))*scale/depth+c->horizon,bottom=(c->height-s->z)*scale/depth+c->horizon;int x0=max((int)SDL_floorf(cx-half),0),x1=min((int)SDL_ceilf(cx+half),w),y0=max((int)SDL_floorf(top),0),y1=min((int)SDL_ceilf(bottom),h);if(x0>=x1||y0>=y1||bottom<=top)continue;SpriteFrame*f=&s->frames[s->currentFrame];for(int y=y0;y<y1;y++){int sy=max(0,min((int)(((y-top)/(bottom-top))*s->pixelHeight),s->pixelHeight-1));for(int x=x0;x<x1;x++){int offset=y*w+x;if(depth>=m->depth[offset])continue;int sx=max(0,min((int)(((x-(cx-half))/(half*2))*s->pixelWidth),s->pixelWidth-1));Uint32 color=f->pixels[sy*s->pixelWidth+sx];if((color>>24)<8)continue;pixels[y*pitch+x]=Blend(color,pixels[y*pitch+x]);m->depth[offset]=depth;}}}
	SDL_UnlockTexture((SDL_Texture*)m->screen);}

#ifdef USE_OPENGL_RENDER
typedef struct {float p[3],uv[2];} GLVertex;
static GLuint Compile(GLenum type,const char*source){GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){glDeleteShader(s);return 0;}return s;}
int SpriteRenderer_InitOpenGL(void){static const char*vs="#version 330 core\nlayout(location=0)in vec3 p;layout(location=1)in vec2 uv;uniform vec2 cameraPosition,resolution;uniform float cameraHeight,horizon,angle,distance;out vec2 fuv;void main(){float s=sin(angle),c=cos(angle);vec2 d=p.xy-cameraPosition;float z=-s*d.x-c*d.y,l=c*d.x-s*d.y;float ny=(1.-2.*horizon/resolution.y)*z-(2./2.5)*(cameraHeight-p.z);float n=1.,f=distance;gl_Position=vec4(l,ny,((f+n)/(f-n))*z-(2.*f*n/(f-n)),z);fuv=uv;}";static const char*fs="#version 330 core\nuniform sampler2D image;in vec2 fuv;out vec4 color;void main(){color=texture(image,fuv);if(color.a<.03)discard;}";GLuint v=Compile(GL_VERTEX_SHADER,vs),f=Compile(GL_FRAGMENT_SHADER,fs);if(!v||!f)return 1;sprites.program=glCreateProgram();glAttachShader(sprites.program,v);glAttachShader(sprites.program,f);glLinkProgram(sprites.program);glDeleteShader(v);glDeleteShader(f);GLint linked=0;glGetProgramiv(sprites.program,GL_LINK_STATUS,&linked);if(!linked)return 1;glGenVertexArrays(1,&sprites.vao);glGenBuffers(1,&sprites.vbo);glGenTextures(1,&sprites.texture);glBindVertexArray(sprites.vao);glBindBuffer(GL_ARRAY_BUFFER,sprites.vbo);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(GLVertex),(void*)0);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(GLVertex),(void*)offsetof(GLVertex,uv));glBindTexture(GL_TEXTURE_2D,sprites.texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);return 0;}
void SpriteRenderer_DrawOpenGL(Camera*c,int w,int h){if(!sprites.count)return;glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glUseProgram(sprites.program);glUniform2f(glGetUniformLocation(sprites.program,"cameraPosition"),c->position.x,c->position.y);glUniform2f(glGetUniformLocation(sprites.program,"resolution"),(float)w,(float)h);glUniform1f(glGetUniformLocation(sprites.program,"cameraHeight"),c->height);glUniform1f(glGetUniformLocation(sprites.program,"horizon"),c->horizon);glUniform1f(glGetUniformLocation(sprites.program,"angle"),c->angle);glUniform1f(glGetUniformLocation(sprites.program,"distance"),c->distance);glUniform1i(glGetUniformLocation(sprites.program,"image"),0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,sprites.texture);glBindVertexArray(sprites.vao);glBindBuffer(GL_ARRAY_BUFFER,sprites.vbo);float rx=SDL_cosf(c->angle),ry=-SDL_sinf(c->angle);
	for(int i=0;i<sprites.count;i++){WorldSprite*s=&sprites.items[i];float hx=rx*s->width*.5f,hy=ry*s->width*.5f,z0=s->z,z1=s->z+s->height;GLVertex v[6]={{{s->x-hx,s->y-hy,z1},{0,0}},{{s->x+hx,s->y+hy,z1},{1,0}},{{s->x+hx,s->y+hy,z0},{1,1}},{{s->x-hx,s->y-hy,z1},{0,0}},{{s->x+hx,s->y+hy,z0},{1,1}},{{s->x-hx,s->y-hy,z0},{0,1}}};SpriteFrame*f=&s->frames[s->currentFrame];if(f->pixels!=sprites.uploadedPixels||s->pixelWidth!=sprites.uploadedWidth||s->pixelHeight!=sprites.uploadedHeight){glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,s->pixelWidth,s->pixelHeight,0,GL_BGRA,GL_UNSIGNED_BYTE,f->pixels);sprites.uploadedPixels=f->pixels;sprites.uploadedWidth=s->pixelWidth;sprites.uploadedHeight=s->pixelHeight;}glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,6);}glBindVertexArray(0);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);}
void SpriteRenderer_DestroyOpenGL(void){if(sprites.texture)glDeleteTextures(1,&sprites.texture);if(sprites.vbo)glDeleteBuffers(1,&sprites.vbo);if(sprites.vao)glDeleteVertexArrays(1,&sprites.vao);if(sprites.program)glDeleteProgram(sprites.program);sprites.texture=sprites.vbo=sprites.vao=sprites.program=0;sprites.uploadedPixels=NULL;sprites.uploadedWidth=sprites.uploadedHeight=0;}
#endif
void SpriteRenderer_Destroy(void){for(int i=0;i<sprites.count;i++)FreeSprite(&sprites.items[i]);SDL_free(sprites.items);sprites.items=NULL;sprites.count=sprites.capacity=0;}
