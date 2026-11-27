/* Compare an unchanged command set's output and redraw costs. Build against
 * either source with -DEDITOR_SOURCE='"/absolute/path/editor.c"'. */
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static size_t allocations;
static void *count_realloc(void *p,size_t n){allocations++;return realloc(p,n);}
#define realloc count_realloc
#define main editor_main
#ifndef EDITOR_SOURCE
#define EDITOR_SOURCE "../editor.c"
#endif
#include EDITOR_SOURCE
#undef main
#undef realloc
static double now(void){struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));return (double)ts.tv_sec+ts.tv_nsec/1e9;}
static void init_doc(const char *bytes,size_t n){
    assert(copy_buffer(&text,bytes,n));assert(copy_buffer(&saved,bytes,n));
    cursor=top=left=0;search_mark=SIZE_MAX;search_length=0;resized=0;
    rows=24;cols=80;reset_screen();
}
static void frames(void){
    const char *fixture="a\r\nsecond\n\twide: \xc3\xa9 \xe4\xb8\xad\ninvalid \xff\033\r\n\nlast";
    assert(setlocale(LC_ALL,"C.UTF-8"));
    init_doc(fixture,strlen(fixture));tty_out=STDOUT_FILENO;
    for(size_t i=0;i<=text.n;i++){cursor=i;draw();}
    assert(copy_buffer(&text,"x\r\ny\r\n",6));layout_reset();cursor=top=left=0;draw();cursor=text.n;draw();
    {char long_line[12500];memset(long_line,'x',sizeof long_line);
     long_line[200]='\t';long_line[4094]='\r';long_line[4095]='\n';long_line[10000]='\n';
     init_doc(long_line,sizeof long_line);
     for(size_t i=0;i<sizeof long_line;i+=97){cursor=i;draw();}}
    assert(copy_buffer(&text,"",0));cursor=top=left=0;draw();
}
static void benchmark(const char *label,size_t iterations,bool long_line){
    size_t n=long_line?LIMIT:16000;
    char *b=malloc(n);assert(b);memset(b,'a',n);
    if(!long_line)for(size_t i=79;i<n;i+=80)b[i]='\n';
    init_doc(b,n);free(b);tty_out=open("/dev/null",O_WRONLY);assert(tty_out>=0);
    if(long_line)cursor=n-1000;
    draw();allocations=0;
    double start=now();
    for(size_t i=0;i<iterations;i++){
        cursor=long_line?n-1000+(i%32):i%70;draw();
    }
    printf("%s: %zu redraws, %.6f seconds, %zu realloc calls\n",label,iterations,now()-start,allocations);
    assert(!close(tty_out));
}
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"frames"))frames();
    else{benchmark("normal",15000,false);benchmark("8 MiB single line",1000,true);}
    reset_screen();free(text.s);free(saved.s);return 0;
}
