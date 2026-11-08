[[ -o interactive ]] || return 0
[[ ${_PROTO_ZSH_LOADED:-0} == 1 ]] && return 0
autoload -Uz is-at-least add-zle-hook-widget
if ! is-at-least 5.9; then
    print -u2 -r -- 'proto: Zsh 5.9+ is required.'
    return 1
fi
zmodload zsh/system || return 1
zmodload zsh/zselect || return 1

typeset -g _proto_zsh_directory=${${(%):-%x}:A:h}
typeset -g PROTO_BIN=${PROTO_BIN:-${commands[proto]:-$_proto_zsh_directory/../build/proto}}
PROTO_BIN=${PROTO_BIN:A}
if [[ ! -x $PROTO_BIN ]]; then
    print -u2 -r -- 'proto: set PROTO_BIN to the built or installed proto executable.'
    return 1
fi
typeset -g _proto_zsh_shell=${commands[zsh]:-${SHELL:-/bin/zsh}}
typeset -ga _proto_zsh_options=() _proto_zsh_commands=() _proto_zsh_risks=() _proto_zsh_widgets=()
typeset -gA _proto_zsh_settings _proto_zsh_maps _proto_zsh_keys _proto_zsh_actions
typeset -gA _proto_zsh_originals _proto_zsh_aliases _proto_zsh_macros _proto_zsh_kinds
typeset -gi _proto_zsh_fd=-1 _proto_zsh_pid=0 _proto_zsh_cursor=-1 _proto_zsh_index=1
typeset -gi _proto_zsh_suppressed=0
typeset -gi _proto_zsh_generation=0
typeset -gi _proto_zsh_history_event=-1
typeset -g _proto_zsh_prefix='' _proto_zsh_cwd='' _proto_zsh_ghost='' _proto_zsh_foreign=''

_proto_zsh_cli() {
    command env "PROTO_SHELL=$_proto_zsh_shell" "${_proto_zsh_options[@]}" "$PROTO_BIN" "$@"
}

_proto_zsh_apply_settings() {
    emulate -L zsh
    local record rest name value
    _proto_zsh_options=()
    _proto_zsh_settings=()
    while IFS= read -r record; do
        [[ $record == $'setting\t'* ]] || continue
        rest=${record#*$'\t'}
        name=${rest%%$'\t'*}
        value=${rest#*$'\t'}
        case $name in
            PROTO_API_BASE|PROTO_MODEL|PROTO_AI|PROTO_CONTEXT|PROTO_SUGGESTIONS|PROTO_MAX_ENTRIES|PROTO_DEBOUNCE_MS|PROTO_TIMEOUT_MS|PROTO_NO_HISTORY|PROTO_SEND_HISTORY|PROTO_THEME)
                _proto_zsh_options+=("$name=$value")
                _proto_zsh_settings[$name]=$value ;;
        esac
    done <<< "$1"
}

_proto_zsh_cancel() {
    emulate -L zsh
    if (( _proto_zsh_fd >= 0 )); then
        zle -F "$_proto_zsh_fd" 2>/dev/null
        exec {_proto_zsh_fd}<&-
        _proto_zsh_fd=-1
    fi
    if (( _proto_zsh_pid > 0 )); then
        kill -TERM "$_proto_zsh_pid" 2>/dev/null
        _proto_zsh_pid=0
    fi
}

_proto_zsh_clear_display() {
    emulate -L zsh
    if [[ $POSTDISPLAY == "$_proto_zsh_ghost" ]]; then
        POSTDISPLAY=$_proto_zsh_foreign
    fi
    _proto_zsh_ghost=''
    region_highlight=("${(@)region_highlight:#*memo=proto}")
}

_proto_zsh_render() {
    emulate -L zsh
    local command=${_proto_zsh_commands[_proto_zsh_index]:-} risk color=8
    if [[ $POSTDISPLAY != "$_proto_zsh_ghost" ]]; then
        _proto_zsh_foreign=$POSTDISPLAY
    fi
    _proto_zsh_clear_display
    if (( _proto_zsh_suppressed || CURSOR != ${#BUFFER} )) ||
       [[ -z $command || $command != "$BUFFER"* || $command == "$BUFFER" ]]; then
        return
    fi
    POSTDISPLAY=${command:${#BUFFER}}
    risk=${_proto_zsh_risks[_proto_zsh_index]}
    if (( ${#_proto_zsh_commands} > 1 )) || [[ $risk != none ]]; then
        POSTDISPLAY+=" [$_proto_zsh_index/${#_proto_zsh_commands}"
        [[ $risk == none ]] || POSTDISPLAY+=" $risk"
        POSTDISPLAY+=']'
    fi
    [[ $risk == caution ]] && color=yellow
    [[ $risk == danger ]] && color=red
    _proto_zsh_ghost=$POSTDISPLAY
    region_highlight+=("${#BUFFER} $((${#BUFFER} + ${#POSTDISPLAY})) fg=$color memo=proto")
}

_proto_zsh_ready() {
    emulate -L zsh
    [[ $1 == "$_proto_zsh_fd" ]] || return
    local record command risk previous=${_proto_zsh_commands[_proto_zsh_index]:-}
    local -a candidates=() risks=()
    while IFS= read -r record <&$_proto_zsh_fd; do
        if [[ $record == $'batch-end\t'* ]]; then
            if (( ${#candidates} > 0 )); then
                _proto_zsh_commands=("${candidates[@]}")
                _proto_zsh_risks=("${risks[@]}")
                _proto_zsh_index=${_proto_zsh_commands[(Ie)$previous]}
                (( _proto_zsh_index > 0 )) || _proto_zsh_index=1
            fi
            if [[ $record == $'batch-end\tai' || ${_proto_zsh_settings[PROTO_AI]} == 0 ]]; then
                _proto_zsh_pid=0
                _proto_zsh_cancel
            fi
            _proto_zsh_render
            zle -R
            return
        fi
        risk=${record%%$'\t'*}
        command=${record#*$'\t'}
        if [[ $risk == (none|caution|danger) && $command == "$_proto_zsh_prefix"* &&
              $command != "$_proto_zsh_prefix" ]] && (( ${#candidates} < 5 )); then
            candidates+=("$command")
            risks+=("$risk")
        fi
    done
    _proto_zsh_pid=0
    _proto_zsh_cancel
}

_proto_zsh_receive() {
    emulate -L zsh
    # Async replies must not replace LASTWIDGET.
    zle _proto_zsh_ready -f nolast -Nw -- "$1"
}

_proto_zsh_schedule() {
    emulate -L zsh
    setopt localoptions no_monitor
    local prefix=$BUFFER
    local -i delay=$((${_proto_zsh_settings[PROTO_DEBOUNCE_MS]:-250} / 10))
    exec {_proto_zsh_fd}< <(
        local -i child=0
        TRAPTERM() {
            (( child > 0 )) && kill -TERM "$child" 2>/dev/null
            exit 143
        }
        TRAPHUP() { TRAPTERM; }
        print -r -- "$sysparams[pid]"
        (( delay > 0 )) && zselect -t "$delay" 2>/dev/null
        command env "PROTO_SHELL=$_proto_zsh_shell" "${_proto_zsh_options[@]}" \
            "$PROTO_BIN" --no-ai --suggest-shell "$prefix" 2>/dev/null &
        child=$!
        wait "$child"
        child=0
        print -r -- $'batch-end\tlocal'
        if [[ ${_proto_zsh_settings[PROTO_AI]} == 1 ]]; then
            command env "PROTO_SHELL=$_proto_zsh_shell" "${_proto_zsh_options[@]}" \
                "$PROTO_BIN" --suggest-shell "$prefix" 2>/dev/null &
            child=$!
            wait "$child"
            child=0
            print -r -- $'batch-end\tai'
        fi
    )
    read -r _proto_zsh_pid <&$_proto_zsh_fd
    zle -F "$_proto_zsh_fd" _proto_zsh_receive
}

_proto_zsh_observe() {
    emulate -L zsh
    [[ ${_PROTO_ZSH_LOADED:-0} == 1 ]] || return
    local -i history_navigation=$(( HISTNO != _proto_zsh_history_event ))
    _proto_zsh_history_event=$HISTNO
    case $LASTWIDGET in
        *history*|*line-or-search|*line-or-beginning-search) history_navigation=1 ;;
    esac
    if [[ $BUFFER != "$_proto_zsh_prefix" || $CURSOR != "$_proto_zsh_cursor" || $PWD != "$_proto_zsh_cwd" ]] ||
       (( history_navigation )); then
        _proto_zsh_cancel
        local previous=${_proto_zsh_commands[_proto_zsh_index]:-}
        local -a candidates=() risks=()
        local -i index
        for ((index = 1; index <= ${#_proto_zsh_commands}; ++index)); do
            if [[ ${_proto_zsh_commands[index]} == "$BUFFER"* && ${_proto_zsh_commands[index]} != "$BUFFER" ]]; then
                candidates+=("${_proto_zsh_commands[index]}")
                risks+=("${_proto_zsh_risks[index]}")
            fi
        done
        if [[ $BUFFER != "$_proto_zsh_prefix" ]]; then
            _proto_zsh_suppressed=$history_navigation
        fi
        (( history_navigation )) && _proto_zsh_suppressed=1
        _proto_zsh_commands=("${candidates[@]}")
        _proto_zsh_risks=("${risks[@]}")
        _proto_zsh_index=${_proto_zsh_commands[(Ie)$previous]}
        (( _proto_zsh_index > 0 )) || _proto_zsh_index=1
        _proto_zsh_prefix=$BUFFER
        _proto_zsh_cursor=$CURSOR
        _proto_zsh_cwd=$PWD
        if (( PENDING == 0 && KEYS_QUEUED_COUNT == 0 && !_proto_zsh_suppressed &&
              CURSOR == ${#BUFFER} && ${#BUFFER} > 0 &&
              ${#BUFFER} <= 4096 && ${#candidates} == 0 )) && [[ $BUFFER != '??'* ]]; then
            _proto_zsh_schedule
        fi
    fi
    _proto_zsh_render
}

_proto_zsh_init() {
    emulate -L zsh
    _proto_zsh_cancel
    _proto_zsh_commands=()
    _proto_zsh_risks=()
    _proto_zsh_cursor=-1
    _proto_zsh_prefix=''
    _proto_zsh_history_event=$HISTNO
    _proto_zsh_suppressed=0
    _proto_zsh_ghost=''
    _proto_zsh_foreign=$POSTDISPLAY
}

_proto_zsh_finish() {
    emulate -L zsh
    _proto_zsh_cancel
    _proto_zsh_clear_display
}

_proto_zsh_resume() {
    emulate -L zsh
    _proto_zsh_cursor=-1
    if (( ${#zle_bracketed_paste} == 2 )); then
        print -rn -- "$zle_bracketed_paste[1]" > /dev/tty
    fi
    zle reset-prompt
}

_proto_zsh_original() {
    emulate -L zsh
    local widget=$1
    shift
    if [[ ${_proto_zsh_kinds[$widget]} == macro ]]; then
        local macro=${_proto_zsh_macros[$widget]}
        zle -U "${(g:ce:)macro}"
    else
        zle "${_proto_zsh_aliases[$widget]}" -- "$@"
    fi
}

_proto_zsh_accept() {
    emulate -L zsh
    local command=${_proto_zsh_commands[_proto_zsh_index]:-}
    if (( !_proto_zsh_suppressed && CURSOR == ${#BUFFER} )) &&
       [[ -n $command && $command == "$BUFFER"* && $command != "$BUFFER" ]]; then
        _proto_zsh_cancel
        _proto_zsh_clear_display
        BUFFER=$command
        CURSOR=${#BUFFER}
        _proto_zsh_commands=()
        _proto_zsh_risks=()
        _proto_zsh_prefix=$BUFFER
        _proto_zsh_cursor=$CURSOR
        _proto_zsh_suppressed=1
    else
        _proto_zsh_original "$@"
    fi
}

_proto_zsh_word() {
    emulate -L zsh
    local command=${_proto_zsh_commands[_proto_zsh_index]:-} suffix
    local -i length=1
    if (( !_proto_zsh_suppressed && CURSOR == ${#BUFFER} )) &&
       [[ -n $command && $command == "$BUFFER"* && $command != "$BUFFER" ]]; then
        suffix=${command:${#BUFFER}}
        while (( length <= ${#suffix} )) && [[ $suffix[length] == [[:space:]] ]]; do
            (( ++length ))
        done
        while (( length <= ${#suffix} )) && [[ $suffix[length] != [[:space:]] ]]; do
            (( ++length ))
        done
        LBUFFER+=${suffix[1,length-1]}
    else
        _proto_zsh_original "$@"
    fi
}

_proto_zsh_cycle() {
    emulate -L zsh
    (( CURSOR == ${#BUFFER} )) || return
    _proto_zsh_suppressed=0
    if (( ${#_proto_zsh_commands} > 0 )); then
        _proto_zsh_index=$(((_proto_zsh_index - 1 + $1 + ${#_proto_zsh_commands}) % ${#_proto_zsh_commands} + 1))
        _proto_zsh_render
    else
        _proto_zsh_cursor=-1
        _proto_zsh_observe
    fi
}

_proto_zsh_menu() {
    emulate -L zsh
    _proto_zsh_finish
    zle -I
    local output record
    output=$(_proto_zsh_cli --menu-shell)
    if (( $? == 0 )); then
        _proto_zsh_apply_settings "$output"
        while IFS= read -r record; do
            if [[ $record == $'recall\t'* ]]; then
                BUFFER=${record#*$'\t'}
                CURSOR=${#BUFFER}
            fi
        done <<< "$output"
    fi
    _proto_zsh_commands=()
    _proto_zsh_risks=()
    _proto_zsh_resume
}

_proto_zsh_plan() {
    emulate -L zsh
    setopt extendedglob
    local goal=${BUFFER#'??'} script
    goal=${goal##[[:space:]]#}
    if [[ -z $goal ]]; then
        zle -M 'proto: type a task, then press Ctrl+G'
        return 1
    fi
    _proto_zsh_finish
    zle -I
    script=$(_proto_zsh_cli --approve-plan "$goal")
    if (( $? == 0 )); then
        BUFFER=$'(\n'"$script"$'\n)'
        CURSOR=${#BUFFER}
        zle .accept-line
    else
        _proto_zsh_resume
    fi
}

_proto_zsh_submit() {
    emulate -L zsh
    if [[ $BUFFER == '??'* ]]; then
        _proto_zsh_plan
        return
    fi
    _proto_zsh_finish
    if _proto_zsh_cli --confirm "$BUFFER"; then
        _proto_zsh_cli --record "$BUFFER" >/dev/null 2>&1
        _proto_zsh_original "$@"
    else
        _proto_zsh_resume
    fi
}

_proto_zsh_dispatch() {
    emulate -L zsh
    local widget=$WIDGET
    case ${_proto_zsh_actions[$widget]} in
        accept) _proto_zsh_accept "$widget" "$@" ;;
        word) _proto_zsh_word "$widget" "$@" ;;
        next) _proto_zsh_cycle 1 ;;
        previous) _proto_zsh_cycle -1 ;;
        menu) _proto_zsh_menu ;;
        plan) _proto_zsh_plan ;;
        submit) _proto_zsh_submit "$widget" "$@" ;;
        dismiss) _proto_zsh_finish; _proto_zsh_suppressed=1 ;;
        request) _proto_zsh_commands=(); _proto_zsh_suppressed=0; _proto_zsh_cursor=-1; _proto_zsh_observe ;;
    esac
}

proto-plan() {
    emulate -L zsh
    local script
    script=$(_proto_zsh_cli --approve-plan "$*") || return $?
    ( eval "$script" )
}

proto-enable() {
    emulate -L zsh
    [[ ${_PROTO_ZSH_LOADED:-0} == 1 ]] && return 0
    local settings map key widget binding original
    local -a keys=('^I' '^[[C' '^[OC' '^[[1;5C' '^[[1;3C' '^[[1;3A' '^[,' '^[[1;3B' '^[.' '^[OQ' '^[[12~' '^@' '^G' '^M' '^J' '^[^[' '^]' '^X^A')
    local -a actions=(accept accept accept word word previous previous next next menu menu menu plan submit submit dismiss dismiss request) parts
    local -i index=0 position
    _proto_zsh_options=()
    settings=$(_proto_zsh_cli --settings-shell) || return $?
    _proto_zsh_apply_settings "$settings"
    _proto_zsh_widgets=()
    _proto_zsh_maps=()
    _proto_zsh_keys=()
    _proto_zsh_actions=()
    _proto_zsh_originals=()
    _proto_zsh_aliases=()
    _proto_zsh_macros=()
    _proto_zsh_kinds=()
    (( ++_proto_zsh_generation ))
    zle -N _proto_zsh_ready
    for map in emacs viins vicmd; do
        for ((position = 1; position <= ${#keys}; ++position)); do
            key=${keys[position]}
            widget="_proto-${sysparams[pid]}-$_proto_zsh_generation-$((++index))"
            binding=$(bindkey -L -M "$map" "$key")
            parts=("${(@z)binding}")
            original=${(Q)parts[-1]}
            _proto_zsh_maps[$widget]=$map
            _proto_zsh_keys[$widget]=$key
            _proto_zsh_actions[$widget]=${actions[position]}
            _proto_zsh_originals[$widget]=$original
            if [[ $parts[2] == -s ]]; then
                _proto_zsh_kinds[$widget]=macro
                _proto_zsh_macros[$widget]=$original
            else
                _proto_zsh_kinds[$widget]=widget
                _proto_zsh_aliases[$widget]="$widget-original"
                zle -A "$original" "${_proto_zsh_aliases[$widget]}"
            fi
            zle -N "$widget" _proto_zsh_dispatch
            bindkey -M "$map" "$key" "$widget"
            _proto_zsh_widgets+=("$widget")
        done
    done
    add-zle-hook-widget line-init _proto_zsh_init
    add-zle-hook-widget line-pre-redraw _proto_zsh_observe
    add-zle-hook-widget line-finish _proto_zsh_finish
    _PROTO_ZSH_LOADED=1
}

proto-disable() {
    emulate -L zsh
    [[ ${_PROTO_ZSH_LOADED:-0} == 1 ]] || return 0
    local widget binding
    local -a parts
    _PROTO_ZSH_LOADED=0
    _proto_zsh_finish
    add-zle-hook-widget -d line-init _proto_zsh_init
    add-zle-hook-widget -d line-pre-redraw _proto_zsh_observe
    add-zle-hook-widget -d line-finish _proto_zsh_finish
    for widget in "${_proto_zsh_widgets[@]}"; do
        binding=$(bindkey -L -M "${_proto_zsh_maps[$widget]}" "${_proto_zsh_keys[$widget]}")
        parts=("${(@z)binding}")
        if [[ ${(Q)parts[-1]} == "$widget" ]]; then
            if [[ ${_proto_zsh_kinds[$widget]} == macro ]]; then
                bindkey -s -M "${_proto_zsh_maps[$widget]}" "${_proto_zsh_keys[$widget]}" "${_proto_zsh_originals[$widget]}"
            else
                bindkey -M "${_proto_zsh_maps[$widget]}" "${_proto_zsh_keys[$widget]}" "${_proto_zsh_originals[$widget]}"
            fi
        fi
        binding=$(zle -l -L "$widget")
        parts=("${(@z)binding}")
        [[ ${(Q)parts[-1]} != _proto_zsh_dispatch ]] || zle -D "$widget"
        [[ -z ${_proto_zsh_aliases[$widget]:-} ]] || zle -D "${_proto_zsh_aliases[$widget]}"
    done
    _proto_zsh_widgets=()
    zle -D _proto_zsh_ready
}

proto-enable
