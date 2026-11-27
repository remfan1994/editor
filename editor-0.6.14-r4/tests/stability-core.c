/* Deterministic fault injection and byte-model checks. No editor hooks. */
#define _XOPEN_SOURCE 700
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
static struct stat watched;
static const char *watched_path, *replacement_path;
static unsigned observations;
static bool swap_on_stat, resize_during_query;
static int watched_fstat(int fd, struct stat *st)
{
    int r=fstat(fd,st);
    if (!r && swap_on_stat && st->st_dev==watched.st_dev && st->st_ino==watched.st_ino && ++observations==2) {
        assert(rename(replacement_path,watched_path)==0);
        swap_on_stat=false;
    }
    return r;
}
static int watched_ioctl(int fd, unsigned long request, struct winsize *ws);
#define fstat watched_fstat
#define ioctl watched_ioctl
#define main editor_main
#ifndef EDITOR_SOURCE
#define EDITOR_SOURCE "../editor.c"
#endif
#include EDITOR_SOURCE
#undef main
#undef ioctl
#undef fstat
static int watched_ioctl(int fd, unsigned long request, struct winsize *ws)
{
    if (resize_during_query) {
        assert(request==TIOCGWINSZ);
        ws->ws_row=30; ws->ws_col=110;
        signal_handler(SIGWINCH);
        return 0;
    }
    return ioctl(fd,request,ws);
}
static void write_fixture(const char *path, const char *bytes)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
    assert(fd>=0);
    assert(write(fd,bytes,strlen(bytes))==(ssize_t)strlen(bytes));
    assert(close(fd)==0);
}
static void arm_swap(const char *path,const char *replacement)
{
    assert(stat(path,&watched)==0);
    watched_path=path;replacement_path=replacement;observations=0;swap_on_stat=true;
}
static void reset_doc(const char *bytes,size_t n)
{
    free_changes(history);history=current=history_tail=NULL;
    assert(copy_buffer(&text,bytes,n));assert(copy_buffer(&saved,bytes,n));
    free(filename);filename=NULL;free(loaded_journal_path);loaded_journal_path=NULL;
    loaded_journal_stat_valid=false;existed=false;
    cursor=top=left=goal=0;group=goal_set=false;
    journal_dirty=journal_branched=journal_rebased=journal_ignored=false;
    search_mark=SIZE_MAX;search_length=0;
}
static void race_tests(void)
{
    Buffer b={0};struct stat st;
    write_fixture("old.txt","old bytes");write_fixture("new.txt","replacement");
    arm_swap("old.txt","new.txt");
    assert(!read_file("old.txt",&b,&st));
    assert(!swap_on_stat && errno==EAGAIN);free(b.s);memset(&b,0,sizeof b);
    assert(!read_file(".",&b,&st) && errno==EISDIR);free(b.s);
    reset_doc("",0);write_fixture("current.txt","saved");read_document("current.txt");
    assert(filename && text.n==5 && !memcmp(text.s,"saved",5));
    write_fixture("incoming.txt","incoming");write_fixture("racing.txt","replacement");
    arm_swap("incoming.txt","racing.txt");read_document("incoming.txt");
    assert(!swap_on_stat && text.n==5 && !memcmp(text.s,"saved",5));
    assert(strstr(filename,"/current.txt") && strstr(message,"Current file retained"));
    assert(edit(5,0,"!",1,false));save_file(NULL,false);assert(!changed());
    {char *journal=journal_path(filename);assert(journal);
     write_fixture("journal-replacement","invalid journal");
     read_document("old.txt");assert(!history);
     arm_swap(journal,"journal-replacement");read_document("current.txt");
     assert(!swap_on_stat && journal_ignored && !history && text.n==6 && !memcmp(text.s,"saved!",6));
     free(journal);}
    puts("PASS deterministic pathname replacement during file read, read command and journal load; directory error");
}
static void ui_tests(void)
{
    resize_during_query=true;resized=1;dimensions();
    assert(resized && rows==30 && cols==110);resize_during_query=false;
    assert(load_user_commands());
    assert(change_user_command("save","goto eof"));
    command_set("undefine save \t ");execute();
    assert(user_command_index("save")<0 && command_lookup("save") && !strcmp(command_lookup("save")->name,"write"));
    reset_screen();rows=3;cols=80;previous_rows=calloc(3,sizeof *previous_rows);assert(previous_rows);previous_count=3;
    {Buffer out={0},line={0};assert(copy_buffer(&line,"same row",8));
     drawing_failed=false;draw_row(&out,0,&line,false);assert(out.n);out.n=0;
     draw_row(&out,0,&line,false);assert(!out.n);
     draw_row(&out,0,&line,true);assert(out.n);out.n=0;
     assert(copy_buffer(&line,"new text",8));draw_row(&out,0,&line,true);assert(out.n);out.n=0;
     draw_row(&out,0,&line,true);assert(!out.n);line.n=0;draw_row(&out,0,&line,true);assert(out.n);
     free(line.s);free(out.s);}
    reset_screen();puts("PASS resize during query, whitespace after undefine, row cache text/style/empty changes");
}
static uint32_t rng=0x5123a0f1u;
static unsigned rnd(void){rng=rng*1664525u+1013904223u;return rng>>8;}
static void check_lines(const char *model,size_t n)
{
    size_t i,lines=1,start=0;
    assert(text.n==n && !memcmp(text.s,model,n));
    for (i=0;i<n;i++) if (model[i]=='\n') lines++;
    assert(line_count()==lines);
    {size_t offset=(size_t)rnd()%(n+1),line=1;
     for(i=0;i<offset;i++) if(model[i]=='\n'){start=i+1;line++;}
     assert(layout_at(offset).line==line && line_start(offset)==start);
     assert(numbered_line(line)==start);}
}
static void model_test(void)
{
    enum { CAP=40000, RUNS=12000 };
    char *model=malloc(CAP),*before=malloc(CAP);size_t n=20000,i,k;
    static const unsigned char alphabet[]={'a','z','\r','\n','\t',0xc3,0xa9,0xff,0x1b,' '};
    assert(model && before);
    for(i=0;i<n;i++)model[i]=(char)alphabet[rnd()%sizeof alphabet];
    reset_doc(model,n);
    for(k=0;k<RUNS;k++){
        char add_bytes[12];size_t at=rnd()%(n+1),old=rnd()%12,add_n=rnd()%12,prior=n;
        if(old>n-at)old=n-at;
        if(n-old+add_n>=CAP)add_n=0;
        if(!old && !add_n)continue;
        for(i=0;i<add_n;i++)add_bytes[i]=(char)alphabet[rnd()%sizeof alphabet];
        memcpy(before,model,n);
        cursor=rnd()%(n+1);
        assert(edit(at,old,add_bytes,add_n,false));
        memmove(model+at+add_n,model+at+old,n-at-old);memcpy(model+at,add_bytes,add_n);n=n-old+add_n;
        check_lines(model,n);
        if(k%4==0){assert(history_step(false));check_lines(before,prior);
            assert(history_step(true));check_lines(model,n);}
        if(k%17==0){assert(history_step(false));memcpy(model,before,prior);n=prior;check_lines(model,n);}
    }
    free(model);free(before);
    printf("PASS %d deterministic mixed-byte edit iterations, line layout, undo/redo and branching\n",RUNS);
}
int main(void)
{
    assert(setlocale(LC_ALL,"C"));
    race_tests();ui_tests();model_test();
    free_changes(history);free(text.s);free(saved.s);free(filename);free(loaded_journal_path);
    for(size_t i=0;i<command_history_n;i++)free(command_history[i]);
    return 0;
}
