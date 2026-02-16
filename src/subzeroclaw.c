/* subzeroclaw.c — complete agentic runtime in <200 lines */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cjson/cJSON.h>
#define MP 512
#define MV 1024
#define MR (128*1024)
typedef struct { char key[MV],model[MV],ep[MV],skills[MP],logdir[MP]; int mt,mm,ck; } Cfg;
typedef struct { char *fr,*text; cJSON *tc,*msg; } Resp;
static const char TOOLS[]=
    "[{\"type\":\"function\",\"function\":{\"name\":\"shell\",\"description\":\"Run command\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}},{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
    "\"description\":\"Read file\",\"parameters\":{\"type\":\"object\",\"properties\":"
    "{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},{\"type\":\"function\","
    "\"function\":{\"name\":\"write_file\",\"description\":\"Write file\",\"parameters\":"
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":"
    "{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}}]";
static void mkdirp(const char *p){char t[512];snprintf(t,512,"%s",p);
    for(char *s=t+1;*s;s++)if(*s=='/'){*s=0;mkdir(t,0755);*s='/';}mkdir(t,0755);}
static void logw(FILE *f,const char *r,const char *c){if(!f)return;time_t n=time(0);
    struct tm *t=localtime(&n);char ts[32];strftime(ts,32,"%Y-%m-%d %H:%M:%S",t);
    fprintf(f,"[%s] %s: %s\n",ts,r,c);fflush(f);}
int config_load(Cfg *c) {
    const char *h=getenv("HOME"); if(!h) h=".";
    snprintf(c->ep,MV,"https://openrouter.ai/api/v1/chat/completions");
    snprintf(c->model,MV,"anthropic/claude-sonnet-4-20250514");
    snprintf(c->skills,MP,"%s/.subzeroclaw/skills",h);snprintf(c->logdir,MP,"%s/.subzeroclaw/logs",h);
    c->key[0]=0; c->mt=200; c->mm=40; c->ck=16;
    char path[MP]; snprintf(path,MP,"%s/.subzeroclaw/config",h);
    FILE *f=fopen(path,"r"); if(f){char ln[2048]; while(fgets(ln,sizeof ln,f)){
        size_t l=strlen(ln); while(l&&strchr("\n\r ",ln[l-1]))ln[--l]=0;
        char *s=ln; while(*s==' ')s++; char *eq=strchr(s,'=');
        if(!eq||*s=='#'||!*s)continue; *eq=0; char *k=s,*v=eq+1;
        l=strlen(k); while(l&&k[l-1]==' ')k[--l]=0;
        while(*v==' ')v++; l=strlen(v); if(l>=2&&*v=='"'&&v[l-1]=='"'){v++;v[l-2]=0;}
        if(!strcmp(k,"api_key"))snprintf(c->key,MV,"%s",v);else if(!strcmp(k,"model"))snprintf(c->model,MV,"%s",v);
        else if(!strcmp(k,"endpoint"))snprintf(c->ep,MV,"%s",v);else if(!strcmp(k,"skills_dir"))snprintf(c->skills,MP,"%s",v);
        else if(!strcmp(k,"log_dir"))snprintf(c->logdir,MP,"%s",v);else if(!strcmp(k,"max_turns"))c->mt=atoi(v);
        else if(!strcmp(k,"max_messages"))c->mm=atoi(v);else if(!strcmp(k,"compact_keep"))c->ck=atoi(v);
    }fclose(f);}
    char *e;if((e=getenv("SUBZEROCLAW_API_KEY")))snprintf(c->key,MV,"%s",e);if((e=getenv("SUBZEROCLAW_MODEL")))snprintf(c->model,MV,"%s",e);
    if((e=getenv("SUBZEROCLAW_ENDPOINT")))snprintf(c->ep,MV,"%s",e);
    if(!c->key[0]){fprintf(stderr,"error: no api_key\n");return -1;} return 0;
}
char *http_post(const char *url,const char *key,const char *body) {
    FILE *t=fopen("/tmp/.szc.json","w"); if(!t)return NULL; fputs(body,t); fclose(t);
    char cmd[2048]; snprintf(cmd,2048,"curl -s -m 120 -H 'Authorization: Bearer %s' "
        "-H 'Content-Type: application/json' -d @/tmp/.szc.json '%s' 2>&1",key,url);
    FILE *fp=popen(cmd,"r"); if(!fp)return NULL;
    size_t cap=65536,len=0,n;char *b=malloc(cap);while(b&&(n=fread(b+len,1,cap-len-1,fp))>0){len+=n;if(len+1>=cap){cap*=2;b=realloc(b,cap);}}
    if(b)b[len]=0; pclose(fp); remove("/tmp/.szc.json"); return b;
}
char *tool_execute(const char *name,const char *aj) {
    cJSON *a=cJSON_Parse(aj),*f1,*f2;
    if(!strcmp(name,"shell")) {
        if(!a||(f1=cJSON_GetObjectItem(a,"command"),!f1||!cJSON_IsString(f1)))
            {if(a)cJSON_Delete(a);return strdup("error: bad args");}
        size_t cl=strlen(f1->valuestring);char *fc=malloc(cl+8);memcpy(fc,f1->valuestring,cl);memcpy(fc+cl," 2>&1",6);
        FILE *fp=popen(fc,"r"); free(fc); if(!fp){cJSON_Delete(a);return strdup("error");}
        char *r=malloc(MR);size_t len=0,n;while((n=fread(r+len,1,MR-len-1,fp))>0){len+=n;if(len>=MR-1)break;}
        r[len]=0;pclose(fp);cJSON_Delete(a);
        if(len+1<MR/2){char *x=realloc(r,len+1);if(x)r=x;} return r;
    } else if(!strcmp(name,"read_file")) {
        if(!a||(f1=cJSON_GetObjectItem(a,"path"),!f1||!cJSON_IsString(f1)))
            {if(a)cJSON_Delete(a);return strdup("error: bad args");}
        FILE *f=fopen(f1->valuestring,"r"); cJSON_Delete(a); if(!f)return strdup("error: can't open");
        fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);if(sz>MR-1)sz=MR-1;
        char *c=malloc(sz+1); c[fread(c,1,sz,f)]=0; fclose(f); return c;
    } else if(!strcmp(name,"write_file")) {
        if(!a||(f1=cJSON_GetObjectItem(a,"path"),f2=cJSON_GetObjectItem(a,"content"),
            !f1||!f2||!cJSON_IsString(f1)||!cJSON_IsString(f2)))
            {if(a)cJSON_Delete(a);return strdup("error: bad args");}
        char *pc=strdup(f1->valuestring),*ls=strrchr(pc,'/');
        if(ls&&ls!=pc){*ls=0;mkdirp(pc);}free(pc);
        FILE *f=fopen(f1->valuestring,"w");if(!f){cJSON_Delete(a);return strdup("error: can't write");}
        fputs(f2->valuestring,f); fclose(f);
        char ok[256];snprintf(ok,256,"wrote %zu bytes to %s",strlen(f2->valuestring),f1->valuestring);
        cJSON_Delete(a); return strdup(ok);
    }
    if(a)cJSON_Delete(a); return strdup("error: unknown tool");
}
cJSON *tools_build_definitions(void) { return cJSON_Parse(TOOLS); }
static char *build_req(const Cfg *c,cJSON *msgs,cJSON *tools) {
    cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"model",c->model);
    cJSON_AddItemReferenceToObject(r,"messages",msgs);if(tools)cJSON_AddItemReferenceToObject(r,"tools",tools);
    char *j=cJSON_PrintUnformatted(r);cJSON_DetachItemFromObject(r,"messages");
    if(tools)cJSON_DetachItemFromObject(r,"tools");cJSON_Delete(r);return j;
}
static int parse_resp(const char *body,Resp *o) {
    memset(o,0,sizeof*o); cJSON *root=cJSON_Parse(body); if(!root)return -1;
    cJSON *e=cJSON_GetObjectItem(root,"error");
    if(e){fprintf(stderr,"API: %s\n",cJSON_GetObjectItem(e,"message")?
        cJSON_GetObjectItem(e,"message")->valuestring:"?");cJSON_Delete(root);return -1;}
    cJSON *ch=cJSON_GetObjectItem(root,"choices");if(!ch||!ch->child){cJSON_Delete(root);return -1;}
    cJSON *c=ch->child,*m=cJSON_GetObjectItem(c,"message");if(!m){cJSON_Delete(root);return -1;}
    cJSON *fr=cJSON_GetObjectItem(c,"finish_reason");
    o->fr=strdup(fr&&cJSON_IsString(fr)?fr->valuestring:"stop");
    o->msg=cJSON_DetachItemFromObject(c,"message");cJSON *ct=cJSON_GetObjectItem(o->msg,"content");
    o->text=(ct&&cJSON_IsString(ct))?ct->valuestring:NULL;
    o->tc=cJSON_GetObjectItem(o->msg,"tool_calls");cJSON_Delete(root);return 0;
}
char *agent_build_system_prompt(const char *dir) {
    size_t cap=8192,len; char *p=malloc(cap);
    len=snprintf(p,cap,"You are SubZeroClaw, a minimal agentic assistant.\n"
        "Tools: shell (any command), read_file, write_file.\nBe concise. Just do it.\n\n");
    DIR *d=opendir(dir); if(!d)return p; struct dirent *e;
    while((e=readdir(d))){size_t nl=strlen(e->d_name);
        if(nl<4||strcmp(e->d_name+nl-3,".md"))continue;
        char fp[MP];snprintf(fp,MP,"%s/%s",dir,e->d_name);FILE *f=fopen(fp,"r");if(!f)continue;
        fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
        while(len+sz+128>=cap){cap*=2;p=realloc(p,cap);}
        len+=snprintf(p+len,cap-len,"\n--- SKILL: %s ---\n",e->d_name);
        len+=fread(p+len,1,sz,f);p[len]=0;fclose(f);}closedir(d);return p;
}
static int compact(const Cfg *c,cJSON *msgs,FILE *log) {
    int tot=cJSON_GetArraySize(msgs); if(tot<=c->mm)return 0;
    int bnd=tot-c->ck; if(bnd<=1)return 0;
    fprintf(stderr,"[compact] %d msgs, keeping last %d\n",tot,c->ck);logw(log,"SYS","compacting");
    size_t cap=4096,len=0; char *buf=malloc(cap); buf[0]=0; int idx=0; cJSON *m=NULL;
    cJSON_ArrayForEach(m,msgs){if(idx>=bnd)break; if(idx>=1){
        cJSON *r=cJSON_GetObjectItem(m,"role"),*ct=cJSON_GetObjectItem(m,"content");
        const char *rs=r?r->valuestring:"?",*cs=ct&&cJSON_IsString(ct)?ct->valuestring:"[tool]";
        size_t n=strlen(rs)+strlen(cs)+4;while(len+n>=cap){cap*=2;buf=realloc(buf,cap);}
        len+=snprintf(buf+len,cap-len,"%s: %s\n",rs,cs);}idx++;}
    cJSON *sm=cJSON_CreateArray(),*sy=cJSON_CreateObject(),*u=cJSON_CreateObject();
    cJSON_AddStringToObject(sy,"role","system");cJSON_AddStringToObject(sy,"content","Summarize. Keep all facts, paths, commands.");
    cJSON_AddStringToObject(u,"role","user");cJSON_AddStringToObject(u,"content",buf);
    cJSON_AddItemToArray(sm,sy);cJSON_AddItemToArray(sm,u);
    char *rj=build_req(c,sm,NULL);free(buf);char *rb=http_post(c->ep,c->key,rj);free(rj);cJSON_Delete(sm);if(!rb)return -1;
    Resp resp;if(parse_resp(rb,&resp)!=0||!resp.text){free(rb);if(resp.fr)free(resp.fr);if(resp.msg)cJSON_Delete(resp.msg);return -1;}
    char *sum=strdup(resp.text);free(resp.fr);cJSON_Delete(resp.msg);free(rb);logw(log,"COMPACT",sum);
    cJSON *na=cJSON_CreateArray(); idx=0;
    for(cJSON *it=msgs->child;it;){cJSON *nx=it->next;it->prev=it->next=NULL;
        if(idx==0||idx>=bnd)cJSON_AddItemToArray(na,it);else cJSON_Delete(it);idx++;it=nx;}
    cJSON *s1=cJSON_CreateObject(),*s2=cJSON_CreateObject();
    cJSON_AddStringToObject(s1,"role","user");cJSON_AddStringToObject(s1,"content","[Summary]");
    cJSON_AddStringToObject(s2,"role","assistant");cJSON_AddStringToObject(s2,"content",sum);free(sum);
    cJSON *sp=na->child,*tf=sp->next;
    s1->prev=sp;s1->next=s2;s2->prev=s1;s2->next=tf;sp->next=s1;if(tf)tf->prev=s2;
    msgs->child=na->child;na->child=NULL;cJSON_Delete(na);return 0;
}
static int agent_run(const Cfg *c,cJSON *msgs,cJSON *tools,const char *in,FILE *log) {
    compact(c,msgs,log);
    cJSON *um=cJSON_CreateObject();cJSON_AddStringToObject(um,"role","user");
    cJSON_AddStringToObject(um,"content",in);cJSON_AddItemToArray(msgs,um);logw(log,"USER",in);
    for(int t=0;++t<=c->mt;){
        char *rj=build_req(c,msgs,tools);if(!rj)return -1;
        fprintf(stderr,"[%d] %s...\n",t,c->model);
        char *rb=http_post(c->ep,c->key,rj);free(rj);if(!rb)return -1;
        Resp r;if(parse_resp(rb,&r)){free(rb);return -1;}free(rb);
        cJSON_AddItemToArray(msgs,r.msg);
        if(!strcmp(r.fr,"stop")){
            if(r.text){printf("%s\n",r.text);logw(log,"ASST",r.text);}free(r.fr);return 0;}
        if(!strcmp(r.fr,"tool_calls")&&r.tc){cJSON *tc=NULL;cJSON_ArrayForEach(tc,r.tc){
            cJSON *id=cJSON_GetObjectItem(tc,"id"),*fn=cJSON_GetObjectItem(tc,"function");
            if(!fn||!id)continue;cJSON *nm=cJSON_GetObjectItem(fn,"name"),*ar=cJSON_GetObjectItem(fn,"arguments");
            if(!nm||!ar)continue;logw(log,"TOOL",nm->valuestring);
            char *res=tool_execute(nm->valuestring,ar->valuestring);logw(log,"RES",res?res:"null");
            cJSON *tm=cJSON_CreateObject();cJSON_AddStringToObject(tm,"role","tool");
            cJSON_AddStringToObject(tm,"tool_call_id",id->valuestring);
            cJSON_AddStringToObject(tm,"content",res?res:"error");cJSON_AddItemToArray(msgs,tm);
            free(res);}free(r.fr);continue;}
        if(r.text){printf("%s\n",r.text);logw(log,"ASST",r.text);}free(r.fr);return 0;}
    fprintf(stderr,"max turns\n");return -1;
}
#ifndef SZC_TEST
int main(int argc,char **argv) {
    if(argc>1&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){
        fprintf(stderr,"SubZeroClaw — pure C agentic runtime\n"
            "Usage: subzeroclaw [\"prompt\"]\nConfig: ~/.subzeroclaw/config\n");return 0;}
    Cfg cfg;if(config_load(&cfg))return 1;
    char *sp=agent_build_system_prompt(cfg.skills);if(!sp)return 1;
    char sid[32]={0};{FILE *r=fopen("/dev/urandom","r");if(r){unsigned char b[8];
        if(fread(b,1,8,r)==8)snprintf(sid,32,"%02x%02x%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);fclose(r);}
    if(!sid[0])snprintf(sid,32,"%lx%x",(long)time(0),getpid());}
    mkdirp(cfg.logdir);char lp[600];snprintf(lp,600,"%s/%s.txt",cfg.logdir,sid);
    FILE *log=fopen(lp,"a");if(log){time_t n=time(0);fprintf(log,"=== %s %s",sid,ctime(&n));fflush(log);}
    cJSON *msgs=cJSON_CreateArray(),*sys=cJSON_CreateObject();
    cJSON_AddStringToObject(sys,"role","system");cJSON_AddStringToObject(sys,"content",sp);
    cJSON_AddItemToArray(msgs,sys);cJSON *tools=cJSON_Parse(TOOLS);
    fprintf(stderr,"subzeroclaw · %s · %s\n",cfg.model,sid);
    if(argc>1){char in[4096],*p=in,*end=in+4095;
        for(int i=1;i<argc&&p<end;i++){if(i>1)*p++=' ';size_t l=strlen(argv[i]),a=end-p;if(l>a)l=a;memcpy(p,argv[i],l);p+=l;}*p=0;
        int rc=agent_run(&cfg,msgs,tools,in,log);cJSON_Delete(msgs);cJSON_Delete(tools);free(sp);if(log)fclose(log);return rc;}
    char in[4096];
    for(;;){printf("> ");fflush(stdout);if(!fgets(in,4096,stdin))break;
        size_t l=strlen(in);while(l&&strchr("\n\r",in[l-1]))in[--l]=0;
        if(!l)continue;if(!strcmp(in,"/quit")||!strcmp(in,"/exit"))break;agent_run(&cfg,msgs,tools,in,log);printf("\n");}
    cJSON_Delete(msgs);cJSON_Delete(tools);free(sp);if(log)fclose(log);return 0;
}
#endif
