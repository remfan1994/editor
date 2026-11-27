/* Targeted regression test for the 0.6.14 rollback, not the full old suite. */
#define main editor_main
#include "../editor.c"
#undef main
#include <assert.h>

static void reset_text(const char *s)
{
    free_changes(history); history=current=history_tail=NULL;
    assert(copy_buffer(&text,s,strlen(s)));
    assert(copy_buffer(&saved,s,strlen(s)));
    cursor=top=left=goal=0; group=goal_set=false;
    layout_reset();
    newline[0]='\n';newline[1]=0;newline_n=1;
    if (strstr(s,"\r\n")) {newline[0]='\r';newline[1]='\n';newline_n=2;}
}
static void cmd(const char *s)
{
    command_set(s);execute();
}
static void expect_text(const char *s)
{
    if(text.n!=strlen(s) || memcmp(text.s,s,text.n)) fprintf(stderr,"EXPECTED [%s] ACTUAL [%.*s] (%zu) STATUS %s\n",s,(int)text.n,text.s,text.n,message);
    assert(text.n==strlen(s)); assert(!memcmp(text.s,s,text.n));
}
static void expect_alias(const char *alias,const char *canonical)
{
    const CommandHelp *c=command_lookup(alias);
    assert(c && !strcmp(c->name,canonical));
}
static void expect_file(const char *path,const char *s)
{
    Buffer b={0};struct stat st;
    assert(read_file(path,&b,&st)); assert(b.n==strlen(s)); assert(!memcmp(b.s,s,b.n));
    free(b.s);
}
int main(void)
{
    const char *after;size_t a,b;
    char aliases[COMMAND_SIZE];
    assert(getenv("XDG_CONFIG_HOME") && getenv("XDG_STATE_HOME"));
    reset_text("one\r\ntwo\r\nthree\r\nfour");
    assert(line_count()==4);
    assert(line_range("2-eof",&after,&a,&b) && !*after && a==2 && b==4);
    assert(line_range(" BOF - END ",&after,&a,&b) && blank_tail(after) && a==1 && b==4);
    assert(!line_address("eofjunk",&after,&a));
    assert(!line_range("2-nonsense",&after,&a,&b));
    cmd("dline 3-eof");expect_text("one\r\ntwo");
    cmd("undo");expect_text("one\r\ntwo\r\nthree\r\nfour");
    cmd("redo");expect_text("one\r\ntwo");
    cmd("undo");cmd("dline 3-end");expect_text("one\r\ntwo");
    cmd("undo");cmd("goto eof");assert(cursor==17);
    cmd("eof");assert(cursor==text.n);cmd("bof");assert(cursor==0);
    cmd("dline eof-bof");expect_text("one\r\ntwo\r\nthree\r\nfour");
    cmd("dline top-bottom");expect_text("");cmd("undo");
    expect_text("one\r\ntwo\r\nthree\r\nfour");
    reset_text("a\nb\n");cmd("dline 2-eof");expect_text("a");cmd("undo");expect_text("a\nb\n");
    puts("PASS symbolic ranges, CRLF/LF, goto endpoints, invalid ranges, undo/redo");

    assert(load_user_commands());
    expect_alias("save","write");assert(user_command_name("save"));
    assert(!user_command_name("write"));assert(user_command_name("q!"));
    assert(change_user_command("save","goto eof"));
    assert(!command_lookup("SaVe"));assert(user_command_index("SAVE")>=0);
    effective_synonyms(canonical_lookup("write"),aliases,sizeof aliases);
    assert(!strstr(aliases,"save"));assert(!strcmp(aliases,"w"));
    cmd("save");assert(cursor==4); /* start of last, empty screen row */
    cmd("top");assert(execute_script_step("SAVE"));assert(cursor==4);
    assert(load_user_commands());assert(!command_lookup("save"));
    assert(!change_user_command("write","goto top"));
    assert(change_user_command("save",NULL));expect_alias("save","write");
    effective_synonyms(canonical_lookup("write"),aliases,sizeof aliases);
    assert(strstr(aliases,"save"));
    assert(change_user_command("q!","goto top"));assert(!command_lookup("q!"));
    cmd("q!");assert(running && cursor==0);assert(change_user_command("q!",NULL));
    expect_alias("q!","quit!");
    puts("PASS persistent custom synonym precedence, help aliases, canonical protection, undefine, script dispatch");

    reset_text("one\ntwo\nthree");
    assert(clipboard_choose("2"));
    cmd("copy 2-eof");
    {Buffer b={0};assert(clipboard_read_named("2",&b)==1);assert(b.n==9 && !memcmp(b.s,"two\nthree",9));free(b.s);}
    reset_text("first");cmd("bottom");cmd("paste");expect_text("two\nthree\nfirst");
    reset_text("one\ntwo\nthree");cmd("cut 2-eof");expect_text("one");cmd("undo");expect_text("one\ntwo\nthree");
    {char *path=clipboard_path(true);assert(path);assert(chmod(path,0400)==0);
     cmd("cut top-eof");expect_text("one\ntwo\nthree");assert(chmod(path,0600)==0);free(path);}
    puts("PASS symbolic copy/cut, selected-slot paste, failed clipboard cut preserves text");

    {FILE *f=fopen("readonly.txt","w");assert(f);assert(fputs("old",f)>=0);assert(fclose(f)==0);}
    assert(chmod("readonly.txt",0400)==0);
    reset_text("");read_document("readonly.txt");expect_text("old");
    assert(edit(0,3,"new",3,false));save_file(NULL,false);
    expect_file("readonly.txt","old");expect_text("new");assert(changed());assert(strstr(message,"not writable"));
    assert(chmod("readonly.txt",0600)==0);
    save_file(NULL,false); /* chmod changes observed metadata; approve if asked */
    if (changed()) save_file(NULL,true);
    expect_file("readonly.txt","new");assert(!changed());
    puts("PASS read-only save refusal and ordinary save after permission restored");

#ifndef EDITOR_NO_CMD
    reset_text("b\na\nc\n");assert(execute_script_step("filter-lines bof-eof sort"));expect_text("a\nb\nc\n");
    assert(insert_shell_output("printf X",0,0,false));expect_text("Xa\nb\nc\n");
    {int hold=dup(STDIN_FILENO);Buffer out={0};char report[256];assert(hold>=0);assert(close(STDIN_FILENO)==0);
     assert(process_capture("printf ok",NULL,0,LIMIT,&out,report,sizeof report));
     assert(dup2(hold,STDIN_FILENO)>=0);close(hold);assert(out.n==2 && !memcmp(out.s,"ok",2));free(out.s);}
    puts("PASS normal shell/filter and closed stdin descriptor regression");
    no_cmd=true;
#endif
    reset_text("keep");
    assert(!insert_shell_output("touch unexpected",0,0,false));
    assert(!execute_script_step("shell touch unexpected"));
    assert(change_user_command("custom","shell touch unexpected"));
    assert(binding_assign("F6","custom"));
    {char name[24];text_key(hotkey_code("F6",name));}
    cmd("custom");assert(!execute_script_step("custom"));
    cmd("filter touch unexpected");cmd("filter-lines 1-eof touch unexpected");
    assert(access("unexpected",F_OK)<0 && errno==ENOENT);expect_text("keep");
    assert(change_user_command("custom",NULL));
    puts("PASS no-cmd direct, script, custom command, hotkey and filter boundary");
    free_changes(history);free(text.s);free(saved.s);free(filename);free(loaded_journal_path);
    return 0;
}
