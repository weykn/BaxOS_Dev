## Screen
- **help**                     show this help
- **echo** `<text>`              print text
- **clear**                    clear the screen
- **mode** `[size]`              list or set screen sizes, e.g. `mode 1024x768`
- **font** `[size]`              list or set text sizes, e.g. `font 16`
- **set-bg** `<file> [alpha]`    wallpaper from a PNG, e.g. `set-bg cool.png 0.5`

## Files
- **ls** `[folder]...`           list a folder, with sizes
- **cd** `[folder]`              change folder; `cd` alone goes to `/`
- **mkdir** `<folder>...`        create folders
- **cat** `<file>...`            print files
- **cp** `<file> <file|folder>`  copy a file
- **mv** `<file> <file|folder>`  move or rename a file
- **rm** `<name>...`             delete files, and empty folders
- **md** `<file>`                read a markdown file
- **more** `[file|command]`      a screenful at a time, e.g. `more log`
- **file** `<file>`              describe a file
- **write** `<file> [text]`      create or replace a file

## Programs
- **run** `<file> [args]`        run a program, e.g. `run /home/hello_world`
- **sh** `<file>`                run a script, one command per line
- **path** `[add|remove folder]` list or change where commands are looked for
- **remap** `[folder folder]`    one folder standing in for another, e.g. `remap /etc /conf/sys`
- **set-default** `[cmd ext]`    open files by extension, e.g. `set-default md md;markdown`
- **log** `[on|off|clear]`       syscalls programs made

## System
- **baxfetch**                 what this machine is, at a glance
- **uptime**                   how long since the machine was switched on
- **mem**                      memory use
- **disk**                     disk use
- **reboot**                   restart the machine
- **poweroff**                 switch the machine off
