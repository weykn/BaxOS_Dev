# Run by bash when it starts. This is the machine's shell, so this file is
# where the prompt, the path and anything you want every session to have go.

PATH=/proc:/pkg/linux-coreutils
PS1='\[\e[36m\]\w\[\e[0m\] $ '
HISTFILE=/home/.bash_history

alias ll='ls -l'
alias la='ls -la'
alias ls='ls --color=never'
