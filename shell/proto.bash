[[ $- == *i* ]] || return 0
if (( BASH_VERSINFO[0] < 4 )); then
    printf '%s\n' 'proto: Bash 4+ is required; use a newer Bash or the Zsh integration.' >&2
    return 1
fi
[[ ${_PROTO_BASH_LOADED:-0} == 1 ]] && return 0

_proto_script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
if [[ -z ${PROTO_BIN:-} ]]; then
    PROTO_BIN=$(type -P proto)
    [[ -n $PROTO_BIN ]] || PROTO_BIN="$_proto_script_directory/../build/proto"
fi
if [[ ! -x $PROTO_BIN ]]; then
    printf '%s\n' 'proto: set PROTO_BIN to the built or installed proto executable.' >&2
    return 1
fi
PROTO_BIN=$(cd -- "$(dirname -- "$PROTO_BIN")" && printf '%s/%s' "$PWD" "$(basename -- "$PROTO_BIN")")

proto-plan() {
    local script result
    script=$("$PROTO_BIN" --approve-plan "$*")
    result=$?
    (( result == 0 )) || return "$result"
    ( eval "$script" )
}

_proto_bash_binding() {
    local map=$1 key=$2 line
    while IFS= read -r line; do
        if [[ $line == "\"$key\":"* ]]; then
            printf 'normal\t%s' "$line"
            return
        fi
    done < <(builtin bind -m "$map" -p; builtin bind -m "$map" -s)
    while IFS= read -r line; do
        if [[ $line == "\"$key\":"* ]]; then
            printf 'x\t%s' "$line"
            return
        fi
    done < <(builtin bind -m "$map" -X)
}

_proto_bash_restore() {
    local map=$1 key=$2 value=$3 kind line
    if [[ -z $value ]]; then
        builtin bind -m "$map" -r "$key"
    else
        kind=${value%%$'\t'*}
        line=${value#*$'\t'}
        if [[ $kind == x ]]; then
            builtin bind -m "$map" -x "$line"
        else
            builtin bind -m "$map" "$line"
        fi
    fi
}

_proto_bash_suggest() {
    local LC_ALL=C record command risk
    [[ $READLINE_POINT == ${#READLINE_LINE} ]] || return
    _proto_bash_prefix=$READLINE_LINE
    _proto_bash_commands=()
    _proto_bash_risks=()
    while IFS= read -r record; do
        risk=${record%%$'\t'*}
        command=${record#*$'\t'}
        if [[ $command == "$_proto_bash_prefix"* && $command != "$_proto_bash_prefix" ]]; then
            _proto_bash_commands+=("$command")
            _proto_bash_risks+=("$risk")
        fi
    done < <("$PROTO_BIN" --suggest-shell "$_proto_bash_prefix" 2>/dev/null)
    _proto_bash_index=0
    _proto_bash_choose
}

_proto_bash_choose() {
    local LC_ALL=C
    (( ${#_proto_bash_commands[@]} > 0 )) || return
    READLINE_LINE=${_proto_bash_commands[_proto_bash_index]}
    READLINE_POINT=${#READLINE_LINE}
    printf '\nproto: variant %d/%d · %s · review before Enter\n' \
        "$((_proto_bash_index + 1))" "${#_proto_bash_commands[@]}" "${_proto_bash_risks[_proto_bash_index]}" >&2
}

_proto_bash_next() {
    if [[ ${_proto_bash_commands[_proto_bash_index]:-} != "$READLINE_LINE" ]]; then
        _proto_bash_suggest
    elif (( ${#_proto_bash_commands[@]} > 0 )); then
        _proto_bash_index=$(((_proto_bash_index + 1) % ${#_proto_bash_commands[@]}))
        _proto_bash_choose
    fi
}

_proto_bash_previous() {
    if [[ ${_proto_bash_commands[_proto_bash_index]:-} != "$READLINE_LINE" ]]; then
        _proto_bash_suggest
    elif (( ${#_proto_bash_commands[@]} > 0 )); then
        _proto_bash_index=$(((_proto_bash_index + ${#_proto_bash_commands[@]} - 1) % ${#_proto_bash_commands[@]}))
        _proto_bash_choose
    fi
}

_proto_bash_menu() {
    local recalled
    recalled=$("$PROTO_BIN" --menu)
    if [[ -n $recalled ]]; then
        READLINE_LINE=$recalled
        local LC_ALL=C
        READLINE_POINT=${#READLINE_LINE}
    fi
}

_proto_bash_plan() {
    [[ $READLINE_LINE == '??'* ]] || READLINE_LINE="?? $READLINE_LINE"
    local LC_ALL=C
    READLINE_POINT=${#READLINE_LINE}
}

_proto_bash_prepare() {
    local script result
    if [[ $READLINE_LINE == '??'* ]]; then
        script=$("$PROTO_BIN" --approve-plan "${READLINE_LINE:2}")
        result=$?
        if (( result == 0 )); then
            READLINE_LINE=$'(\n'"$script"$'\n)'
        else
            READLINE_LINE=''
        fi
    elif ! "$PROTO_BIN" --confirm "$READLINE_LINE"; then
        READLINE_LINE=''
    else
        "$PROTO_BIN" --record "$READLINE_LINE" >/dev/null 2>&1
    fi
    local LC_ALL=C
    READLINE_POINT=${#READLINE_LINE}
}

proto-enable() {
    [[ ${_PROTO_BASH_LOADED:-0} == 1 ]] && return 0
    local module map key index action enter binding
    if [[ ${PROTO_BASH_MODULE+x} ]]; then
        module=$PROTO_BASH_MODULE
    elif [[ -f $(dirname -- "$PROTO_BIN")/proto-bash.so ]]; then
        module="$(dirname -- "$PROTO_BIN")/proto-bash.so"
    else
        module="$_proto_script_directory/../../lib/proto/proto-bash.so"
    fi
    _proto_bash_native=0
    if [[ -n $module && -f $module ]] && builtin enable -f "$module" proto_hook 2>/dev/null; then
        if proto_hook on "$PROTO_BIN"; then
            _proto_bash_native=1
            _PROTO_BASH_LOADED=1
            return 0
        fi
    fi
    _proto_bash_keys=('\C-x\C-a' '\e[1;3A' '\e[1;3B' '\e,' '\e.' '\eOQ' '\e[12~' '\C-@' '\C-g' '\C-m' '\C-j' '\C-x\C-v' '\C-x\C-j')
    _proto_bash_maps=()
    _proto_bash_saved=()
    _proto_bash_installed=()
    _proto_bash_commands=()
    _proto_bash_risks=()
    _proto_bash_index=0
    for map in emacs-standard vi-insertion; do
        for key in "${_proto_bash_keys[@]}"; do
            _proto_bash_maps+=("$map")
            _proto_bash_saved+=("$(_proto_bash_binding "$map" "$key")")
        done
        enter=$(_proto_bash_binding "$map" '\C-m')
        if [[ -z $enter ]]; then
            builtin bind -m "$map" '"\C-x\C-j": accept-line'
        else
            binding=${enter#*$'\t'}
            action=${binding#*: }
            if [[ ${enter%%$'\t'*} == x ]]; then
                builtin bind -m "$map" -x "\"\\C-x\\C-j\": $action"
            else
                builtin bind -m "$map" "\"\\C-x\\C-j\": $action"
            fi
        fi
        builtin bind -m "$map" -x '"\C-x\C-a": _proto_bash_suggest'
        for key in '\e[1;3A' '\e,'; do
            builtin bind -m "$map" -x "\"$key\": _proto_bash_previous"
        done
        for key in '\e[1;3B' '\e.'; do
            builtin bind -m "$map" -x "\"$key\": _proto_bash_next"
        done
        for key in '\eOQ' '\e[12~' '\C-@'; do
            builtin bind -m "$map" -x "\"$key\": _proto_bash_menu"
        done
        builtin bind -m "$map" -x '"\C-g": _proto_bash_plan'
        builtin bind -m "$map" -x '"\C-x\C-v": _proto_bash_prepare'
        for key in '\C-m' '\C-j'; do
            builtin bind -m "$map" "\"$key\": \"\\C-x\\C-v\\C-x\\C-j\""
        done
    done
    for ((index = 0; index < ${#_proto_bash_maps[@]}; ++index)); do
        key=${_proto_bash_keys[index % ${#_proto_bash_keys[@]}]}
        _proto_bash_installed+=("$(_proto_bash_binding "${_proto_bash_maps[index]}" "$key")")
    done
    _PROTO_BASH_LOADED=1
    printf '%s\n' 'proto: on-demand Bash mode (Ctrl+X Ctrl+A); build the Bash module for inline suggestions.' >&2
}

proto-disable() {
    [[ ${_PROTO_BASH_LOADED:-0} == 1 ]] || return 0
    local index key map current
    if (( _proto_bash_native )); then
        proto_hook off
    else
        for ((index = ${#_proto_bash_maps[@]} - 1; index >= 0; --index)); do
            key=${_proto_bash_keys[index % ${#_proto_bash_keys[@]}]}
            map=${_proto_bash_maps[index]}
            current=$(_proto_bash_binding "$map" "$key")
            if [[ $current == "${_proto_bash_installed[index]}" ]]; then
                _proto_bash_restore "$map" "$key" "${_proto_bash_saved[index]}"
            fi
        done
    fi
    _PROTO_BASH_LOADED=0
}

proto-enable
