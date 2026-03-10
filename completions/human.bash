# Bash completion for human
# Source with: source completions/human.bash
# Or install to: /etc/bash_completion.d/human (or ~/.local/share/bash-completion/completions/human)

_human() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]:-}"

    # Global flags (before or as first argument)
    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=($(compgen -W '--version -v --help -h --mcp agent gateway mcp service service-loop status onboard init doctor cron channel skills hardware migrate memory workspace capabilities models auth update paperclip version help persona sandbox' -- "$cur"))
        return
    fi

    local cmd="${COMP_WORDS[1]}"

    case "$cmd" in
        cron)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list add remove' -- "$cur"))
            fi
            ;;
        channel)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list status start stop' -- "$cur"))
            fi
            ;;
        hardware)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list info scan flash monitor' -- "$cur"))
            fi
            ;;
        memory)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'stats count list search get forget' -- "$cur"))
            fi
            ;;
        workspace)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'show set' -- "$cur"))
            fi
            ;;
        models)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list info benchmark' -- "$cur"))
            fi
            ;;
        auth)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'login status logout' -- "$cur"))
            fi
            ;;
        skills)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list search install uninstall update info publish' -- "$cur"))
            fi
            ;;
        migrate)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'sqlite markdown --dry-run' -- "$cur"))
            fi
            ;;
        capabilities)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W '--json' -- "$cur"))
            fi
            ;;
        update)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W '--check' -- "$cur"))
            fi
            ;;
        persona)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'create show list delete validate feedback export import merge' -- "$cur"))
            fi
            ;;
        sandbox)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'list status' -- "$cur"))
            fi
            ;;
        paperclip)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=($(compgen -W 'heartbeat' -- "$cur"))
            fi
            ;;
        *)
            COMPREPLY=()
            ;;
    esac
}

complete -F _human human
