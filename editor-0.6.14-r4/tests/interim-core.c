/* Focused regressions for the retained 0.7.x fixes. Test-only syscall wrappers. */
#define _XOPEN_SOURCE 700
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static enum { NORMAL, FAIL_DIRSYNC, FAIL_FILESYNCH, SWAP_AFTER_SYNC, CHANGE_MODE_AFTER_RENAME } fault;
static struct stat target_dir;
static const char *target_path;
static bool triggered;
static int test_fsync(int fd)
{
    struct stat st;
    int r;
    assert(fstat(fd,&st)==0);
    if (fault==FAIL_FILESYNCH && S_ISREG(st.st_mode)) {
        triggered=true;errno=ENOSPC;return -1;
    }
    if (st.st_dev==target_dir.st_dev && st.st_ino==target_dir.st_ino && fault==FAIL_DIRSYNC) {
        triggered=true;errno=EIO;return -1;
    }
    r=fsync(fd);
    if (r==0 && fault==SWAP_AFTER_SYNC && st.st_dev==target_dir.st_dev && st.st_ino==target_dir.st_ino) {
        assert(rename("replacement.journal",target_path)==0);triggered=true;
    }
    return r;
}
static int test_rename(const char *old,const char *newname)
{
    int r=rename(old,newname);
    if (r==0 && fault==CHANGE_MODE_AFTER_RENAME && target_path && !strcmp(newname,target_path)) {
        assert(chmod(newname,0644)==0);triggered=true;
    }
    return r;
}
#define fsync test_fsync
#define rename test_rename
#define main editor_main
#ifndef EDITOR_SOURCE
#define EDITOR_SOURCE "../editor.c"
#endif
#include EDITOR_SOURCE
#undef main
#undef rename
#undef fsync
static void put_file(const char *path,const char *contents)
{
    FILE *f=fopen(path,"w");assert(f);assert(fputs(contents,f)>=0);assert(fclose(f)==0);assert(chmod(path,0600)==0);
}
static void expect_file(const char *path,const char *contents)
{
    struct stat st;Buffer b={0};assert(read_file(path,&b,&st));
    assert(b.n==strlen(contents) && !memcmp(b.s,contents,b.n));free(b.s);
}
static void reset_document(void)
{
    clear_prompt();free_changes(history);history=history_tail=current=NULL;
    free(filename);filename=NULL;free(loaded_journal_path);loaded_journal_path=NULL;
    assert(copy_buffer(&text,"",0));assert(copy_buffer(&saved,"",0));layout_reset();
    journal_dirty=journal_ignored=journal_branched=journal_rebased=loaded_journal_stat_valid=false;
    cursor=top=left=goal=0;group=goal_set=existed=false;
}
static void prep(const char *name)
{
    reset_document();put_file(name,"old");read_document(name);assert(filename && text.n==3);
    assert(edit(3,0,"!",1,false));save_file(NULL,false);
    assert(!changed() && !journal_dirty && loaded_journal_stat_valid);expect_file(name,"old!");
}
static void arm_fault(int which,const char *path)
{
    char *parent=strdup(path),*slash;assert(parent);slash=strrchr(parent,'/');assert(slash);*slash=0;
    assert(stat(parent,&target_dir)==0);free(parent);target_path=path;fault=which;triggered=false;
}
static void journal_failures(void)
{
    char *path;Change *node;Buffer before={0},after={0};struct stat st;
    prep("sync.txt");path=journal_path(filename);assert(path);node=current;
    assert(read_file(path,&before,&st));
    /* Before publication: ENOSPC must preserve the old journal and live chain. */
    assert(history_step(false));assert(history_step(true));
    arm_fault(FAIL_FILESYNCH,path);assert(!journal_save());assert(triggered && errno==ENOSPC);
    fault=NORMAL;assert(journal_dirty && current==node && loaded_journal_stat_valid);
    assert(read_file(path,&after,&st));assert(same_bytes(&before,&after));free(before.s);free(after.s);
    assert(journal_save() && !journal_dirty);
    puts("PASS file sync ENOSPC preserves old journal and live undo");
    /* After document commit: a journal sync failure is a warning, not a lost document. */
    assert(edit(4,0,"?",1,false));node=current;
    arm_fault(FAIL_DIRSYNC,path);save_file(NULL,false);assert(triggered);fault=NORMAL;
    expect_file("sync.txt","old!?");assert(!changed() && journal_dirty && !loaded_journal_stat_valid);
    assert(current==node && strstr(message,"history could not be persisted"));
    assert(journal_save() && !journal_dirty && loaded_journal_stat_valid);
    assert(history_step(false) && text.n==4);assert(history_step(true) && text.n==5);
    puts("PASS post-commit directory sync failure keeps dirty undo and retry succeeds");
    /* A replacement during sync must not be accepted as the saved journal. */
    put_file("replacement.journal","not our journal");arm_fault(SWAP_AFTER_SYNC,path);
    assert(!journal_save() && triggered && errno==EAGAIN);fault=NORMAL;
    assert(journal_dirty && !loaded_journal_stat_valid && current==node);
    expect_file(path,"not our journal");assert(journal_save());assert(!journal_dirty);
    puts("PASS journal replaced during directory sync detected; live chain intact");
    /* A chmod immediately after rename must not validate nonprivate state. */
    arm_fault(CHANGE_MODE_AFTER_RENAME,path);assert(!journal_save() && triggered && errno==EAGAIN);
    fault=NORMAL;assert(journal_dirty && !loaded_journal_stat_valid && current==node);
    assert(chmod(path,0600)==0);assert(journal_save());free(path);target_path=NULL;
    puts("PASS journal permission change after publication rejected");
}
static void corrupt_redo(void)
{
    /* Construct a validly checksummed EDUNDO02 with an impossible redo
     * offset, then externally change the document. Previously the parser
     * dropped the invalid redo tail before validating the chain. */
    char *path;int fd;JournalOutput out={0};
    prep("corrupt.txt");path=journal_path(filename);assert(path);
    fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0);out.fd=fd;out.hash=JOURNAL_HASH_INITIAL;
    assert(journal_write(&out,"EDUNDO02",8)==0);
    assert(journal_number(&out,strlen(filename))==0 && journal_number(&out,4)==0);
    assert(journal_number(&out,1)==0 && journal_number(&out,UINT64_MAX)==0);
    assert(journal_write(&out,filename,strlen(filename))==0 && journal_write(&out,"old!",4)==0);
    assert(journal_number(&out,7)==0 && journal_number(&out,0)==0 && journal_number(&out,1)==0);
    assert(journal_number(&out,0)==0 && journal_number(&out,1)==0 && journal_write(&out,"x",1)==0);
    assert(journal_flush(&out)==0 && put_number(fd,out.hash)==0 && close(fd)==0);
    put_file("corrupt.txt","outside");reset_document();read_document("corrupt.txt");
    assert(journal_ignored && !journal_rebased && !history && text.n==7 && !memcmp(text.s,"outside",7));
    free(path);puts("PASS malformed redo rejected before external-content rebase");
    /* Legitimate redo and outside rebase still work and use the old format. */
    prep("valid.txt");assert(history_step(false));save_file(NULL,false);assert(!changed());
    reset_document();read_document("valid.txt");assert(!journal_ignored && redo_depth()==1);
    assert(history_step(true) && text.n==4);assert(history_step(false) && text.n==3);
    assert(!changed());save_file(NULL,false);assert(!journal_dirty);
    put_file("valid.txt","outside");reset_document();read_document("valid.txt");
    assert(journal_rebased && !journal_ignored && text.n==7 && undo_depth()==1 && !redo_depth());
    assert(history_step(false) && text.n==3 && !memcmp(text.s,"old",3));
    assert(history_step(true) && text.n==7 && !memcmp(text.s,"outside",7));
    puts("PASS valid redo reopen and outside rebase, undo and redo");
}
static void limit_and_permissions(void)
{
    char *path;Change *old_chain,*old_tail,*old_current,*q;
    prep("limit.txt");path=journal_path(filename);assert(path);
    old_chain=history;old_tail=history_tail;old_current=current;
    /* Exercise the real count limit without creating large text copies. */
    for (size_t i=1;i<=JOURNAL_CHANGE_LIMIT;i++) {
        q=calloc(1,sizeof *q);assert(q);q->prev=history_tail;history_tail->next=q;
        q->ordinal=i+1;history_tail=q;current=q;
    }
    journal_dirty=true;errno=0;assert(!journal_save() && errno==EFBIG && journal_dirty);
    save_file(NULL,false);assert(strstr(message,"undo journal limit reached") && !changed());
    free_changes(old_tail->next);old_tail->next=NULL;history=old_chain;history_tail=old_tail;current=old_current;
    assert(journal_save() && !journal_dirty);free(path);
    puts("PASS journal count limit reports cause and retains live history");
    prep("writable.txt");assert(edit(text.n,0,"draft",5,false));put_file("protected.txt","protect");
    assert(chmod("protected.txt",0400)==0);save_file("protected.txt",false);
    assert(prompt==PROMPT_NONE && strstr(message,"not writable") && changed());
    expect_file("protected.txt","protect");expect_file("writable.txt","old!");
    puts("PASS read-only destination refused before overwrite prompt");
    reset_document();
}
int main(void)
{
    journal_failures();corrupt_redo();limit_and_permissions();
    free(text.s);free(saved.s);return 0;
}
