# Bash completion for seaclaw
# Source with: source completions/seaclaw.bash
# Or install to: /etc/bash_completion.d/seaclaw (or ~/.local/share/bash-completion/completions/seaclaw)

_seaclaw() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]:-}"

    # Global flags (before or as first argument)
    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=($(compgen -W '--version -v --help -h --mcp agent gateway mcp service service-loop status onboard doctor cron channel skills hardware migrate memory workspace capabilities models auth update version help' -- "$cur"))
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
        *)
            COMPREPLY=()
            ;;
    esac
}

complete -F _seaclaw seaclaw
