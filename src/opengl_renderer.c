#ifdef USE_OPENGL_RENDER
#include "opengl_renderer.h"
#include "defines.h"
#include "polygon_renderer.h"
#include <SDL_log.h>
#include <SDL_stdinc.h>
#include <stddef.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>

struct Vertex { float x,y; unsigned char color[4]; };
static struct {
	SDL_GLContext context; int width,height,outputWidth,outputHeight,integerScale;
	GLuint mapProgram,mapVao,uiProgram,uiVao,uiVbo,tex[4];
	GLuint sceneFbo,sceneColor,sceneDepth;
	GLint uiResolution; struct Vertex *vertices; size_t count,capacity;
	const void *data[4]; int floorWidth,ceilingWidth;
} glr;

static GLuint Compile(GLenum type,const char *source){
	GLuint shader=glCreateShader(type); glShaderSource(shader,1,&source,NULL); glCompileShader(shader);
	GLint ok=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&ok); if(!ok){ char log[2048];
		glGetShaderInfoLog(shader,sizeof(log),NULL,log); SDL_LogCritical(0,"Shader compile failed: %s",log);
		glDeleteShader(shader); return 0; } return shader;
}
static GLuint Link(const char *vs,const char *fs){
	GLuint v=Compile(GL_VERTEX_SHADER,vs),f=Compile(GL_FRAGMENT_SHADER,fs); if(!v||!f)return 0;
	GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
	glDeleteShader(v); glDeleteShader(f); GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
	if(!ok){ char log[2048]; glGetProgramInfoLog(p,sizeof(log),NULL,log);
		SDL_LogCritical(0,"Program link failed: %s",log); glDeleteProgram(p); return 0; } return p;
}

static int CreatePipelines(void){
	static const char *mapVS=
		"#version 330 core\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\n"
		"void main(){gl_Position=vec4(p[gl_VertexID],0,1);}\n";
	static const char *mapFS=
		"#version 330 core\n"
		"uniform sampler2D floorColor,floorHeight,ceilingColor,ceilingHeight;\n"
		"uniform vec2 resolution,cameraPosition; uniform float cameraHeight,horizon,angle,distance,zstep;\n"
		"uniform float ceilingBase,optimizeDistance; uniform int floorSize,ceilingSize,ceilingEnabled,optimize;\n"
		"out vec4 outColor;\n"
		"vec3 fog(vec3 c,float z){float f=smoothstep(distance*.5833,distance*.9667,z);return mix(c,vec3(144,224,255)/255.,f);}\n"
		"float depthValue(float z){float n=1.,f=distance;return .5*((f+n)/(f-n)-(2.*f*n/(f-n))/z)+.5;}\n"
		"void main(){float sx=gl_FragCoord.x,sy=resolution.y-gl_FragCoord.y;float s=sin(angle),c=cos(angle);\n"
		"float scale=resolution.y/2.5,z=1.,dz=1.;\n"
		"for(int i=0;i<4096;i++){if(z>=distance)break;float dx=2.*c*z/resolution.x,dy=-2.*s*z/resolution.x;\n"
		"float px=(-c-s)*z+cameraPosition.x+dx*sx,py=(s-c)*z+cameraPosition.y+dy*sx;\n"
		"ivec2 fp=ivec2(int(px)&(floorSize-1),int(py)&(floorSize-1));\n"
		"vec2 fhp=(vec2(px,py)+.5)/float(floorSize);float a=texture(floorHeight,fhp).r*255.;float ft=(cameraHeight-a)*scale/z+horizon;\n"
		"if(sy>=ft){gl_FragDepth=depthValue(z);outColor=vec4(fog(texelFetch(floorColor,fp,0).rgb,z),1);return;}\n"
		"if(ceilingEnabled!=0){ivec2 cp=ivec2(int(px)&(ceilingSize-1),int(py)&(ceilingSize-1));\n"
		"vec2 chp=(vec2(px,py)+.5)/float(ceilingSize);float ca=texture(ceilingHeight,chp).r*255.;float cb=(cameraHeight-(ceilingBase-ca))*scale/z+horizon;\n"
		"if(sy<=cb){gl_FragDepth=depthValue(z);outColor=vec4(fog(texelFetch(ceilingColor,cp,0).rgb,z),1);return;}}\n"
		"dz+=zstep;float ls=distance*.5;if(z>ls)dz+=zstep*(z/2.)*.35*smoothstep(0.,1.,(z-ls)/(distance-ls));\n"
		"if(optimize!=0&&z>optimizeDistance)dz+=zstep*(z/2.);z+=dz;}\n"
		"gl_FragDepth=1.;outColor=vec4(144,224,255,255)/255.;}\n";
	static const char *uiVS=
		"#version 330 core\nlayout(location=0)in vec2 position;layout(location=1)in vec4 color;\n"
		"uniform vec2 resolution;out vec4 vc;void main(){vec2 n=position/resolution*2.-1.;gl_Position=vec4(n.x,-n.y,0,1);vc=color;}\n";
	static const char *uiFS="#version 330 core\nin vec4 vc;out vec4 color;void main(){color=vc;}\n";
	glr.mapProgram=Link(mapVS,mapFS); glr.uiProgram=Link(uiVS,uiFS); if(!glr.mapProgram||!glr.uiProgram)return 0;
	glGenVertexArrays(1,&glr.mapVao); glGenVertexArrays(1,&glr.uiVao); glGenBuffers(1,&glr.uiVbo);
	glBindVertexArray(glr.uiVao); glBindBuffer(GL_ARRAY_BUFFER,glr.uiVbo);
	glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(struct Vertex),(void*)0);
	glEnableVertexAttribArray(1); glVertexAttribPointer(1,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(struct Vertex),(void*)offsetof(struct Vertex,color));
	glBindVertexArray(0); glr.uiResolution=glGetUniformLocation(glr.uiProgram,"resolution");
	glGenTextures(4,glr.tex);glGenFramebuffers(1,&glr.sceneFbo);
	glGenTextures(1,&glr.sceneColor);glGenRenderbuffers(1,&glr.sceneDepth);return 1;
}

static void Upload(GLuint texture,GLint internal,GLenum format,GLint filter,int width,const void *pixels){
	glBindTexture(GL_TEXTURE_2D,texture); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filter);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filter); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT); glPixelStorei(GL_UNPACK_ALIGNMENT,1);
	glTexImage2D(GL_TEXTURE_2D,0,internal,width,width,0,format,GL_UNSIGNED_BYTE,pixels);
}
static void SyncTextures(Map *m){
	if(glr.data[0]!=m->color||glr.data[1]!=m->altitude||glr.floorWidth!=m->width){
		Upload(glr.tex[0],GL_RGBA8,GL_BGRA,GL_NEAREST,m->width,m->color); Upload(glr.tex[1],GL_R8,GL_RED,GL_LINEAR,m->width,m->altitude);
		glr.data[0]=m->color;glr.data[1]=m->altitude;glr.floorWidth=m->width;}
	if(m->ceilingReady&&(glr.data[2]!=m->ceilingColor||glr.data[3]!=m->ceilingAltitude||glr.ceilingWidth!=m->ceilingWidth)){
		Upload(glr.tex[2],GL_RGBA8,GL_BGRA,GL_NEAREST,m->ceilingWidth,m->ceilingColor);Upload(glr.tex[3],GL_R8,GL_RED,GL_LINEAR,m->ceilingWidth,m->ceilingAltitude);
		glr.data[2]=m->ceilingColor;glr.data[3]=m->ceilingAltitude;glr.ceilingWidth=m->ceilingWidth;}
}
static void U1f(const char*n,float v){glUniform1f(glGetUniformLocation(glr.mapProgram,n),v);}
static void U1i(const char*n,int v){glUniform1i(glGetUniformLocation(glr.mapProgram,n),v);}

int OpenGLRenderer_Init(SDL_Window*w,int vsync){glr.context=SDL_GL_CreateContext(w);
	if(!glr.context||SDL_GL_MakeCurrent(w,glr.context)!=0){SDL_LogCritical(0,"OpenGL context failed: %s",SDL_GetError());return 1;}
	if(!CreatePipelines()||PolygonRenderer_InitOpenGL()!=0)return 1;SDL_GL_SetSwapInterval(vsync?1:0);glDisable(GL_DEPTH_TEST);
	SDL_Log("Using OpenGL %s renderer: %s",glGetString(GL_VERSION),glGetString(GL_RENDERER));return 0;}
void OpenGLRenderer_Resize(int w,int h,int outputWidth,int outputHeight,int integerScale){
	glr.width=w;glr.height=h;glr.outputWidth=outputWidth;glr.outputHeight=outputHeight;
	glr.integerScale=max(integerScale,1);
	SDL_Log("OpenGL render resolution: %dx%d, output: %dx%d, integer scale: %dx",
		w,h,outputWidth,outputHeight,glr.integerScale);
	glBindTexture(GL_TEXTURE_2D,glr.sceneColor);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
	glBindRenderbuffer(GL_RENDERBUFFER,glr.sceneDepth);
	glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);
	glBindFramebuffer(GL_FRAMEBUFFER,glr.sceneFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,glr.sceneColor,0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,glr.sceneDepth);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
		SDL_LogCritical(0,"OpenGL scene framebuffer is incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,outputWidth,outputHeight);
}
void OpenGLRenderer_Draw(Map*m,Camera*c){
	glBindFramebuffer(GL_FRAMEBUFFER,glr.sceneFbo);glViewport(0,0,glr.width,glr.height);
	glClearColor(144.f/255,224.f/255,1,1);glClearDepth(1.0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);if(!m->ready){glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,glr.outputWidth,glr.outputHeight);glClear(GL_COLOR_BUFFER_BIT);return;}SyncTextures(m);
	glUseProgram(glr.mapProgram);glUniform2f(glGetUniformLocation(glr.mapProgram,"resolution"),glr.width,glr.height);
	glUniform2f(glGetUniformLocation(glr.mapProgram,"cameraPosition"),c->position.x,c->position.y);
	U1f("cameraHeight",c->height);U1f("horizon",c->horizon);U1f("angle",c->angle);U1f("distance",c->distance);U1f("zstep",c->zstep);
	U1f("ceilingBase",m->ceilingBase);U1f("optimizeDistance",m->optdist);U1i("floorSize",m->width);U1i("ceilingSize",m->ceilingWidth);
	U1i("ceilingEnabled",m->ceilingReady&&m->ceilingEnabled);U1i("optimize",m->optimize);
	const char*n[4]={"floorColor","floorHeight","ceilingColor","ceilingHeight"};for(int i=0;i<4;i++){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,glr.tex[i]);U1i(n[i],i);}
	glBindVertexArray(glr.mapVao);glDrawArrays(GL_TRIANGLES,0,3);glBindVertexArray(0);m->redraw=0;
	PolygonRenderer_DrawOpenGL(c,glr.width,glr.height);
	glBindFramebuffer(GL_READ_FRAMEBUFFER,glr.sceneFbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
	glViewport(0,0,glr.outputWidth,glr.outputHeight);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
	const int scaledWidth=glr.width*glr.integerScale,scaledHeight=glr.height*glr.integerScale;
	const int x=(glr.outputWidth-scaledWidth)/2,y=(glr.outputHeight-scaledHeight)/2;
	glBlitFramebuffer(0,0,glr.width,glr.height,x,y,x+scaledWidth,y+scaledHeight,GL_COLOR_BUFFER_BIT,GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER,0);
}

static int Reserve(size_t n){if(n<=glr.capacity)return 1;size_t c=glr.capacity?glr.capacity:256;while(c<n)c*=2;
	void*p=SDL_realloc(glr.vertices,c*sizeof(*glr.vertices));if(!p)return 0;glr.vertices=p;glr.capacity=c;return 1;}
static void V(float x,float y,const unsigned char c[4]){if(!Reserve(glr.count+1))return;struct Vertex*v=&glr.vertices[glr.count++];v->x=x;v->y=y;SDL_memcpy(v->color,c,4);}
static void Rect(float x,float y,float w,float h,const unsigned char c[4]){V(x,y,c);V(x+w,y,c);V(x+w,y+h,c);V(x,y,c);V(x+w,y+h,c);V(x,y+h,c);}
enum{A=1,B=2,C=4,D=8,E=16,F=32,G=64};
static unsigned char Mask(char c){switch(c){case'0':return A|B|C|D|E|F;case'1':return B|C;case'2':return A|B|G|E|D;case'3':return A|B|G|C|D;case'4':return F|G|B|C;case'5':return A|F|G|C|D;case'6':return A|F|G|E|C|D;case'7':return A|B|C;case'8':return 127;case'9':return A|B|C|D|F|G;case'F':return A|E|F|G;case'P':return A|B|E|F|G;case'S':return A|F|G|C|D;default:return 0;}}
static int Width(char c,int s){return c==' '?s*3:s*6;}
static void Glyph(char c,int x,int y,int s,const unsigned char k[4]){int h=s*4,v=s*5,w=Width(c,s);unsigned char m=Mask(c);if(c=='.'){Rect(x+w-s,y+2*v+2*s,s,s,k);return;}if(m&A)Rect(x+s,y,h,s,k);if(m&B)Rect(x+s+h,y+s,s,v,k);if(m&C)Rect(x+s+h,y+2*s+v,s,v,k);if(m&D)Rect(x+s,y+2*v+2*s,h,s,k);if(m&E)Rect(x,y+2*s+v,s,v,k);if(m&F)Rect(x,y+s,s,v,k);if(m&G)Rect(x+s,y+s+v,h,s,k);}
void OpenGLRenderer_DrawFPS(float fps){int s=2,g=4,p=6,m=10,w=0;char t[24];SDL_snprintf(t,sizeof(t),"FPS %.0f",fps);for(size_t i=0;t[i];i++)w+=Width(t[i],s)+g;if(w)w-=g;glr.count=0;int l=max(glr.outputWidth-w-p*2-m,m);const unsigned char bg[4]={0,0,0,140},fg[4]={255,255,90,255};Rect(l,m,w+p*2,26+p*2,bg);int x=l+p;for(size_t i=0;t[i];i++){Glyph(t[i],x,m+p,s,fg);x+=Width(t[i],s)+g;}glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glUseProgram(glr.uiProgram);glUniform2f(glr.uiResolution,glr.outputWidth,glr.outputHeight);glBindVertexArray(glr.uiVao);glBindBuffer(GL_ARRAY_BUFFER,glr.uiVbo);glBufferData(GL_ARRAY_BUFFER,glr.count*sizeof(*glr.vertices),glr.vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,(GLsizei)glr.count);glBindVertexArray(0);glDisable(GL_BLEND);}
void OpenGLRenderer_Present(SDL_Window*w){glFinish();SDL_GL_SwapWindow(w);}
void OpenGLRenderer_Destroy(void){PolygonRenderer_DestroyOpenGL();glDeleteTextures(4,glr.tex);if(glr.sceneColor)glDeleteTextures(1,&glr.sceneColor);if(glr.sceneDepth)glDeleteRenderbuffers(1,&glr.sceneDepth);if(glr.sceneFbo)glDeleteFramebuffers(1,&glr.sceneFbo);if(glr.uiVbo)glDeleteBuffers(1,&glr.uiVbo);if(glr.uiVao)glDeleteVertexArrays(1,&glr.uiVao);if(glr.mapVao)glDeleteVertexArrays(1,&glr.mapVao);if(glr.uiProgram)glDeleteProgram(glr.uiProgram);if(glr.mapProgram)glDeleteProgram(glr.mapProgram);SDL_free(glr.vertices);if(glr.context)SDL_GL_DeleteContext(glr.context);SDL_memset(&glr,0,sizeof(glr));}
#endif
