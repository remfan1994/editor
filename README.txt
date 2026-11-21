EDITOR(1)
editor 0.6.14 - a small, dependency-light, full-screen text editor

NAME
    editor - edit a text file using plain commands or a numbered menu

SYNOPSIS
    editor [--menu|--cli] [--mouse|--no-mouse] [--movement|--edit] [--no-keys] [FILE|-]
    editor --help | --manual | --about | --version

BUILD AND INSTALL (FIRST INSTALL OR UPDATE)
Run these commands from this extracted minimal-editors directory, as the
account that will use editor. The && operators stop the sequence if a step
fails, so a failed compilation does not install an older local executable.

    mkdir -p "$HOME/.local/bin" &&
    cc -std=c99 -O2 -Wall -Wextra -pedantic editor.c -o editor &&
    install -m 755 editor "$HOME/.local/bin/editor" &&
    "$HOME/.local/bin/editor" --version

For the standalone source download, substitute editor-0.6.14.c for editor.c
in the compilation command above.

Repeat this build-and-install sequence after each source update. Compiling
creates ./editor in the current directory; it does not update the installed
executable. The install command replaces $HOME/.local/bin/editor.

ONE-TIME PATH SETUP
To run editor by name, $HOME/.local/bin must be on PATH. If it is not already
present, add this line once to ~/.profile for your OpenBSD ksh login:

    export PATH="$HOME/.local/bin:$PATH"

Apply the profile to the current shell:

    . "$HOME/.profile"

Check which executable will run, then check its version:

    command -v editor
    editor --version

The first command should identify $HOME/.local/bin/editor. If it identifies
another copy or a shell alias/function, resolve that before relying on the
bare editor command. The explicit path $HOME/.local/bin/editor bypasses that
name lookup. Other shells may use a different login/startup file. PATH setup
is only needed once; do not append it again for each editor update.

SYSTEM-WIDE OPENBSD INSTALL (ROOT-OWNED)
For use from both a personal login and doas -u root -s:

    cc -std=c99 -O2 -Wall -Wextra -pedantic editor.c -o editor &&
    doas install -o root -g wheel -m 755 editor /usr/local/bin/editor &&
    /usr/local/bin/editor --version

From an existing root shell, omit doas. A separate ~/.local/bin/editor
copy is not updated by this command; check command -v editor.

RUN
    editor config.conf
    editor --menu config.conf

You can still run the uninstalled build as ./editor config.conf.

EXTERNAL COMMANDS AND USER-DEFINED COMMANDS

    shell ls -1
        Execute /bin/sh -c 'ls -1' and insert its standard output at cursor.
        With no argument, shell asks for the command at Shell>.

    filter sort
        Send the entire document through sort and replace it with stdout.

    filter-lines 3-9 sort -u
        Send only complete lines 3 through 9 to the command, replacing those
        lines with captured output. Use one line number for a single line.

    All three are available under Edit in the menu. Each successful operation
    is one undo step. The shell child runs with editor's OS privileges and
    working directory. When editor runs as root, the child also runs as root.
    Shell commands are therefore explicit only: opening a document, pasting,
    loading a config, and viewing a script as a document do not execute it.
    No terminal input is provided to shell commands. For filter, stdin is the
    selected document text. stdout is the only text inserted; stderr and exit
    status are kept separate. A nonzero exit, interrupt, allocation failure,
    excessive output, or NUL/escape/control byte rejects the whole edit.
    CR, LF and CRLF output is normalized to the document's newline style.
    Do not use curses or interactive programs like vi, less, or top this way.
    Filters may legitimately return empty output and thereby delete text.
    Undo reverses each successful insertion or replacement.

    define dirlist shell ls -1
        Save a custom editor command named dirlist; type dirlist to run it.

    define q1 quit!
        Save q1 as an explicit alias for discarding unsaved edits and exiting.
        q1 exists only after defining it. Plain quit still checks for changes.

    A directly invoked definition behaves like typing its command, including
    normal prompts. For example, define store write retains Filename> for an
    unnamed file. Definitions may call other definitions, up to eight levels.
    Script calls keep the restricted script rules even through nested aliases.
    The built-in files name already means ls; dirlist avoids that conflict.

    define stamp shell date
        Save a name for a shell-output insertion.

    functions
        List the saved definitions. functions reload rereads the file.
    undefine dirlist
        Remove a definition.

    run ~/commands.editor
        Execute a plaintext editor-command script, one instruction per line.
        # comments and empty lines are ignored. Example file:

            # Build a header
            shell date
            shell uname -a

    define header run ~/commands.editor
        Bind the script to a custom command named header. Type header to run.

    Only user-owned, non-group/other-writable regular script files are read;
    script file size is limited to 32 KiB. Scripts are never run automatically
    on startup or opening documents. A script can use shell, filter,
    filter-lines, find, next/prev, goto, top/bottom, undo/redo,
    movement/edit, run, and named custom commands. Other editor commands
    (including quit, read, write, define) are refused within scripts to
    prevent unexpected interactive prompts or document switches. A script
    stops on failure (including a find with no match), after no more than
    200 executed steps or 8 nested
    definitions/scripts. Earlier successful steps stay in the undo history,
    one entry per edit; a script is NOT an all-or-nothing transaction.

    Optional hotkeys can invoke defined commands: bind F2 dirlist. No default
    bindings are added; defining a command never assigns a key.

    Key binding saves detect another session changing keys.conf, including
    while a save is being staged. A refused save keeps the requested binding
    in this session and reports that it was not saved. Reopen editor to load
    the current file before saving more bindings. Malformed keymap content
    must be corrected first; saving never silently removes unknown entries.
    --no-keys also disables bind/unbind for that launch, so an unloaded map
    cannot be overwritten accidentally.

    User definitions are stored in $XDG_CONFIG_HOME/editor/commands.conf,
    or ~/.config/editor/commands.conf (not in config or keys.conf). This is
    inert text loaded at startup. Names begin with a lowercase ASCII letter
    and continue with lowercase letters, digits, hyphen or underscore,
    maximum 31 characters. They cannot replace a built-in command or alias.
    Each definition is a single editor command line, up to 2047 bytes.
    Built-ins always win. User commands currently take no arguments; script
    files provide multi-step behavior without introducing a new language.
    Up to 32 definitions are saved atomically. A stale editor refuses to
    overwrite externally changed command definitions: functions reload
    refreshes them. File permissions and errors are reported instead of
    silently executing or ignoring unsafe definitions.

    For shell and filter, /bin/sh interprets the command text exactly as
    requested. Do not run untrusted command strings or pass untrusted shell
    fragments. Running a root-owned editor means shell commands run as root.


DESCRIPTION
    editor is a small C text editor for configuration files and other
    ordinary text. It needs the Unix terminal interface and system C library,
    but not curses, ncurses, terminfo, libedit, readline, or a pager.

    The program edits one file at a time. It has no default user-command
    hotkeys. Esc enters the command prompt by default; the optional menu
    provides another way to invoke the same operations. Basic typing,
    arrows, Home/End, Page Up/Down, Backspace and Delete work in the document.

MOVEMENT MODE
    Default is ordinary text editing: typing inserts text, and Backspace,
    Delete, and Enter edit text. To enter the navigation-only mode, type
    'movement' at Command>, or choose Controls > Current editing. The 'edit'
    command returns to typing; Controls > Current editing offers one switch. A startup option also exists:

        editor --movement example.conf

    Movement mode keys are ONLY navigation controls within that mode;
    none are global hotkeys and the user-defined keymap remains empty.
    Holding keys uses terminal keyboard autorepeat:

        Left / h / b / Ctrl+B    Move left one character
        Down / j / n / Ctrl+N    Move down one display line
        Up / k / p / Ctrl+P      Move up one display line
        Right / l / f / Ctrl+F   Move right one character
        Home / 0 / a / Ctrl+A   Beginning of current line
        End / $ / e / Ctrl+E    End of current line
        g / G                   Beginning / end of document
        PageUp / y / Ctrl+Y     Scroll up one page
        PageDown / v / Ctrl+V   Scroll down one page
        w / @                   Move to next word
        Enter                   Leave movement mode; resume typing
        Esc                     Open Command> or the optional menu

    Typing, pasted text, Delete and Backspace cannot modify the file while
    movement mode is active. Use 'edit' to return to typing from a command.
    The former 'mode' command is removed: use movement and edit directly.
    A short MOVE indicator appears on the bottom line (including narrow
    terminals). If the editor is editing clipboard.txt, the same mode applies.
    This navigation mode is independent of Reverse (the undo-history browser).
    Search commands 'next/n' and 'prev/p' still operate from Command>;
    within movement mode, the single letters n and p move down/up.

PROGRAM SETTINGS AND CONFIGURATION
    settings opens Help > Settings. Each switch occupies ONE row, showing
    its current choice. Selecting it switches to the other choice and saves.
    It does not open separate entries for each possible value.

        toggle-esc          Esc opens: Command line / Menu.
        toggle-startup      Next launch: Edit / Movement.
        toggle-mouse        Mouse input: Off / On.
        toggle-signals      Interrupt: Exit / Cancel input.
        toggle-cliphistory  Clipboard history: Off / On.

    All five commands take NO arguments. Each switches one setting, saves it,
    and reports the result. A second invocation switches it back. Failed
    saves do not silently apply session-only changes. The Settings menu calls
    those exact commands and retains the selected row after success. Errors
    stay visible instead of disappearing behind the menu.

    toggle-startup changes the NEXT launch, not this session's movement/edit
    mode. The other switches apply now as well as after restarting. The
    current-session commands movement and edit remain explicit operations;
    Controls > Current editing presents them as one switch without saving.

    settings show is read-only. It reports the actual config path, saved
    choices, current choices and clipboard-history state. The menu has a
    Current / saved settings entry for this same report. help settings and
    help toggle-startup (or any other toggle) explain purpose and examples.
    Tab completes names, but never runs them. settings alone opens the menu.

    The default config path is ~/.config/editor/config. With an absolute
    XDG_CONFIG_HOME, it is $XDG_CONFIG_HOME/editor/config instead. Example:

        startup=movement
        esc=menu
        mouse=off
        signal_controls=off

    Defaults: startup=edit, esc=cli, mouse=off, signal_controls=off. Existing
    startup/esc/signal_controls config keys keep their meanings. Mouse is now
    saved here as well. Clipboard-history preference retains its existing
    clipboard-history.enabled file alongside clipboard data in the state
    directory; existing installations do not need to migrate it. Default is
    Off, and turning it off does not delete stored snapshots.

    Blank lines, # comments, spaces around '=', CRLF and a missing final
    newline are accepted. Unknown/invalid entries warn. Config is inert text,
    never shell code. The maximum size is 32 KiB; NUL/nonregular files are
    refused. Owned 0755 directories and 0644 files work. Group/other writable
    paths, foreign ownership and symlinked final config paths are refused.
    A missing config means defaults; an unreadable config produces an error.

    Setting changes preserve comments, unrelated lines and line endings.
    Duplicate occurrences of a changed key are updated. Replacement is staged
    and intervening config changes are checked. Failures before replacement
    retain the prior file and live behavior. Post-commit directory-sync
    failures warn about durability rather than claiming nothing was saved.
    New/replaced config files use 0600; existing directory permissions remain.
    Hard-linked config files are not replaced. Final writer races remain.

    --movement/--edit, --menu/--cli and --mouse/--no-mouse override one launch
    without writing preferences. Last flag in each pair wins. A toggle uses
    the displayed live choice for Esc/mouse/signals; toggle-startup uses the
    latest saved startup choice. --no-keys disables user keybindings only.
    -- ends option parsing for filenames beginning with a dash.

    MIGRATION FROM 0.6.9
    The old startup, toggle-mode, mouse and signals commands do not execute.
    They show the new toggle name and retain the draft for correction; no
    old explicit on/off request is silently converted into a toggle. Likewise,
    cliphistory now only browses snapshots; toggle-cliphistory changes whether
    they are collected. Update your own keys.conf entries that use retired
    commands. No default keybindings are installed. Keep startup=movement in
    config as written: it is a setting key, not the retired startup command.

TERMINAL SIGNAL SETTINGS
    Ctrl+C and Ctrl+Z are never editor command bindings. Editor preserves the
    terminal's ISIG flag and configured interrupt/suspend characters instead
    of forcing control-byte values. With ordinary terminal settings, Ctrl+C
    generates an interrupt and Ctrl+Z requests suspension. Remapped or
    deliberately disabled terminal controls remain remapped or disabled.

    The optional application signal setting is OFF by default:

        signal_controls=off

    Set it in the same editor/config file as startup=movement or startup=edit.
    OFF means interrupt terminates editor after terminal cleanup; suspension
    restores the terminal and stops the job. Resume with the shell's fg.
    A suspended filename prompt or confirmation remains in place by default.
    No save is performed on interrupt: unsaved document AND clipboard edits
    are lost. Use write before terminating, or suspend to retain the process.

        signal_controls=on

    ON opts into application cancellation: an interrupt cancels pending input
    and returns to editing/Command> instead of terminating. A stop signal still
    suspends; an active yes/no confirmation is cancelled before suspension.
    This switch does NOT install Ctrl-key bindings, disable terminal signals,
    or override the configured interrupt/suspend characters.

    settings show reports handling without changing it. toggle-signals
    switches signal_controls=on/off, saving before applying. The preference
    survives restarting editor. Help > Settings > Signals uses the same
    saved operation. A failure is shown explicitly and retains prior behavior.
    This updates only the signal_controls key, not startup or Esc preferences.
    Esc remains the ordinary cancel/back control.
    Ctrl+C/Z bindings in older keys.conf files are ignored; attempts to bind
    them are refused. The rest of your user-defined keymap is unchanged.

ORDINARY QUIT NOTICE
    A successful quit (including its q/exit aliases or the menu's Quit) prints
    the BLOODGUILT CURSE notice after restoring the terminal. It is a small
    plain-text exit notice, with no delay, key acknowledgement, or network use.
    The existing About notice remains, and no startup screen is added.

    quit!, confirmed double-Esc exits (Really quit? Y/n), menu discard exits,
    input loss and signal exits skip the notice. Cancelled or refused quit
    attempts also do not print it. It never goes into the document or stdout
    pipe: pipeline users receive only their explicit write - document bytes.

COMMANDS
    Press Esc, type a command, and press Enter. Esc cancels a nonempty
    command. A second Esc at an empty Command> asks Really quit? (Y/n).
    Only Y/y quits; any other ordinary key returns to editing. Terminal
    interrupt/stop behavior follows TERMINAL SIGNAL SETTINGS above. The
    default interrupt exits, including at this prompt, without a notice.
    Tab completes a command name without executing it: he + Tab becomes
    help, li + Tab becomes list. Unambiguous aliases work too (sa becomes
    save); primary command names take precedence over aliases. When
    several commands match, Tab shows their names AND what they do. Esc/q
    returns to your unchanged command; add letters and press Tab again.
    Tab at an empty prompt opens the command guide. It also completes
    command names after help/list (and their aliases) and in 'N dline'.
    Tab also completes pathnames after read/write/ls, including ~/ and
    filenames with spaces. Completing a directory lists its contents in
    the scrollable viewer, without suspending editor. Tab in the document
    still inserts a literal tab character.
    The Up and Down arrows recall commands, with a history of up to 32
    distinct entries; repeating one makes it the most recent entry.

    write             Save the active file; if unnamed, ask for a filename.
    write FILE        Save under FILE; after successful save, FILE becomes
                      the active file for all subsequent plain writes.
                      If FILE exists and is not the active file, ask first
                      before overwriting; N asks for another filename.
    write -           Write exact text bytes to redirected stdout.
    read FILE         Open another file if the current edits are saved.
    read              Prompt for a file to open.
    ls [DIRECTORY]    Display a scrollable directory listing; default '.'
    find TEXT         Find literal, case-sensitive TEXT from cursor;
                      wraps at the end. Move cursor to match and highlight.
    find              Prompt for text to find (Find>); Esc cancels.
    next / n          Find the next occurrence, wrapping if necessary.
    prev / p          Find the previous occurrence, wrapping if necessary.
    replace           Prompt for search and replacement (first match).
    replaceall        Prompt to replace all matching text (one undo).
    replace OLD => NEW     Replace first match starting at cursor.
    replaceall OLD => NEW  Replace every non-overlapping match.
    copy              Copy the current line into the selected clipboard.
    copy N            Copy line N (N is a line, not a clipboard slot).
    copy A-B          Copy inclusive lines A-B into the selected clipboard.
    cut [N|A-B]       Copy a line or range, then delete as one undo.
    paste             Insert from the selected clipboard before this line.
    paste SLOT        Insert directly from SLOT, without changing selection.
    clipboard         Open/close the selected clipboard's own text editor.
    clipboard SLOT    Select a persistent clipboard, e.g. clipboard 2.
    clipboard default Return to the original unnamed clipboard.txt slot.
    clipboard list    Browse all saved clipboard slots.
    cliphistory       Browse prior clips in a read-only viewer.
    toggle-cliphistory  Switch and save history collection (max 64 entries).
    clipuse [N]       Restore clipboard snapshot N, newest first; ask if absent.
    dline             Delete the cursor's entire line.
    dline 30          Delete line 30.
    dline 30-43       Delete lines 30 through 43, inclusive (14 lines).
    deleol            Delete from cursor to end of line; keep LF/CRLF.
    deleof            Delete from cursor to end of document (EOF).
    backbof           Backspace from cursor to beginning of file (BOF).
    13 dline          Delete 13 lines starting at the cursor's line.
    undo [N]          Undo N operations (default 1; stop at oldest).
    redo [N]          Redo N operations (default 1; stop at newest).
    history           Show undo/redo counts.
    reverse           Browse undo/redo while viewing the actual document.
    reverse 0         Jump to the oldest undo register position.
    reverse N         Jump to absolute history position N (0 = oldest).
    goto              Ask for a line and optional column.
    goto 50           Move to line 50.
    goto 50 12        Move to line 50, screen column 12.
    top / bottom      Go to start / end of document.
    left / right [N]  Move by N characters (default 1).
    up / down [N]     Move by N display lines (default 1).
    pageup/pagedown   Move by a screenful.
    movement          Enter navigation-only control (aliases: move, navigation).
    edit              Return to typing (aliases: editmode, insertmode, typing).
    toggle-esc        Switch and save Esc: Command line / Menu.
    toggle-startup    Switch and save next launch: Edit / Movement.
    toggle-mouse      Switch and save mouse input: Off / On.
    toggle-signals    Switch and save interrupt: Exit / Cancel input.
    settings          Open Help > Settings without changing preferences.
    settings show     Report saved/current values and actual config path.
    bind KEY COMMAND  Create a user-defined hotkey for a built-in command.
    unbind KEY        Remove a user-defined binding.
    bindings          Display all user-defined bindings.
    suspend           Suspend and restore terminal state; resume with fg.
    about             Show credits and BLOODGUILT CURSE notice.
    list              All commands grouped by task, with explanations.
    list TOPIC        Filter by words in names, synonyms or descriptions.
    help              Starting guide and ways to get instructions.
    help COMMAND      Purpose, examples, details, syntax and synonyms.
    help TOPIC        Find commands about a topic when it is not a name.
    manual            Open the built-in manual.
    quit              Leave when saved, then print the non-pausing exit notice.
    quit!             Leave, discarding unsaved edits.

FILENAME COMPLETION AND INPUT FAILURES
    Tab completes the final pathname after read/write/ls, or the input in a
    Filename> prompt. Complete one common prefix or unique name at a time;
    directories gain '/' and their contents are displayed. Closing a listing
    returns to the unfinished input. Filenames containing spaces are literal.

    Completion does not guess a unique name from a failed directory scan.
    Memory errors or interruption during matching, and names that will not
    fit, leave the original command untouched. If completion succeeded but
    listing the completed directory later fails, the valid completed path is
    retained. The error is displayed in a scrollable notice,
    not hidden behind Command>. Esc/q returns to the same draft and prompt.
    Ambiguous UTF-8 names never add half of a valid character. Already typed
    bytes are not removed; non-UTF-8 filenames are still treated as bytes.

    Directory listings retain their 1024-entry and fixed display-buffer bounds.
    Truncation is explicit; an unsuccessful scan is not shown as a complete
    list. Large directories can take time to scan, and directory contents can
    change after completion. Writing still requires the normal destination
    and outside-change checks; completion never performs a write.

    Command entry and its argument prompts accept up to 4095 bytes. A bracketed
    paste that exceeds the space remaining, contains NUL/forbidden control
    input, or supplies multiple command lines is rejected as a whole. The
    paste end marker is consumed, and the existing draft remains available.
    Esc/q closes the error notice. This does not change the document paste
    limit or make unbracketed paste distinguishable from ordinary typing.

FIND AND REPLACE (LITERAL TEXT)
    Search is case-sensitive and literal, not regex. The remembered search
    term remains available after edits. Matches wrap at document boundaries.
    The bare find command opens Find>, which accepts its contents literally:
    leading/trailing spaces and a search consisting only of spaces are kept.
    Empty Enter cancels that prompt without changing the previous search.
    Inline find TEXT still treats separators after the command as syntax;
    use Find> when the search starts with spaces.

        find CONFIG        Search for CONFIG starting at cursor.
        find               Open the Find> prompt.
        next               Move to next match (short alias n).
        prev               Move to previous match (short alias p).
        replace            Prompt for Find text> and Replace with>.
        replaceall         Prompt to replace all occurrences.
        replace OLD => NEW       Replace first match, wrapping from cursor.
        replaceall OLD => NEW    Replace all non-overlapping matches.

    Search checks the relevant range before wrapping, not an unrelated
    whole-document scan on each next/previous operation. Preparing a
    replace-all result can be interrupted before it becomes an edit: the
    original text, cursor and undo chain remain intact when preparation is
    cancelled. Native Ctrl+C still exits by default and loses unsaved edits;
    signal_controls=on is the optional cancel-instead-of-exit preference.
    Suspension and termination are also checked during long preparations.

    Aliases for next: n, findnext, nextmatch, nextfind, find-next.
    Aliases for prev: p, previous, findprev, prevmatch, previousmatch,
    find-prev, previousfind. The names next and previous now search matches;
    use up/down or moveup/movedown for vertical cursor movement.

    The exact delimiter for inline replacement is one space, =>, one space.
    OLD and NEW may contain spaces. To replace with an empty string, use the
    prompt and submit an empty Replace with> line, or end with ' => '.
    Without an inline delimiter, replace OLD prompts for the new text.
    Search and replacement may not contain embedded newlines or NUL bytes.

    Replace affects the active document only, is case-sensitive, and uses the
    ordinary undo engine. replaceall commits all changes as ONE undo step;
    undo restores the complete original text. Replacements that would exceed
    the 8 MiB document limit are refused without modifying it. A no-match
    search or replacement leaves the document unchanged. If replacing all,
    matches are non-overlapping; inserted text is never rescanned.

    Find, next, and prev are read-only operations. Search highlights are
    display-only, and cursor movement does not write a file. Temporarily
    editing a clipboard preserves the original document's remembered search.

    Menu path: Move > Find text / Next match / Previous match;
    Edit > Replace / Replace all.

REVERSE (UNDO REGISTER)
    Type reverse to enter, or select Reverse from the numbered menu.
    The displayed register reads REVERSE current/total. Position 0 is
    the oldest saved undo state; total is the newest state. Pressing a key
    changes the visible document, but does not write it to disk.

        Left / h / < / -  Undo one transaction (move toward 0).
        Right / l / > / + Redo one transaction (move toward total).
        Home / End        Jump to oldest / newest history state.
        Esc               Exit Reverse, keeping selected contents.

    Holding any movement key uses terminal autorepeat at one undo/redo
    step per event. Digits 0-9 have NO special meaning in Reverse.
    From the command prompt, reverse N jumps directly to position N
    and enters Reverse mode; reverse 0 goes to the oldest position.
    The older command name logbrowser remains as a compatibility alias.
    Use help reverse to see the full command syntax and aliases.

    Outside changes recorded between sessions are one undoable change.
    If you confirm overwriting a live outside edit, undo once restores
    that outside version; undo again returns to the previous editor chain.

LINE DELETION
    A number before dline is a count relative to the cursor; a number after
    dline is an absolute line number. A dash denotes an inclusive range.

        13 dline          Delete the cursor line and the 12 following lines.
        dline 13          Delete only numbered line 13.
        dline 30-43       Delete 14 numbered lines.

    Synonyms for dline include delline, delete-line, delete, and remove-line.
    These forms also accept the count or range syntax.
    An invalid or out-of-bounds range changes nothing. Counts must fit
    entirely within the remaining document. Deleting a range, even a large
    one, is a SINGLE undo step. LF/CRLF and final-newline handling are
    preserved in the remaining file as appropriate.

    Deletion to a boundary uses the cursor position, not a line number:
        deleol   Delete up to end of current line; preserve newline bytes.
        deleof   Delete up to end of the entire document, including newlines.
        backbof  Delete from start of file to cursor (backspace direction).

    Each operation is one transaction for undo/redo, including after saving
    and reopening the file. At a boundary the command is a no-op. Existing
    CRLF separators are not converted, and line-ending styles are preserved.
    Aliases: deleol/delete-to-eol; deleof/delete-to-end; backbof/
    backspace-to-start. 'help deleol', etc. lists every accepted synonym.

    Example: "help dline" lists the exact forms and synonyms. More aliases
    exist for many commands: write/save/w, goto/go/line/jump, quit/exit/q.
    The list command explains the canonical commands by task; help NAME
    gives concrete examples and lists its aliases after the explanations.
    Synonyms are alternate spellings of the same operation, not key codes.

MENU
    Start with "editor --menu file.conf", or use toggle-esc to switch and
    save the Esc choice. toggle-esc switches back when repeated.
    The main menu is grouped rather than a long flat list:

       1 Write             Save the current file (or choose another name)
       2 Files >           Read, ls, clipboards, clipboard history
       3 Edit >            Undo, redo, range and boundary deletions
       4 Move >            Find, next/previous match, goto, movement
       5 History >         Reverse, numbered history jump, history counts
       6 Controls >        Current editing switch, write to pipe, suspend
       7 Help >            Guides, manual, About, Settings >
       8 Command line      Open a command prompt
       9 Exit >            Normal quit or confirmed discard and quit

    Menus support Up/k and Down/j, Enter/Right/l to select, Esc/Left/h
    to go back one level, and numbered choices followed by Enter.
    Menu subpages use the same control methods and are scrollable.
    A small header indicates the current selection and total menu entries,
    including on narrow screens. Click menu entries if mouse is enabled.
    All editing and file-management commands are reachable from the menu.
    Commands needing an argument have a specific labeled input prompt,
    e.g. Go to line, Delete range, Undo count, Bind key, Command or topic.
    The same command parser handles both interfaces. Esc cancels an input.

    Menu Write on an unnamed document requests a filename. On an existing
    active file, Write asks "Overwrite existing file? Y/n"; N asks for a
    different name. Saving another file makes it the active file on success.
    There is no separate Write as menu action. "Write to pipe" is under
    Controls for the case where stdout has been redirected.

CLIPBOARD AND TEMPORARY CLIPBOARD EDITING
    Clipboard slots are regular plaintext files in private state storage:
      $XDG_STATE_HOME/editor/clipboard.txt       (default slot)
      $XDG_STATE_HOME/editor/clipboard-2.txt     (named slot 2)
    or ~/.local/state/editor/ when XDG_STATE_HOME is unset.
    It is created with mode 0600; existing clipboard files must be regular,
    owned by the current user, and not accessible to group/others.
    This is an editor clipboard, not the desktop system clipboard.

    clipboard N selects slot N for future copy/cut/plain paste; that choice
    is remembered across restarts. Slot names may be 1-32 ASCII letters,
    digits, '-' or '_'. The name 'selected' is reserved for internal state.
    'default' selects clipboard.txt, while 'list'
    displays the stored slots. Plain 'clipboard' toggles the selected
    clipboard's editor; aliases include cb, cboard, clip, and editclip.
    A named slot is separate from the optional clipboard history, which
    stores past values from any slot, newest first, when explicitly enabled.

    copy/cut accept the current line, a numbered line, or an inclusive
    numbered range. 'cut 2' cuts line 2 into the selected clipboard,
    never into slot 2. 'paste 2' inserts the contents of slot 2 directly
    without selecting it; 'paste' uses the currently selected slot.
    Cut changes the original document as a single undo
    transaction. Paste inserts before the current line in one undo step.
    Line separators (including CRLF) and a missing final newline are
    preserved as far as the inserted lines permit. Paste is positional:
    cutting lines and immediately pasting does not necessarily put them
    back at their original position; use undo for exact restoration.

    The clipboard command opens the selected clipboard file in the same screen
    WITHOUT closing the original file. The original file's buffer, unsaved
    text, cursor/scroll position, filename, and complete undo chain remain
    in memory. In the clipboard editor:
      write           Save the selected clipboard and KEEP editing it.
      clipboard       Save pending changes automatically and return to
                      the original document (aliases: clip, cb, cboard).
      quit / quit!    Exit editor itself, not just the clipboard.
    Returning with unsaved clipboard changes requires no confirmation;
    The selected clipboard is a scratch file. If saving fails, the draft
    remains open and the original file remains intact. When another process
    changed the selected clipboard, the latest draft overwrites it without
    a question, unlike ordinary documents. An optional clipboard history
    can retain prior versions, but it is off by default.
    read and saving under another pathname are blocked while inside the
    clipboard sub-editor. The footer shows operation/error messages first;
    otherwise it identifies the return destination.

    Optional clipboard history is OFF by default. toggle-cliphistory
    switches and saves collection. cliphistory lists snapshots; clipuse N
    restores one. Turning collection off retains existing snapshots.
    At most 64 snapshots are retained; older snapshots are deleted as
    needed after a successful complete scan. A failed initial scan does not
    remove snapshots or supply a partial numbered list for restoration.
    History entries must be private, owned regular files. An unreadable
    preview is labeled "preview unavailable" instead of hiding its number;
    clipuse still reads and validates the complete selected entry.
    Numbers are newest-first at each command; another session may change
    history between a listing and a later restore. This is not a lock.
    If cleanup fails after a new snapshot is written, history can temporarily
    exceed 64 entries; the warning reports incomplete saving/cleanup.
    Individual clips are limited by the 8 MiB text-file limit.
    Previous clipboard contents may contain deleted plaintext; protect
    these private history files appropriately or disable the feature.

FILESYSTEM COMPLETION AND LISTING
    read FILE opens a specified file; read alone prompts for a filename.
    Existing unsaved changes block read. The explicit filename option
    takes precedence over redirected stdin.

    ls [PATH] opens a sorted, scrollable directory viewer; use j/k or
    Up/Down for rows, h/l or Page Up/Down for larger movements, Esc/q to
    close. At Command>, pressing Tab after read, write, or ls completes
    filesystem paths. A unique directory match appends / and opens its
    directory listing so a filename can be verified without suspending.
    Multiple matches show the containing directory while preserving the
    unfinished command. Filename> also accepts Tab completion. Paths with
    spaces are accepted, and ~/ expands to the current user's home directory.
    Path strings are treated literally; no shell command is executed.

HELP, LIST AND SMALL WINDOWS
    Commands are an interface for doing work, not merely names for binding
    keys. Use help for a short starting guide and list for a task directory.
    Every command has a brief purpose; details show runnable examples first,
    then rules, argument syntax and all its synonyms.

        list                 All commands grouped by task
        list clipboard       Narrow the list to clipboard-related commands
        list save            Search purposes and synonyms as well as names
        list delete cursor   Match both words across the command information
        help cut             Examples explaining which lines will be cut
        help save            The write command, reached by its save synonym
        help saving          Search for a topic rather than an exact command

    Topic matching is literal, case-insensitive filtering, not a shell or
    natural-language command executor. A missing match suggests broadening
    the query. Brackets mean an optional argument, not something to type.
    For example, write [FILE] means either write or write notes.txt.

    Esc/q from help, list and command-specific guidance returns directly to
    Command>. You can use the command you have just learned without leaving
    and reopening the command interface. Enter on an empty Command> returns
    to the document. Esc at an empty prompt retains the existing Really quit?
    confirmation; only Y/y quits. These pages do not change the preference
    for whether Esc normally opens the menu or the command line.

    Unknown command names give read-only suggestions and retain the entire
    draft, including filenames, spaces and cursor position. Esc/q returns
    to that draft for correction. Invalid navigation/count arguments and
    unexpected arguments to no-argument commands also show examples rather
    than throwing the command away. Suggestions are never executed or
    silently substituted. The editor does not guess filenames or change
    text, clipboard numbers, or replacement strings.

    A bare goto asks for a line and optional column. A bare clipuse asks
    for a history snapshot number. Existing read, write, find and replace
    prompts continue to work; these prompts reuse the same input editor as
    menu actions. Esc cancels without applying the operation.

    All read-only viewers accept j/k for rows, h/l for pages, arrows,
    Page Up/Down and Home/End. Esc or q closes the viewer. Pages wrap at
    the current terminal width rather than clipping to a status line.
    Help/list resume the command prompt; Manual/About retain their previous
    return to the document. Filename completion returns to its own input.

    Menus scroll when space is short. A 12-column, three-row terminal shows
    one selected entry or wrapped viewer row at a time. Use list TOPIC to
    reduce scrolling. No text-mode interface can show a long explanation
    all at once on that size of screen. No new hotkeys are assigned.

MOUSE (OPTIONAL)
    toggle-mouse switches and saves mouse input. --mouse/--no-mouse
    overrides the saved value for one launch without writing config.
    Mouse support starts OFF. Left click in text places the cursor; the
    wheel scrolls; clicking the status row opens the menu; clicking a menu
    item activates it. This uses VT/xterm mouse sequences. Some terminals
    reserve mouse events for text selection. No operation requires a mouse.

USER-DEFINED HOTKEYS
    None are assigned by default. Developers do not reserve command
    shortcuts for Undo, Write, or anything else. The user may assign
    optional bindings using commands such as:

        bind Ctrl+U undo
        bind F5 write
        unbind Ctrl+U
        bindings

    Available forms include Ctrl+A-Z except the terminal's editing/control
    keys C/H/I/J/M/Z, Alt+A-Z, F1-F12, and Ctrl/Alt+arrow when reported by the
    terminal. Ordinary text editing keys remain editing keys. Bindings
    apply ONLY while editing text, not in menus or prompts; actions must
    be built-in commands. The private bindings file is:

        ~/.config/editor/keys.conf

    or $XDG_CONFIG_HOME/editor/keys.conf if set. The --no-keys option
    disables loading bindings for that invocation, useful for recovery.

PIPES AND FILTERS
    You can launch editor with data produced by another program:

        grep ERROR server.log | editor

    This opens matching lines in an unnamed document, rather than parsing
    them as editor commands. The editor reads the stream first (8 MiB max).
    Its interactive screen and input go to /dev/tty, so the document and
    screen-control sequences do not get mixed into stdout. When editing is
    finished, use one of these commands:

        write matches.txt     Save to disk
        write -               Emit raw text to stdout for a following command

    Example pipeline with outgoing data:

        grep ERROR server.log | editor | sort > errors.txt

    In this case, type "write -" inside editor to send its edited buffer
    down the pipeline. No document bytes or notice are sent to stdout on quit.
    The ordinary-quit notice goes only to the controlling terminal. A working
    controlling terminal is necessary; batch/headless use is not supported.
    If stdout is the terminal, "write -" is refused to avoid escape junk
    and unintentional dumping of the buffer onto the editing screen.

SAVING AND UNDO
    Files are staged then replaced on write. If another program changes
    the opened file, write asks:

        File written since read. Overwrite y/n?

    Only Y/y explicitly approves replacing the observed disk version.
    N offers a different filename instead. Esc cancels. Terminal interrupt
    exits by default, or cancels with signal_controls=on; stop suspends.
    If the disk changes again before replacement, the program
    asks again rather than reusing earlier approval. No guarantee is
    possible against every simultaneous writer at the rename boundary.
    Multiple hard links remain refused.

    A separate write FILE to an existing other destination also asks:

        Overwrite existing file? Y/n

    Y permits replacing only the version just checked; other responses
    offer a new filename. Writing to a different destination leaves the
    previously active file unchanged. The new destination becomes active only
    after it is successfully written. An approved overwrite replaces that
    destination; its previous contents can be restored through undo.
    Existing permissions and ownership are retained where permitted;
    set-ID bits are cleared. Files' ACLs and extended attributes are not
    copied. Saving a file does not clear its undo history.

    After successful disk saves, the editor also attempts to persist its
    undo/redo transactions, which can be restored next week or in another
    session if the file bytes still match. The separate state directory is:

        ~/.local/state/editor/history/

    or $XDG_STATE_HOME/editor/history/ when set. The records include deleted
    text in PLAIN TEXT (not encryption). Do not leave sensitive historical
    content there unless that is acceptable to you. Limits: 128 MiB of
    history or 100,000 transactions; no age-based cleanup. When a file
    changes between sessions, a valid journal records the outside difference
    as one undo operation. If a redo branch must be discarded, the old
    journal is archived. A confirmed overwrite during an editing session
    preserves the outside content as an undoable state; the older editing
    history remains available beyond it. Unsaved edits do not survive
    a crash.

    The edited file's exact bytes, mixed LF/CRLF line endings and the
    presence or absence of a final newline survive ordinary operations
    unless your edits change them. New Enter characters use the first
    newline style found in the file, or LF by default. NUL bytes are refused.

PASTING FROM THE TERMINAL
    An incomplete paste is discarded after five seconds without data. The
    editor displays a waiting-for-end-marker notice and keeps the document
    and command draft intact. Delayed paste bytes cannot become commands.
    If the terminal lost its end marker, make a small fresh bracketed paste
    to supply one; that paste is discarded too. Then paste the wanted text
    again. A complete paste still uses one undo step.

    Terminal paste recognizes CR, LF and CRLF line breaks and inserts each
    using the document's current newline style (the first newline found
    when opened, or LF for a new file). A CRLF pair produces one line break,
    not two. CRLF pairs split across terminal reads are handled together.
    Blank lines, indentation, tabs, UTF-8 and an absent final newline are
    retained. The two literal characters ^M are not changed.

    One complete terminal paste is one undoable change. Conversion happens
    while collecting the paste, without a second full-sized buffer. The
    document limit applies after conversion; a paste that would exceed it
    is rejected without partly inserting the text.

    This applies only to bracketed paste received from the terminal, including
    when editing a clipboard. Files opened with read, piped input, and the
    editor's paste [SLOT] command remain byte-preserving. Existing document
    bytes are not normalized or repaired automatically. Multiline pastes at
    Command> and argument prompts remain rejected without execution.

    This fixes the earlier behavior that saved CR-only terminal newlines
    as visible ^M characters on one long line. Undo a bad paste or remove
    that inserted block, then paste the original text again with this version.
    Simply hiding ^M or deleting CR bytes would join lines, not fix them.

TERMINAL AND LIMITATIONS
    VT/xterm-compatible terminal, Unix, and a system C compiler for the
    build. No ncurses. Tested on Linux; OpenBSD SIGWINCH compile fix is
    included but a native OpenBSD test has not been performed here.
    Complex Unicode grapheme/shaping support is outside scope; cursor moves
    through UTF-8 code points. Long lines scroll horizontally. One document
    at a time. At most 8 MiB of input. No syntax highlighting, tabs-as-open
    documents. Literal find/replace is supported. The built-in clipboard is a separate
    private plaintext file, not the desktop clipboard.

ABOUT
    editor 0.6.14
    design by remfan1994
    published by ThatIAm.vip

    BLOODGUILT CURSE
    Cows aren't killers, and beef is the bloodshed of the innocent.
    www.bloodguiltcurse.net has the Bible study

RELIABILITY, FAILURE HANDLING, AND HISTORY ARCHIVES
    Failed creation of a staging file never removes a filename that editor
    did not successfully create. Cleanup retains the original error instead
    of replacing it with a secondary cleanup error. Drafts and existing
    destinations remain intact when that preparation fails.

    Staged saves check pending interrupt, suspend and termination requests
    before publishing the replacement, including after successful file sync.
    With opt-in interrupt cancellation, retry can use the same draft after
    returning to the editor. Default native interruption still exits and loses
    unsaved changes. A completed write is never rolled back merely because a
    later signal, journal save or directory synchronization fails. Signals and
    concurrent writers can still arrive after the final check.

    An unchanged write verifies disk contents without replacing the file or
    changing its modification time. Changed undo/redo positions can be saved
    without rewriting the document. Repeated unchanged writes skip work.

    Journal updates are staged. A failed update leaves the previous journal
    and current in-memory history available; it does not reverse a successful
    document save. A separately saved journal from another editor is archived
    before replacement, even when the document bytes have not changed.
    If a journal changes while a replacement is being staged, the update is
    refused rather than replacing the intervening version. A later write can
    retry. This is preservation of separate histories, not automatic merging.

    Archives use unique .stale.* names. Reverse traverses the active chain
    only; archived branches still require separate/manual recovery. Archives
    are not silently pruned and may grow without bound. Journals retain their
    128 MiB / 100,000 transaction limits. Monitor storage and keep backups.
    The final check-to-replacement race is not eliminated; coordinate editors
    that write shared files. No advisory locking protocol is introduced.

    EDUNDO02 journals include an FNV-1a checksum for accidental corruption,
    plus checks on document lengths, offsets and cursors. A changed journal
    is refused if its metadata changes during the read. This is not encryption
    or authentication. Legacy EDUNDO01 journals remain readable but lack a
    checksum; a successful history save upgrades them. Plausible damage in a
    legacy journal cannot always be detected. Keep independent file backups.

    Counted undo/redo checks pending interrupt, suspend and termination signals
    between transactions. Completed steps remain valid and can be reversed.
    This does not install a global Ctrl-key command. Mouse entry into the menu
    cancels any obsolete filename/search/replacement prompt. Failed clipboard
    saves stay open with the draft intact, and the footer displays the error.
    Clipboard metadata is not an editable clipboard slot or a listed clip.

    Failed staged writes and rejected clipboard reads release their resources.
    With signal_controls=on, SIGINT cancels filename entry completely. Cancelled bracketed-paste bytes
    are discarded through their end marker even after an idle timeout. A
    visible notice appears after five seconds without data; late input remains
    part of the discarded paste. Ordinary unbracketed paste cannot reliably
    be distinguished from typing. Partial screen output invalidates the cached
    display; the next successful redraw repaints it. Viewer wrapping counts
    display columns, preserving tabs and complete UTF-8 code points.

    A named file takes priority over piped stdin, with the controlling terminal
    used for interaction. Interrupted pipe output may already be partial;
    retrying write - can repeat a prefix received downstream. Inherited input
    conversion and flow-control flags are restored when the editor exits.

    OS file-size limits are handled as write failures instead of fatal signals.
    A failed staged write retains the original file and the editing buffer.
    If only undo-journal persistence exceeds a limit, a successful document
    save remains successful and its history warning is shown.

    A display allocation failure drops the expendable screen cache and shows
    a minimal low-memory message without discarding the document or its undo
    history. A later input or resize can retry drawing. This is not protection
    from the operating system killing the process or all possible OOM paths.

    Private journal reads/writes use fixed-size buffers without changing the
    EDUNDO02 format. Clipboard-history listings read only short previews;
    restoring an entry still validates its complete contents. If optional
    archival of the previous clip fails, copy/cut still reports that warning
    after a successful clipboard update. History failure is not silently
    converted into an unqualified success message.

    Terminal parity marking, input conversions, and newline echo are disabled
    while active and restored on exit. Malformed mouse coordinates, including
    oversized decimal values, are ignored rather than wrapped into clicks.

    Unsaved changes still do not survive process termination. There is no
    guarantee against all bugs, remote-filesystem failures or power loss.
    VALIDATION.txt lists checks actually run for this release. tests/ contains
    reproducible development checks; CHANGES.txt retains release history.

SIGNAL REVIEW IN 0.6.5
    The prior version disabled terminal signal generation and interpreted
    Ctrl+C/Z bytes differently in several input screens. This release removes
    those special byte actions, reserves the two keys from command bindings,
    and preserves terminal signal characters/ISIG across the whole interface.
    Native suspension is tested under a real foreground job-control shell,
    including filename prompts and remapped interrupt/suspend characters.
    Terminal restoration retries EINTR; an interrupt during resume follows
    the signal path rather than becoming a fatal terminal-start I/O error.
    See VALIDATION.txt and tests/signals_terminal.py for reproducible checks.
