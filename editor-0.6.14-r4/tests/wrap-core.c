/* Display-only wrap regression tests. No production test hooks. */
#define main editor_main
#include "../editor.c"
#undef main
#include <assert.h>

static void doc(const char *s,size_t n)
{
    free_changes(history);history=current=history_tail=NULL;
    assert(copy_buffer(&text,s,n));assert(copy_buffer(&saved,s,n));
    cursor=top=left=goal=0;goal_set=group=false;
    search_mark=SIZE_MAX;search_length=0;
    journal_dirty=false;
}

static void select_toggle(int page)
{
    const MenuItem *items;size_t n,i;const char *title;
    menu_enter(page);items=menu_entries(&n,&title);
    for (i=0;i<n;i++) if (items[i].command && !strcmp(items[i].command,"toggle-wrap")) break;
    assert(i<n);menu_selection=i;menu_run();assert(menu_open);
}

static void check_cache(void)
{
    size_t p=0,q=0,iterations=0;
    /* Enumerate each row in order without relying on the cached lookup. */
    while (1) {
        WrapRow row=wrap_row(p);
        assert(row.end>=p && row.end<=text.n);
        assert(row.next==SIZE_MAX || row.next>p);
        assert(wrap_at(p)==p);
        if (row.end>p) assert(wrap_at(row.end-1)==p);
        if (p) assert(wrap_at(p-1)==q);
        if (row.next==SIZE_MAX) {assert(wrap_at(text.n)==p);break;}
        q=p;p=row.next;assert(++iterations<=text.n+1);
    }
}

int main(void)
{
    const char sample[]="alpha beta gamma delta\r\n\r\nshort\n";
    char *longline=malloc(120001);size_t i;
    WrapRow r;Buffer b={0};
    (void)setlocale(LC_CTYPE,"C.UTF-8");
    assert(longline);
    cols=12;rows=7;wrap_enabled=true;
    doc(sample,sizeof sample-1);
    r=wrap_row(0);assert(r.end==11 && r.next==11);
    r=wrap_row(11);assert(r.end==22 && r.next==24);
    assert(wrap_at(23)==11 && wrap_at(24)==24);
    assert(wrap_at(text.n)==text.n);assert(line_count()==4);
    cursor=2;vertical(1,1);assert(cursor==13 && current_line_number()==1);
    vertical(1,1);assert(cursor==24 && current_line_number()==2);
    vertical(1,1);assert(cursor==28 && current_line_number()==3);
    vertical(-1,3);assert(cursor==2);
    cursor=0;goal_set=false;vertical(1,99);assert(cursor==text.n);
    assert(!changed() && !history);
    cols=17;assert(wrap_at(13)==11);check_cache();
    cols=80;assert(wrap_at(13)==0);check_cache();
    puts("PASS word boundaries, CRLF, blank/EOF rows, logical line numbers, visual motion, resize");

    cols=12;doc("0123456789ABCDEFGHIJKL\n",23);
    assert(wrap_row(0).end==11 && wrap_row(11).end==22);
    assert(wrap_at(11)==11);assert(wrap_position(0,20)==10);
    doc("abc\tdef\tghi\001\xff\n",14);
    check_cache();
    if (wcwidth((wchar_t)0x4e2d)==2) {
        const char utf8[]="1234567890\xe4\xb8\xad\xe4\xb8\xad x\n";
        doc(utf8,sizeof utf8-1);assert(wrap_row(0).end==10);assert(wrap_at(11)==10);
        assert(wrap_position(10,1)==10 && wrap_position(10,2)==13);check_cache();
    }
    puts("PASS long words, tabs, visible controls, malformed bytes, UTF-8 wide characters (when supported)");
    for(i=0;i<120000;i++)longline[i]=i%13==0?' ':i%23==0?'\t':'a';
    longline[120000]=0;cols=80;doc(longline,120000);
    (void)wrap_at(text.n);assert(wrap_count>20);check_cache();
    for(i=0;i<8;i++) {
        size_t p=4096+i*12000;
        assert(edit(p,1,"\r\n\xe4\xb8\xad ",6,false));check_cache();
        assert(history_step(false));check_cache();
        assert(text.n==120000 && !memcmp(text.s,longline,120000));
        assert(history_step(true));check_cache();assert(history_step(false));
    }
    cols=37;check_cache();cols=120;check_cache();
    assert(!changed());free(longline);
    puts("PASS sparse wrapped-row cache after edits, undo/redo, width changes, long document byte preservation");

    /* Config absent -> default ON. Both menu paths invoke the same toggle. */
    doc(sample,sizeof sample-1);load_startup_config();assert(wrap_enabled && startup_wrap);
    cursor=13;select_toggle(MENU_HELP);assert(!wrap_enabled && !startup_wrap);
    select_toggle(MENU_SETTINGS);assert(wrap_enabled && startup_wrap);
    menu_open=false;load_startup_config();assert(wrap_enabled);
    assert(read_config("config",&b)==1 && strstr(b.s,"wrap=on"));free(b.s);
    show_settings();assert(strstr(viewer_text,"Saved word wrap: On") && strstr(viewer_text,"Current word wrap: On"));
    viewer_text=NULL;command_mode=false;menu_open=false;
    assert(!changed() && !history && cursor==13);
    cols=12;rows=7;top=0;mouse_enabled=mouse_pressed=true;mouse_button=0;mouse_x=3;mouse_y=2;
    mouse_key();assert(cursor==13);
    /* Render actual document rows and a logical status line through draw(). */
    tty_out=open("frame.txt",O_WRONLY|O_CREAT|O_TRUNC,0600);assert(tty_out>=0);
    draw();assert(strstr(previous_rows[0],"alpha beta ") && strstr(previous_rows[1],"gamma delta"));
    assert(strstr(previous_rows[rows-2],"1:14"));
    close(tty_out);tty_out=STDOUT_FILENO;reset_screen();
    assert(!changed() && !history);
    puts("PASS persistent toggle, both menu paths, settings reporting, mouse, actual row rendering, no undo/dirty side effects");
    free(text.s);free(saved.s);free_changes(history);
    return 0;
}
